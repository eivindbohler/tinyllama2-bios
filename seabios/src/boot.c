// Code to load disk image and start system boot.
//
// Copyright (C) 2008-2013  Kevin O'Connor <kevin@koconnor.net>
// Copyright (C) 2002  MandrakeSoft S.A.
//
// This file may be distributed under the terms of the GNU LGPLv3 license.

#include "block.h" // struct drive_s
#include "bregs.h" // struct bregs
#include "config.h" // CONFIG_*
#include "fw/paravirt.h" // qemu_cfg_show_boot_menu
#include "hw/pci.h" // pci_bdf_to_*
#include "hw/rtc.h" // rtc_read
#include "hw/usb.h" // struct usbdevice_s
#include "list.h" // hlist_node
#include "malloc.h" // free
#include "output.h" // dprintf
#include "romfile.h" // romfile_loadint
#include "std/disk.h" // struct mbr_s
#include "string.h" // memset
#include "util.h" // irqtimer_calc show_adjust_cpu_speed_menu
#include "bios_fonts.h"


/****************************************************************
 * Boot priority ordering
 ****************************************************************/

static char **Bootorder VARVERIFY32INIT;
static int BootorderCount;

static void
loadBootOrder(void)
{
    if (!CONFIG_BOOTORDER)
        return;

    char *f = romfile_loadfile("bootorder", NULL);
    if (!f)
        return;

    int i = 0;
    BootorderCount = 1;
    while (f[i]) {
        if (f[i] == '\n')
            BootorderCount++;
        i++;
    }
    Bootorder = malloc_tmphigh(BootorderCount*sizeof(char*));
    if (!Bootorder) {
        warn_noalloc();
        free(f);
        BootorderCount = 0;
        return;
    }

    dprintf(1, "boot order:\n");
    i = 0;
    do {
        Bootorder[i] = f;
        f = strchr(f, '\n');
        if (f)
            *(f++) = '\0';
        Bootorder[i] = nullTrailingSpace(Bootorder[i]);
        dprintf(1, "%d: %s\n", i+1, Bootorder[i]);
        i++;
    } while (f);
}

// See if 'str' starts with 'glob' - if glob contains an '*' character
// it will match any number of characters in str that aren't a '/' or
// the next glob character.
static char *
glob_prefix(const char *glob, const char *str)
{
    for (;;) {
        if (!*glob && (!*str || *str == '/'))
            return (char*)str;
        if (*glob == '*') {
            if (!*str || *str == '/' || *str == glob[1])
                glob++;
            else
                str++;
            continue;
        }
        if (*glob != *str)
            return NULL;
        glob++;
        str++;
    }
}

// Search the bootorder list for the given glob pattern.
static int
find_prio(const char *glob)
{
    dprintf(1, "Searching bootorder for: %s\n", glob);
    int i;
    for (i = 0; i < BootorderCount; i++)
        if (glob_prefix(glob, Bootorder[i]))
            return i+1;
    return -1;
}

#define FW_PCI_DOMAIN "/pci@i0cf8"

static char *
build_pci_path(char *buf, int max, const char *devname, struct pci_device *pci)
{
    // Build the string path of a bdf - for example: /pci@i0cf8/isa@1,2
    char *p = buf;
    if (pci->parent) {
        p = build_pci_path(p, max, "pci-bridge", pci->parent);
    } else {
        if (pci->rootbus)
            p += snprintf(p, max, "/pci-root@%x", pci->rootbus);
        p += snprintf(p, buf+max-p, "%s", FW_PCI_DOMAIN);
    }

    int dev = pci_bdf_to_dev(pci->bdf), fn = pci_bdf_to_fn(pci->bdf);
    p += snprintf(p, buf+max-p, "/%s@%x", devname, dev);
    if (fn)
        p += snprintf(p, buf+max-p, ",%x", fn);
    return p;
}

int bootprio_find_pci_device(struct pci_device *pci)
{
    if (CONFIG_CSM)
        return csm_bootprio_pci(pci);
    if (!CONFIG_BOOTORDER)
        return -1;
    // Find pci device - for example: /pci@i0cf8/ethernet@5
    char desc[256];
    build_pci_path(desc, sizeof(desc), "*", pci);
    return find_prio(desc);
}

int bootprio_find_scsi_device(struct pci_device *pci, int target, int lun)
{
    if (!CONFIG_BOOTORDER)
        return -1;
    if (!pci)
        // support only pci machine for now
        return -1;
    // Find scsi drive - for example: /pci@i0cf8/scsi@5/channel@0/disk@1,0
    char desc[256], *p;
    p = build_pci_path(desc, sizeof(desc), "*", pci);
    snprintf(p, desc+sizeof(desc)-p, "/*@0/*@%d,%d", target, lun);
    return find_prio(desc);
}

int bootprio_find_ata_device(struct pci_device *pci, int chanid, int slave)
{
    if (CONFIG_CSM)
        return csm_bootprio_ata(pci, chanid, slave);
    if (!CONFIG_BOOTORDER)
        return -1;
    if (!pci)
        // support only pci machine for now
        return -1;
    // Find ata drive - for example: /pci@i0cf8/ide@1,1/drive@1/disk@0
    char desc[256], *p;
    p = build_pci_path(desc, sizeof(desc), "*", pci);
    snprintf(p, desc+sizeof(desc)-p, "/drive@%x/disk@%x", chanid, slave);
    return find_prio(desc);
}

int bootprio_find_fdc_device(struct pci_device *pci, int port, int fdid)
{
    if (CONFIG_CSM)
        return csm_bootprio_fdc(pci, port, fdid);
    if (!CONFIG_BOOTORDER)
        return -1;
    if (!pci)
        // support only pci machine for now
        return -1;
    // Find floppy - for example: /pci@i0cf8/isa@1/fdc@03f1/floppy@0
    char desc[256], *p;
    p = build_pci_path(desc, sizeof(desc), "isa", pci);
    snprintf(p, desc+sizeof(desc)-p, "/fdc@%04x/floppy@%x", port, fdid);
    return find_prio(desc);
}

int bootprio_find_pci_rom(struct pci_device *pci, int instance)
{
    if (!CONFIG_BOOTORDER)
        return -1;
    // Find pci rom - for example: /pci@i0cf8/scsi@3:rom2
    char desc[256], *p;
    p = build_pci_path(desc, sizeof(desc), "*", pci);
    if (instance)
        snprintf(p, desc+sizeof(desc)-p, ":rom%d", instance);
    return find_prio(desc);
}

int bootprio_find_named_rom(const char *name, int instance)
{
    if (!CONFIG_BOOTORDER)
        return -1;
    // Find named rom - for example: /rom@genroms/linuxboot.bin
    char desc[256], *p;
    p = desc + snprintf(desc, sizeof(desc), "/rom@%s", name);
    if (instance)
        snprintf(p, desc+sizeof(desc)-p, ":rom%d", instance);
    return find_prio(desc);
}

static char *
build_usb_path(char *buf, int max, struct usbhub_s *hub)
{
    if (!hub->usbdev)
        // Root hub - nothing to add.
        return buf;
    char *p = build_usb_path(buf, max, hub->usbdev->hub);
    p += snprintf(p, buf+max-p, "/hub@%x", hub->usbdev->port+1);
    return p;
}

int bootprio_find_usb(struct usbdevice_s *usbdev, int lun)
{
    if (!CONFIG_BOOTORDER)
        return -1;
    // Find usb - for example: /pci@i0cf8/usb@1,2/storage@1/channel@0/disk@0,0
    char desc[256], *p;
    p = build_pci_path(desc, sizeof(desc), "usb", usbdev->hub->cntl->pci);
    p = build_usb_path(p, desc+sizeof(desc)-p, usbdev->hub);
    snprintf(p, desc+sizeof(desc)-p, "/storage@%x/*@0/*@0,%d"
             , usbdev->port+1, lun);
    int ret = find_prio(desc);
    if (ret >= 0)
        return ret;
    // Try usb-host/redir - for example: /pci@i0cf8/usb@1,2/usb-host@1
    snprintf(p, desc+sizeof(desc)-p, "/usb-*@%x", usbdev->port+1);
    return find_prio(desc);
}


/****************************************************************
 * Boot setup
 ****************************************************************/

static int BootRetryTime;
static int CheckFloppySig = 1;

#define DEFAULT_PRIO           9999

static int DefaultFloppyPrio = 101;
static int DefaultCDPrio     = 102;
static int DefaultHDPrio     = 103;
static int DefaultBEVPrio    = 104;

void
boot_init(void)
{
    if (! CONFIG_BOOT)
        return;

    if (CONFIG_QEMU) {
        // On emulators, get boot order from nvram.
        if (rtc_read(CMOS_BIOS_BOOTFLAG1) & 1)
            CheckFloppySig = 0;
        u32 bootorder = (rtc_read(CMOS_BIOS_BOOTFLAG2)
                         | ((rtc_read(CMOS_BIOS_BOOTFLAG1) & 0xf0) << 4));
        DefaultFloppyPrio = DefaultCDPrio = DefaultHDPrio
            = DefaultBEVPrio = DEFAULT_PRIO;
        int i;
        for (i=101; i<104; i++) {
            u32 val = bootorder & 0x0f;
            bootorder >>= 4;
            switch (val) {
            case 1: DefaultFloppyPrio = i; break;
            case 2: DefaultHDPrio = i;     break;
            case 3: DefaultCDPrio = i;     break;
            case 4: DefaultBEVPrio = i;    break;
            }
        }
    }

    BootRetryTime = romfile_loadint("etc/boot-fail-wait", 60*1000);

    loadBootOrder();
}


/****************************************************************
 * BootList handling
 ****************************************************************/

struct bootentry_s {
    int type;
    union {
        u32 data;
        struct segoff_s vector;
        struct drive_s *drive;
    };
    int priority;
    const char *description;
    struct hlist_node node;
};
static struct hlist_head BootList VARVERIFY32INIT;

#define IPL_TYPE_FLOPPY      0x01
#define IPL_TYPE_HARDDISK    0x02
#define IPL_TYPE_CDROM       0x03
#define IPL_TYPE_CBFS        0x20
#define IPL_TYPE_BEV         0x80
#define IPL_TYPE_BCV         0x81
#define IPL_TYPE_HALT        0xf0

static void
bootentry_add(int type, int prio, u32 data, const char *desc)
{
    if (! CONFIG_BOOT)
        return;
    struct bootentry_s *be = malloc_tmp(sizeof(*be));
    if (!be) {
        warn_noalloc();
        return;
    }
    be->type = type;
    be->priority = prio;
    be->data = data;
    be->description = desc ?: "?";
    dprintf(3, "Registering bootable: %s (type:%d prio:%d data:%x)\n"
            , be->description, type, prio, data);

    // Add entry in sorted order.
    struct hlist_node **pprev;
    struct bootentry_s *pos;
    hlist_for_each_entry_pprev(pos, pprev, &BootList, node) {
        if (be->priority < pos->priority)
            break;
        if (be->priority > pos->priority)
            continue;
        if (be->type < pos->type)
            break;
        if (be->type > pos->type)
            continue;
        if (be->type <= IPL_TYPE_CDROM
            && (be->drive->type < pos->drive->type
                || (be->drive->type == pos->drive->type
                    && be->drive->cntl_id < pos->drive->cntl_id)))
            break;
    }
    hlist_add(&be->node, pprev);
}

// Return the given priority if it's set - defaultprio otherwise.
static inline int defPrio(int priority, int defaultprio) {
    return (priority < 0) ? defaultprio : priority;
}

// Add a BEV vector for a given pnp compatible option rom.
void
boot_add_bev(u16 seg, u16 bev, u16 desc, int prio)
{
    bootentry_add(IPL_TYPE_BEV, defPrio(prio, DefaultBEVPrio)
                  , SEGOFF(seg, bev).segoff
                  , desc ? MAKE_FLATPTR(seg, desc) : "Unknown");
    DefaultBEVPrio = DEFAULT_PRIO;
}

// Add a bcv entry for an expansion card harddrive or legacy option rom
void
boot_add_bcv(u16 seg, u16 ip, u16 desc, int prio)
{
    bootentry_add(IPL_TYPE_BCV, defPrio(prio, DefaultHDPrio)
                  , SEGOFF(seg, ip).segoff
                  , desc ? MAKE_FLATPTR(seg, desc) : "Legacy option rom");
}

void
boot_add_floppy(struct drive_s *drive_g, const char *desc, int prio)
{
    bootentry_add(IPL_TYPE_FLOPPY, defPrio(prio, DefaultFloppyPrio)
                  , (u32)drive_g, desc);
}

void
boot_add_hd(struct drive_s *drive_g, const char *desc, int prio)
{
    bootentry_add(IPL_TYPE_HARDDISK, defPrio(prio, DefaultHDPrio)
                  , (u32)drive_g, desc);
}

void
boot_add_cd(struct drive_s *drive_g, const char *desc, int prio)
{
    bootentry_add(IPL_TYPE_CDROM, defPrio(prio, DefaultCDPrio)
                  , (u32)drive_g, desc);
}

// Add a CBFS payload entry
void
boot_add_cbfs(void *data, const char *desc, int prio)
{
    bootentry_add(IPL_TYPE_CBFS, defPrio(prio, DEFAULT_PRIO), (u32)data, desc);
}


/****************************************************************
 * Boot menu and BCV execution
 ****************************************************************/

#define DEFAULT_BOOTMENU_WAIT 2500

// Show IPL option menu.
void
interactive_bootmenu(void)
{
    int cpu_freq = get_current_cpu_freq();
    u32 ram_size_bytes = GET_GLOBAL(LegacyRamSize);
    u32 ram_size_kb = ram_size_bytes / 1024;
    u32 ram_size_mb = ram_size_kb / 1024;
    u32 ram_size_mb_rest = ram_size_kb % 1024;
    if (ram_size_mb_rest > 0) {
        ram_size_mb += 1;
    }

    struct bios_settings s;
    memset(&s, 0, sizeof(s));
    load_bios_settings(&s);

    if (s.cache_enabled == 0) {
        setcr0(getcr0() | (CR0_CD|CR0_NW));
        wbinvd();
    }

    load_custom_fonts(VGA8_F16, 0x00, 256);

    if (s.boot_tune == 1) {
        play_mushroom_tune();
    } else if (s.boot_tune == 2) {
        play_ducks_tune();
    }

    // Set SBCLK
    outl(0x800038c0, 0x0cf8);
    u32 c0 = inl(0x0cfc);
    c0 |= (1 << 31); // C0h[31]
    // switch (s.isa_freq_index) {
    //     case 1:
    //     case 2:
    //     case 3:
    //         c0 |= (s.isa_freq_index << 14); // C0h[15:14]
    //         break;
    //     case 0:
    //     default:
    //         break;
    // }
    outl(c0, 0x0cfc); // write value

    // Set UART clocks/dividers
    if (s.com1_clock_index == 1) {
        u32 com1cntr = inl(0x0c00);
        com1cntr |= (1 << 22); // cntrl[22]
        if (s.com1_clock_ratio_index == 1) {
            com1cntr |= (1 << 20); // cntrl[20]
        }
        outl(com1cntr, 0x0c00);
    }
    if (s.com2_clock_index == 1) {
        u32 com2cntr = inl(0x0c04);
        com2cntr |= (1 << 22);  // cntrl[22]
        if (s.com2_clock_ratio_index == 1) {
            com2cntr |= (1 << 20);  // cntrl[20]
        }
        outl(com2cntr, 0x0c04);
    }

    printf("\n");
    printf("CPU: DMP Vortex86EX    ");
    if (cpu_freq < 100) {
        printf(" ");
    }
    printf("%lu MHz", (unsigned long int)cpu_freq);
    if (s.cache_enabled == 0) {
        printf(" [L1 Cache Disabled]\n");
    } else {
        printf("\n");
    }
    printf("RAM: DDR3             ");
    if (ram_size_mb < 1000) {
        printf(" ");
    }
    printf("%lu MB\n", (unsigned long int)ram_size_mb);

    while (get_keystroke(0) >= 0);

    int setup_menu_key = 0x3B; // F1
    int boot_menu_key = 0x86;  // F12
    printf("\nPress F1 for setup, F12 for boot menu\n");

    u32 menu_time = romfile_loadint("etc/boot-menu-wait", DEFAULT_BOOTMENU_WAIT);
    int scan_code = get_keystroke(menu_time);
    if (scan_code != setup_menu_key && scan_code != boot_menu_key)
        return;

    while (get_keystroke(0) >= 0);

    if (scan_code == setup_menu_key) {
        bios_setup_main(&s);
        return;
    }

    printf("\nBoot selection:\n\n");
    wait_threads();

    // Show menu items
    int maxmenu = 0;
    struct bootentry_s *pos;
    hlist_for_each_entry(pos, &BootList, node) {
        char desc[60];
        maxmenu++;
        printf("%d. %s\n", maxmenu
               , strtcpy(desc, pos->description, ARRAY_SIZE(desc)));
    }

    // Get key press
    for (;;) {
        scan_code = get_keystroke(1000);
        if (scan_code >= 1 && scan_code <= maxmenu+1) {
            break;
        }
    }
    printf("\n");
    if (scan_code == 0x01)
        // ESC
        return;

    // Find entry and make top priority.
    int choice = scan_code - 1;
    hlist_for_each_entry(pos, &BootList, node) {
        if (! --choice)
            break;
    }
    hlist_del(&pos->node);
    pos->priority = 0;
    hlist_add_head(&pos->node, &BootList);
}

// BEV (Boot Execution Vector) list
struct bev_s {
    int type;
    u32 vector;
};
static struct bev_s BEV[20];
static int BEVCount;
static int HaveHDBoot, HaveFDBoot;

static void
add_bev(int type, u32 vector)
{
    if (type == IPL_TYPE_HARDDISK && HaveHDBoot++)
        return;
    if (type == IPL_TYPE_FLOPPY && HaveFDBoot++)
        return;
    if (BEVCount >= ARRAY_SIZE(BEV))
        return;
    struct bev_s *bev = &BEV[BEVCount++];
    bev->type = type;
    bev->vector = vector;
}

// Prepare for boot - show menu and run bcvs.
void
bcv_prepboot(void)
{
    if (! CONFIG_BOOT)
        return;

    int haltprio = find_prio("HALT");
    if (haltprio >= 0)
        bootentry_add(IPL_TYPE_HALT, haltprio, 0, "HALT");

    // Map drives and populate BEV list
    struct bootentry_s *pos;
    hlist_for_each_entry(pos, &BootList, node) {
        switch (pos->type) {
        case IPL_TYPE_BCV:
            call_bcv(pos->vector.seg, pos->vector.offset);
            add_bev(IPL_TYPE_HARDDISK, 0);
            break;
        case IPL_TYPE_FLOPPY:
            map_floppy_drive(pos->drive);
            add_bev(IPL_TYPE_FLOPPY, 0);
            break;
        case IPL_TYPE_HARDDISK:
            map_hd_drive(pos->drive);
            add_bev(IPL_TYPE_HARDDISK, 0);
            break;
        case IPL_TYPE_CDROM:
            map_cd_drive(pos->drive);
            // NO BREAK
        default:
            add_bev(pos->type, pos->data);
            break;
        }
    }

    // If nothing added a floppy/hd boot - add it manually.
    add_bev(IPL_TYPE_FLOPPY, 0);
    add_bev(IPL_TYPE_HARDDISK, 0);
}


/****************************************************************
 * Boot code (int 18/19)
 ****************************************************************/

// Jump to a bootup entry point.
static void
call_boot_entry(struct segoff_s bootsegip, u8 bootdrv)
{
    dprintf(1, "Booting from %04x:%04x\n", bootsegip.seg, bootsegip.offset);
    struct bregs br;
    memset(&br, 0, sizeof(br));
    br.flags = F_IF;
    br.code = bootsegip;
    // Set the magic number in ax and the boot drive in dl.
    br.dl = bootdrv;
    br.ax = 0xaa55;
    farcall16(&br);
}

// Boot from a disk (either floppy or harddrive)
static void
boot_disk(u8 bootdrv, int checksig)
{
    u16 bootseg = 0x07c0;

    // Read sector
    struct bregs br;
    memset(&br, 0, sizeof(br));
    br.flags = F_IF;
    br.dl = bootdrv;
    br.es = bootseg;
    br.ah = 2;
    br.al = 1;
    br.cl = 1;
    call16_int(0x13, &br);

    if (br.flags & F_CF) {
        printf("Boot failed: could not read the boot disk\n\n");
        return;
    }

    if (checksig) {
        struct mbr_s *mbr = (void*)0;
        if (GET_FARVAR(bootseg, mbr->signature) != MBR_SIGNATURE) {
            printf("Boot failed: not a bootable disk\n\n");
            return;
        }
    }

    /* Canonicalize bootseg:bootip */
    u16 bootip = (bootseg & 0x0fff) << 4;
    bootseg &= 0xf000;

    call_boot_entry(SEGOFF(bootseg, bootip), bootdrv);
}

// Boot from a CD-ROM
static void
boot_cdrom(struct drive_s *drive_g)
{
    if (! CONFIG_CDROM_BOOT)
        return;
    printf("Booting from DVD/CD...\n");

    int status = cdrom_boot(drive_g);
    if (status) {
        printf("Boot failed: Could not read from CDROM (code %04x)\n", status);
        return;
    }

    u8 bootdrv = CDEmu.emulated_drive;
    u16 bootseg = CDEmu.load_segment;
    /* Canonicalize bootseg:bootip */
    u16 bootip = (bootseg & 0x0fff) << 4;
    bootseg &= 0xf000;

    call_boot_entry(SEGOFF(bootseg, bootip), bootdrv);
}

// Boot from a CBFS payload
static void
boot_cbfs(struct cbfs_file *file)
{
    if (!CONFIG_COREBOOT_FLASH)
        return;
    printf("Booting from CBFS...\n");
    cbfs_run_payload(file);
}

// Boot from a BEV entry on an optionrom.
static void
boot_rom(u32 vector)
{
    printf("Booting from ROM...\n");
    struct segoff_s so;
    so.segoff = vector;
    call_boot_entry(so, 0);
}

// Unable to find bootable device - warn user and eventually retry.
static void
boot_fail(void)
{
    if (BootRetryTime == (u32)-1)
        printf("No bootable device.\n");
    else
        printf("No bootable device.  Retrying in %d seconds.\n"
               , BootRetryTime/1000);
    // Wait for 'BootRetryTime' milliseconds and then reboot.
    u32 end = irqtimer_calc(BootRetryTime);
    for (;;) {
        if (BootRetryTime != (u32)-1 && irqtimer_check(end))
            break;
        yield_toirq();
    }
    printf("Rebooting.\n");
    struct bregs br;
    memset(&br, 0, sizeof(br));
    br.code = SEGOFF(SEG_BIOS, (u32)reset_vector);
    farcall16big(&br);
}

// Determine next boot method and attempt a boot using it.
static void
do_boot(int seq_nr)
{
    if (! CONFIG_BOOT)
        panic("Boot support not compiled in.\n");

    if (seq_nr >= BEVCount)
        boot_fail();

    // Boot the given BEV type.
    struct bev_s *ie = &BEV[seq_nr];
    switch (ie->type) {
    case IPL_TYPE_FLOPPY:
        printf("Booting from Floppy...\n");
        boot_disk(0x00, CheckFloppySig);
        break;
    case IPL_TYPE_HARDDISK:
        printf("Booting from Hard Disk...\n");
        boot_disk(0x80, 1);
        break;
    case IPL_TYPE_CDROM:
        boot_cdrom((void*)ie->vector);
        break;
    case IPL_TYPE_CBFS:
        boot_cbfs((void*)ie->vector);
        break;
    case IPL_TYPE_BEV:
        boot_rom(ie->vector);
        break;
    case IPL_TYPE_HALT:
        boot_fail();
        break;
    }

    // Boot failed: invoke the boot recovery function
    struct bregs br;
    memset(&br, 0, sizeof(br));
    br.flags = F_IF;
    call16_int(0x18, &br);
}

int BootSequence VARLOW = -1;

// Boot Failure recovery: try the next device.
void VISIBLE32FLAT
handle_18(void)
{
    debug_enter(NULL, DEBUG_HDL_18);
    int seq = BootSequence + 1;
    BootSequence = seq;
    do_boot(seq);
}

// INT 19h Boot Load Service Entry Point
void VISIBLE32FLAT
handle_19(void)
{
    debug_enter(NULL, DEBUG_HDL_19);
    BootSequence = 0;
    do_boot(0);
}
