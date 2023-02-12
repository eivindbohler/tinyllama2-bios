#include "bregs.h"   // struct bregs
#include "config.h"  // SEG_BIOS
#include "farptr.h"  // FLATPTR_TO_SEG, FLATPTR_TO_OFFSET
#include "output.h"  // dprintf
#include "stacks.h"  // call16_int
#include "string.h"  // memset
#include "util.h"
#include "bios_fonts.h"

#define BLACK         0x00
#define BLUE          0x01
#define GREEN         0x02
#define CYAN          0x03
#define RED           0x04
#define MAGENTA       0x05
#define BROWN         0x06
#define LIGHT_GRAY    0x07
#define DARK_GRAY     0x08
#define LIGHT_BLUE    0x09
#define LIGHT_GREEN   0x0A
#define LIGHT_CYAN    0x0B
#define LIGHT_RED     0x0C
#define LIGHT_MAGENTA 0x0D
#define YELLOW        0x0E
#define WHITE         0x0F

#define COLOR(foreground, background) ((foreground & 0x0F) | ((background & 0x0F) << 4))

#define BACKGROUND        BLUE
#define ACTIVE_BACKGROUND RED
#define PASSIVE_TEXT      WHITE
#define ACTIVE_TEXT       YELLOW
#define POPUP_BACKGROUND  LIGHT_GRAY

#define INITIALIZED_OFFSET            0xC0
#define CPU_FREQ_INDEX_OFFSET         0xC1
#define CACHE_ENABLED_OFFSET          0xC2
#define BOOT_TUNE_OFFSET              0xC3
#define COM1_CLOCK_INDEX_OFFSET       0xC4
#define COM1_CLOCK_RATIO_INDEX_OFFSET 0xC5
#define COM2_CLOCK_INDEX_OFFSET       0xC6
#define COM2_CLOCK_RATIO_INDEX_OFFSET 0xC7
#define ISA_FREQ_INDEX_OFFSET         0xC8

int exit_now = 0;

const char *enabled_disabled_values[] = {"Disabled", "Enabled"};
const int enabled_disabled_values_length = 2;

const char cpu_freq_title[] = "CPU Frequency";
const char *cpu_freq_values[] = {"60 MHz", "100 MHz", "200 MHz", "300 MHz", "400 MHz", "466 MHz", "500 MHz"};
const int cpu_freq_values_length = 7;
void set_cpu_freq_value(struct bios_settings *s, int value) { s->cpu_freq_index = value; }
const char *cpu_freq_desc[] = {
    "A low CPU frequency makes sense for use with early",
    "80s programs and games - and draws less power.",
    "A high frequency runs hotter and might require active cooling."
};

const char cache_title[] = "L1 Cache";
void set_cache_value(struct bios_settings *s, int value) { s->cache_enabled = value; }
const char *cache_desc[] = {
    "Disabling the CPU L1 Cache slows down the system significantly.",
    "Only use this setting if you require 286-like performance for",
    "speed-sensitive, early 80s games."
};

const char boot_tune_title[] = "Boot Tune";
const char *boot_tune_values[] = {"Disabled", "Mushroom", "Ducks"};
const int boot_tune_values_length = 3;
void set_boot_tune_value(struct bios_settings *s, int value) { s->boot_tune = value; }
const char *boot_tune_desc[] = {
    "",
    "Select or disable playing a short tune when booting the system.",
    ""
};

const char com1_title[] = "COM1 Clock/Ratio";
const char com2_title[] = "COM2 Clock/Ratio";
const char *com_values[] = {"1.8432 MHz / 16", "48 MHz / 16", "48 MHz / 8"};
const int com_values_length = 3;
void set_com1_values(struct bios_settings *s, int value)
{
    s->com1_clock_index = value > 0 ? 1 : 0;
    s->com1_clock_ratio_index = value > 1 ? 1 : 0;
}
void set_com2_values(struct bios_settings *s, int value)
{
    s->com2_clock_index = value > 0 ? 1 : 0;
    s->com2_clock_ratio_index = value > 1 ? 1 : 0;
}
const char *com_desc[] = {
    "The COM clock/ratio equals the baud speed with a divider of 1.",
    "Eg., setting the baud rate to 115200 (divider 1) at 1.8432 MHz / 16 gives",
    "you 115200 baud, 57600 (divider 2) at 48 MHz / 16 turns into 1.5 Mbaud."
};

const char isa_freq_title[] = "ISA Bus Frequency";
const char *isa_freq_values[] = {"8.33 MHz", "16.67 MHz", "25 MHz", "33 MHz"};
const int isa_freq_values_length = 4;
void set_isa_freq_value(struct bios_settings *s, int value) { s->isa_freq_index = value; }
const char *isa_freq_desc[] = {
    "8.33 MHz is the original, most compatible ISA bus frequency.",
    "Higher speeds are possible, but probably not a good idea if",
    "connected peripherals don't support this."
};

const char exit_title[] = "Exit Without Saving?";
const char *exit_values[] = {"No", "Yes"};
const int exit_values_length = 2;
void set_exit_value(struct bios_settings *s, int value) { exit_now = value; }

const u8 clock_array[7][6] = {
  //{0x3C, 0x37, 0x23, 0x02, 0x1F, 0x07},  //  50/125/100
  {0x48, 0x37, 0x23, 0x02, 0xEF, 0x07},  //  60/150/100
  {0x40, 0x26, 0x23, 0x02, 0x3F, 0x07},  // 100/200/100
  {0x30, 0x03, 0x23, 0x02, 0xDF, 0x07},  // 200/200/100
  {0x48, 0x03, 0x23, 0x02, 0x7F, 0x07},  // 300/300/100
  {0x80, 0x62, 0x23, 0x02, 0x8F, 0x07},  // 400/400/100
  {0xA8, 0x53, 0x23, 0x02, 0x3F, 0x07},  // 466/350/100
  {0x78, 0x52, 0x23, 0x02, 0xDF, 0x07},  // 500/375/100
  //{0x78, 0x52, 0x04, 0x02, 0xDF, 0x07},  // 500/375/125
};

int selection = 0;
const int max_selection = 5;

void reboot(void)
{
    struct bregs br;
    memset(&br, 0, sizeof(br));
    br.code = SEGOFF(SEG_BIOS, (u32)reset_vector);
    farcall16big(&br);
}

void save_settings(struct bios_settings *s)
{
    int i;
    u8 crossbar[spi_page_size];
    u8 bios_settings[spi_page_size];

    if (get_spi_flash_info() == 0) {
        dprintf(1, "Aborting");
        return;
    }
    dprintf(1, "Reading crossbar page\n");
    for (i = 0; i < spi_page_size; i++) {
        crossbar[i] = spi_flash_read_byte(spi_crossbar_offset + i);
    }

    dprintf(1, "Reading BIOS settings page\n");
    for (i = 0; i < spi_page_size; i++) {
        bios_settings[i] = spi_flash_read_byte(spi_bios_settings_offset + i);
    }

    dprintf(1, "Modifying BIOS settings page\n");
    bios_settings[0xB6] = clock_array[s->cpu_freq_index][0];
    bios_settings[0xB7] = clock_array[s->cpu_freq_index][1];
    bios_settings[0xBB] = clock_array[s->cpu_freq_index][2];
    bios_settings[0xBC] = clock_array[s->cpu_freq_index][3];
    bios_settings[0xBD] = clock_array[s->cpu_freq_index][4];
    bios_settings[0xBF] = clock_array[s->cpu_freq_index][5];

    bios_settings[INITIALIZED_OFFSET]            = 1;
    bios_settings[CPU_FREQ_INDEX_OFFSET]         = (u8)s->cpu_freq_index;
    bios_settings[CACHE_ENABLED_OFFSET]          = (u8)s->cache_enabled;
    bios_settings[BOOT_TUNE_OFFSET]              = (u8)s->boot_tune;
    bios_settings[COM1_CLOCK_INDEX_OFFSET]       = (u8)s->com1_clock_index;
    bios_settings[COM1_CLOCK_RATIO_INDEX_OFFSET] = (u8)s->com1_clock_ratio_index;
    bios_settings[COM2_CLOCK_INDEX_OFFSET]       = (u8)s->com2_clock_index;
    bios_settings[COM2_CLOCK_RATIO_INDEX_OFFSET] = (u8)s->com2_clock_ratio_index;
    bios_settings[ISA_FREQ_INDEX_OFFSET]         = (u8)s->isa_freq_index;

    dprintf(1, "Erasing sector\n");
    spi_flash_erase_sector(spi_sector_offset);

    dprintf(1, "Writing back crossbar page\n");
    for (i = 0; i < spi_page_size; i++) {
        spi_flash_write_byte(spi_crossbar_offset + i, crossbar[i]);
    }

    dprintf(1, "Writing back BIOS settings page\n");
    for (i = 0; i < spi_page_size; i++) {
        spi_flash_write_byte(spi_bios_settings_offset + i, bios_settings[i]);
    }
}

void load_bios_settings(struct bios_settings *s)
{
    int initialized = (int)spi_flash_read_byte(spi_bios_settings_offset + INITIALIZED_OFFSET) == 1 ? 1 : 0;
    if (!initialized) {
        s->has_changes = 0;
        s->cpu_freq_index = 3; // 300 MHz
        s->cache_enabled = 1;
        s->boot_tune = 1;
        s->com1_clock_index = 0;
        s->com1_clock_ratio_index = 0;
        s->com2_clock_index = 0;
        s->com2_clock_ratio_index = 0;
        s->isa_freq_index = 0;
        save_settings(s);
    } else {
        s->has_changes = 0;
        s->cpu_freq_index = (int)spi_flash_read_byte(spi_bios_settings_offset + CPU_FREQ_INDEX_OFFSET);
        s->cache_enabled = (int)spi_flash_read_byte(spi_bios_settings_offset + CACHE_ENABLED_OFFSET);
        s->boot_tune = (int)spi_flash_read_byte(spi_bios_settings_offset + BOOT_TUNE_OFFSET);
        s->com1_clock_index = (int)spi_flash_read_byte(spi_bios_settings_offset + COM1_CLOCK_INDEX_OFFSET);
        s->com1_clock_ratio_index = (int)spi_flash_read_byte(spi_bios_settings_offset + COM1_CLOCK_RATIO_INDEX_OFFSET);
        s->com2_clock_index = (int)spi_flash_read_byte(spi_bios_settings_offset + COM2_CLOCK_INDEX_OFFSET);
        s->com2_clock_ratio_index = (int)spi_flash_read_byte(spi_bios_settings_offset + COM2_CLOCK_RATIO_INDEX_OFFSET);
        s->isa_freq_index = (int)spi_flash_read_byte(spi_bios_settings_offset + ISA_FREQ_INDEX_OFFSET);
    }
}

u32 get_current_cpu_freq(void)
{
    u32 strapreg2 = nbsb_read32(vx86ex_nb, 0x64);
    u32 ddiv = (strapreg2 >> 14) & 0x01L;
    u32 cdiv = (strapreg2 >> 12) & 0x03L;
    u32 cms  = (strapreg2 >> 8) & 0x03L;
    u32 cns  = strapreg2 & 0xFFL;
    int crs  = (int)((strapreg2 >> 10) & 0x03L);
    dprintf(1, "NS       = %d\n", (unsigned char)cns);
    dprintf(1, "MS       = %d\n", (unsigned char)cms);
    dprintf(1, "RS       = %d\n", crs);
    dprintf(1, "CPU_DIV  = %d\n", (unsigned char)cdiv);
    dprintf(1, "DRAM_DIV = %d\n", (unsigned char)ddiv);
    return (25L * cns) / (cms * (1L << crs) * (cdiv + 2L));
}

void set_cursor_position(u8 row, u8 col)
{
    struct bregs br;
    memset(&br, 0, sizeof(br));
    br.flags = F_IF;
    br.ah = 0x02;
    br.dh = row;
    br.dl = col;
    call16_int(0x10, &br);
}

// color: high 4 bits = background, low 4 bits = foreground
void print_color_char(const char c, u8 color, u16 repeat)
{
    struct bregs br;
    memset(&br, 0, sizeof(br));
    br.flags = F_IF;
    br.ah = 0x09;
    br.al = c;
    br.bl = color;
    br.cx = repeat;
    call16_int(0x10, &br);
}

// color: high 4 bits = background, low 4 bits = foreground
void print_color_string(const char *str, u16 length, u8 color, u8 row, u8 col)
{
    struct bregs br;
    memset(&br, 0, sizeof(br));
    br.flags = F_IF;
    br.ah = 0x13;
    br.al = 0; // subservice 0
    br.bl = color;
    br.cx = length;
    br.dh = row;
    br.dl = col;
    br.es = FLATPTR_TO_SEG(str);
    br.bp = FLATPTR_TO_OFFSET(str);
    call16_int(0x10, &br);
}

// clear screen = scroll up window
// color: high 4 bits = background, low 4 bits = foreground
void clear_screen(u8 color)
{
    struct bregs br;
    memset(&br, 0, sizeof(br));
    br.flags = F_IF;
    br.ah = 0x06;
    br.al = 0; // clear entire window
    br.bh = color;
    br.ch = 0;  // row of top left corner of window
    br.cl = 0;  // column of top left corner of window
    br.dh = 24; // row of bottom right corner of window
    br.dl = 79; // column of bottom right corner of window
    call16_int(0x10, &br);
}

void load_custom_fonts(u8 *font_ptr, u16 ascii_position, u16 count)
{
    struct bregs br;
    memset(&br, 0, sizeof(br));
    br.flags = F_IF;
    br.ax = 0x1110;
    br.bh = 16;             // height of each character
    br.bl = 0;              // font block
    br.cx = count;          // how manay characters will be redefined?
    br.dx = ascii_position; // index of first character to be redefined
    br.es = FLATPTR_TO_SEG(font_ptr);
    br.bp = FLATPTR_TO_OFFSET(font_ptr);
    call16_int(0x10, &br);
}

void draw_frame(void)
{
    int r;
    u8 frame_color = COLOR(PASSIVE_TEXT, BACKGROUND);
    for (r = 0; r < 25; r++)
    {
        set_cursor_position(r, 0);
        switch (r) {
            case 0:
                print_color_char(0xC9, frame_color, 1);  // thick top left corner
                set_cursor_position(r, 1);
                print_color_char(0xCD, frame_color, 78); // thick horizontal line
                set_cursor_position(r, 79);
                print_color_char(0xBB, frame_color, 1); // thick top right corner
                break;
            case 1:
            case 2:
            case 18:
            case 19:
            case 21:
            case 22:
            case 23:
                print_color_char(0xBA, frame_color, 1);  // thick vertical line
                set_cursor_position(r, 79);
                print_color_char(0xBA, frame_color, 1);  // thick vertical line
                break;
            case 3:
                print_color_char(0xCC, frame_color, 1);  // thick vertical line with right thick horizontal line
                set_cursor_position(r, 1);
                print_color_char(0xCD, frame_color, 38); // thick horizontal line
                set_cursor_position(r, 39);
                print_color_char(0xD1, frame_color, 1);  // thick horizontal line with downward thin vertical line
                set_cursor_position(r, 40);
                print_color_char(0xCD, frame_color, 39); // thick horizontal line
                set_cursor_position(r, 79);
                print_color_char(0xB9, frame_color, 1); // thick vertical line with left thick horizontal line
                break;
            case 4 ... 16:
                print_color_char(0xBA, frame_color, 1);  // thick vertical line
                set_cursor_position(r, 39);
                print_color_char(0xB3, frame_color, 1);  // thin vertical line
                set_cursor_position(r, 79);
                print_color_char(0xBA, frame_color, 1);  // thick vertical line
                break;
            case 17:
                print_color_char(0xC7, frame_color, 1);  // thick vertical line with right thin horizontal line
                set_cursor_position(r, 1);
                print_color_char(0xC4, frame_color, 38); // thin horizontal line
                set_cursor_position(r, 39);
                print_color_char(0xC1, frame_color, 1);  // thin horizontal line with upward thin vertical line
                set_cursor_position(r, 40);
                print_color_char(0xC4, frame_color, 39); // thin horizontal line
                set_cursor_position(r, 79);
                print_color_char(0xB6, frame_color, 1); // thick vertical line with left thin horizontal line
                break;
            case 20:
                print_color_char(0xC7, frame_color, 1);  // thick vertical line with right thin horizontal line
                set_cursor_position(r, 1);
                print_color_char(0xC4, frame_color, 78); // thin horizontal line
                set_cursor_position(r, 79);
                print_color_char(0xB6, frame_color, 1); // thick vertical line with left thin horizontal line
                break;
            case 24:
                print_color_char(0xC8, frame_color, 1);  // thick bottom left corner
                set_cursor_position(r, 1);
                print_color_char(0xCD, frame_color, 78); // thick horizontal line
                set_cursor_position(r, 79);
                print_color_char(0xBC, frame_color, 1); // thick bottom right corner
                break;
            default:
                break;
        }
    }
}

void draw_static_text(void)
{
    const char header_one[] = "TinyLlama BIOS Setup";
    u16 header_one_length = sizeof(header_one) - 1;
    u8 header_one_row = 1;
    u8 header_one_col = (80 - header_one_length) / 2;

    const char header_two[] = "(C) 2023 Eivind Bohler";
    u16 header_two_length = sizeof(header_two) - 1;
    u8 header_two_row = 2;
    u8 header_two_col = (80 - header_two_length) / 2;

    const char quit[] = "ESC : Quit";
    u16 quit_length = sizeof(quit) - 1;
    u8 quit_row = 18;
    u8 quit_col = 2;

    const char move[] = "U D : Move Between Items";
    u16 move_length = sizeof(move) - 1;
    u8 move_row = 18;
    u8 move_col = 41;

    const char save[] = "F10 : Save & Exit";
    u16 save_length = sizeof(save) - 1;
    u8 save_row = 19;
    u8 save_col = 2;

    const char select[] = "Enter : Select Item";
    u16 select_length = sizeof(select) - 1;
    u8 select_row = 19;
    u8 select_col = 41;

    u8 active_color = COLOR(ACTIVE_TEXT, BACKGROUND);
    u8 passive_color = COLOR(PASSIVE_TEXT, BACKGROUND);

    print_color_string(header_one, header_one_length, active_color, header_one_row, header_one_col);
    print_color_string(header_two, header_two_length, passive_color, header_two_row, header_two_col);
    print_color_string(quit, quit_length, passive_color, quit_row, quit_col);
    print_color_string(move, move_length, passive_color, move_row, move_col);
    print_color_string(save, save_length, passive_color, save_row, save_col);
    print_color_string(select, select_length, passive_color, select_row, select_col);

    set_cursor_position(1, 74);
    print_color_char(llama_font_0, passive_color, 1);
    set_cursor_position(1, 75);
    print_color_char(llama_font_1, passive_color, 1);
    set_cursor_position(1, 76);
    print_color_char(llama_font_2, passive_color, 1);
    set_cursor_position(1, 77);
    print_color_char(llama_font_3, passive_color, 1);
    set_cursor_position(2, 74);
    print_color_char(llama_font_4, passive_color, 1);
    set_cursor_position(2, 75);
    print_color_char(llama_font_5, passive_color, 1);
    set_cursor_position(2, 76);
    print_color_char(llama_font_6, passive_color, 1);
    set_cursor_position(2, 77);
    print_color_char(llama_font_7, passive_color, 1);

    set_cursor_position(move_row, move_col);
    print_color_char(arrow_font_up, passive_color, 1);
    set_cursor_position(move_row, move_col + 2);
    print_color_char(arrow_font_down, passive_color, 1);
}

void draw_menu_items(void)
{
    int i;
    u8 cap = 38;
    u8 std_col = 3;

    char cpu_freq[cap];
    snprintf(cpu_freq, cap, "%s", cpu_freq_title);
    u16 cpu_freq_length = strlen(cpu_freq);
    u8 cpu_freq_row = 4;

    char cache[cap];
    snprintf(cache, cap, "%s", cache_title);
    u16 cache_length = strlen(cache);
    u8 cache_row = 6;

    char isa_freq[cap];
    snprintf(isa_freq, cap, "%s", isa_freq_title);
    u16 isa_freq_length = strlen(isa_freq);
    u8 isa_freq_row = 8;

    char com1[cap];
    snprintf(com1, cap, "%s", com1_title);
    u16 com1_length = strlen(com1);
    u8 com1_row = 10;

    char com2[cap];
    snprintf(com2, cap, "%s", com2_title);
    u16 com2_length = strlen(com2);
    u8 com2_row = 12;

    char boot_tune[cap];
    snprintf(boot_tune, cap, "%s", boot_tune_title);
    u16 boot_tune_length = strlen(boot_tune);
    u8 boot_tune_row = 14;

    u8 active_color = COLOR(PASSIVE_TEXT, ACTIVE_BACKGROUND);
    u8 passive_color = COLOR(ACTIVE_TEXT, BACKGROUND);

    print_color_string(cpu_freq, cpu_freq_length, selection == 0 ? active_color : passive_color, cpu_freq_row, std_col);
    print_color_string(cache, cache_length, selection == 1 ? active_color : passive_color, cache_row, std_col);
    print_color_string(isa_freq, isa_freq_length, selection == 2 ? active_color : passive_color, isa_freq_row, std_col);
    print_color_string(com1, com1_length, selection == 3 ? active_color : passive_color, com1_row, std_col);
    print_color_string(com2, com2_length, selection == 4 ? active_color : passive_color, com2_row, std_col);
    print_color_string(boot_tune, boot_tune_length, selection == 5 ? active_color : passive_color, boot_tune_row, std_col);

    u8 color = COLOR(PASSIVE_TEXT, BACKGROUND);
    set_cursor_position(21, 1);
    print_color_char(' ', color, 78);
    set_cursor_position(22, 1);
    print_color_char(' ', color, 78);
    set_cursor_position(23, 1);
    print_color_char(' ', color, 78);

    const char **desc;
    switch (selection) {
        case 0:
            desc = cpu_freq_desc;
            break;
        case 1:
            desc = cache_desc;
            break;
        case 2:
            desc = isa_freq_desc;
            break;
        case 3:
        case 4:
            desc = com_desc;
            break;
        case 5:
        default:
            desc = boot_tune_desc;
            break;
    }
    for (i = 0; i < 3; i++) {
        char d[80];
        snprintf(d, 80, "%s", desc[i]);
        u16 length = strlen(d);
        print_color_string(d, length, color, 21 + i, (80 - length) / 2);
    }
}

void draw_settings(struct bios_settings *s)
{
    u8 cap = 39;
    u8 std_col = 42;
    u8 color = COLOR(PASSIVE_TEXT, BACKGROUND);

    char cpu_freq[cap];
    snprintf(cpu_freq, cap, "%s: %s", cpu_freq_title, cpu_freq_values[s->cpu_freq_index]);
    u16 cpu_freq_length = strlen(cpu_freq);
    u8 cpu_freq_row = 4;

    char cache[cap];
    snprintf(cache, cap, "%s: %s", cache_title, enabled_disabled_values[s->cache_enabled]);
    u16 cache_length = strlen(cache);
    u8 cache_row = 6;

    char isa_freq[cap];
    snprintf(isa_freq, cap, "%s: %s", isa_freq_title, isa_freq_values[s->isa_freq_index]);
    u16 isa_freq_length = strlen(isa_freq);
    u8 isa_freq_row = 8;

    int com1_index = s->com1_clock_index ? (s->com1_clock_ratio_index ? 2 : 1) : 0;
    char com1[cap];
    snprintf(com1, cap, "%s: %s", com1_title, com_values[com1_index]);
    u16 com1_length = strlen(com1);
    u8 com1_row = 10;

    int com2_index = s->com2_clock_index ? (s->com2_clock_ratio_index ? 2 : 1) : 0;
    char com2[cap];
    snprintf(com2, cap, "%s: %s", com2_title, com_values[com2_index]);
    u16 com2_length = strlen(com2);
    u8 com2_row = 12;

    char boot_tune[cap];
    snprintf(boot_tune, cap, "%s: %s", boot_tune_title, boot_tune_values[s->boot_tune]);
    u16 boot_tune_length = strlen(boot_tune);
    u8 boot_tune_row = 14;

    print_color_string(cpu_freq, cpu_freq_length, color, cpu_freq_row, std_col);
    print_color_string(cache, cache_length, color, cache_row, std_col);
    print_color_string(isa_freq, isa_freq_length, color, isa_freq_row, std_col);
    print_color_string(com1, com1_length, color, com1_row, std_col);
    print_color_string(com2, com2_length, color, com2_row, std_col);
    print_color_string(boot_tune, boot_tune_length, color, boot_tune_row, std_col);
}

void draw_description(int only_clear, const char **description_lines)
{
    int i;
    u8 color = COLOR(PASSIVE_TEXT, BACKGROUND);
    for (i = 21; i < 24; i++) {
        set_cursor_position(i, 1);
        print_color_char(' ', color, 78);
    }
    if (only_clear) {
        return;
    }

    for (i = 0; i < 3; i++) {
        u16 line_length = strlen(description_lines[i]);
        u16 line_padding = (80 - line_length) / 2;
        print_color_string(description_lines[i], line_length, color, 21 + i, line_padding);
    }
}

int draw_popup(const char *t, int number, const char **values, int selected_value)
{
    int i;
    char title[80];
    snprintf(title, 80, "%s", t);
    u16 title_length = strlen(title);
    u16 value_lengths[number];
    u16 max_length = title_length;
    for (i = 0; i < number; i++) {
        u16 value_length = strlen(values[i]);
        if (value_length > max_length) {
            max_length = value_length;
        }
        value_lengths[i] = value_length;
    }
    u16 popup_width = max_length + 6;
    if (popup_width < 23) {
        popup_width = 23;
    }
    u16 popup_height = number + 2;
    u16 popup_row = (25 - popup_height) / 2;
    u16 popup_col = (80 - popup_width) / 2;

    u16 title_left_padding = (popup_width - title_length) / 2;
    u16 title_right_padding = popup_width - title_left_padding - title_length;

    u8 color = COLOR(BACKGROUND, POPUP_BACKGROUND);

    set_cursor_position(popup_row, popup_col);
    print_color_char(0xDA, color, 1); // thin top left corner
    set_cursor_position(popup_row, popup_col + 1);
    print_color_char(0xC4, color, title_left_padding - 2); // thin horizontal line
    set_cursor_position(popup_row, popup_col + title_left_padding - 1);
    print_color_char(' ', color, 1);
    print_color_string(title, title_length, color, popup_row, popup_col + title_left_padding);
    set_cursor_position(popup_row, popup_col + title_left_padding + title_length);
    print_color_char(' ', color, 1);
    set_cursor_position(popup_row, popup_col + title_left_padding + title_length + 1);
    print_color_char(0xC4, color, title_right_padding - 2); // thin horizontal line
    set_cursor_position(popup_row, popup_col + popup_width - 1);
    print_color_char(0xBF, color, 1); // thin top right corner

    for (i = 0; i < number; i++) {
        set_cursor_position(popup_row + 1 + i, popup_col);
        print_color_char(0xB3, color, 1); // thin vertical line
        set_cursor_position(popup_row + 1 + i, popup_col + 1);
        print_color_char(' ', color, popup_width - 2);
        set_cursor_position(popup_row + 1 + i, popup_col + popup_width - 1);
        print_color_char(0xB3, color, 1); // thin vertical line
        set_cursor_position(popup_row + 1 + i, popup_col + popup_width);
        print_color_char(0xDB, COLOR(BLACK, BLACK), 2);

        u8 value_color = i == selected_value ? COLOR(PASSIVE_TEXT, BACKGROUND) : color;
        print_color_string(values[i], value_lengths[i], value_color, popup_row + 1 + i, popup_col + 3);
    }

    set_cursor_position(popup_row + 1 + number, popup_col);
    print_color_char(0xC0, color, 1); // thin bottom left corner
    set_cursor_position(popup_row + 1 + number, popup_col + 1);
    print_color_char(0xC4, color, popup_width - 2); // thin horizontal line
    set_cursor_position(popup_row + 1 + number, popup_col + popup_width - 1);
    print_color_char(0xD9, color, 1); // thin bottom right corner
    set_cursor_position(popup_row + 1 + number, popup_col + popup_width);
    print_color_char(0xDB, COLOR(BLACK, BLACK), 2);

    set_cursor_position(popup_row + 2 + number, popup_col + 2);
    print_color_char(0xDB, COLOR(BLACK, BLACK), popup_width);

    return 0;
}

int inc_selection(void)
{
    if (selection == max_selection) return 0;
    selection++;
    return 1;
}

int dec_selection(void)
{
    if (selection == 0) return 0;
    selection--;
    return 1;
}

void change_setting(struct bios_settings *s)
{
    int setting_selection;
    const char *title;
    int number_of_values;
    const char **values;
    void (*change_function)(struct bios_settings *, int);

    switch (selection) {
        case -1:
            setting_selection = 0;
            title = exit_title;
            number_of_values = exit_values_length;
            values = exit_values;
            change_function = &set_exit_value;
            break;
        case 0:
            setting_selection = s->cpu_freq_index;
            title = cpu_freq_title;
            number_of_values = cpu_freq_values_length;
            values = cpu_freq_values;
            change_function = &set_cpu_freq_value;
            break;
        case 1:
            setting_selection = s->cache_enabled;
            title = cache_title;
            number_of_values = enabled_disabled_values_length;
            values = enabled_disabled_values;
            change_function = &set_cache_value;
            break;
        case 2:
            setting_selection = s->isa_freq_index;
            title = isa_freq_title;
            number_of_values = isa_freq_values_length;
            values = isa_freq_values;
            change_function = &set_isa_freq_value;
            break;
        case 3:
            setting_selection = s->com1_clock_index ? (s->com1_clock_ratio_index ? 2 : 1) : 0;
            title = com1_title;
            number_of_values = com_values_length;
            values = com_values;
            change_function = &set_com1_values;
            break;
        case 4:
            setting_selection = s->com2_clock_index ? (s->com2_clock_ratio_index ? 2 : 1) : 0;
            title = com2_title;
            number_of_values = com_values_length;
            values = com_values;
            change_function = &set_com2_values;
            break;
        case 5:
            setting_selection = s->boot_tune;
            title = boot_tune_title;
            number_of_values = boot_tune_values_length;
            values = boot_tune_values;
            change_function = &set_boot_tune_value;
            break;
        default:
            return;
    }

    for (;;) {
        draw_popup(title, number_of_values, values, setting_selection);
        int scancode = get_keystroke_full(1000);
        if (scancode == -1) continue;
        switch (scancode >> 8) {
            case 0x01: // ESC
                return;
            case 0x48: // Up
                if (setting_selection > 0)
                    setting_selection--;
                break;
            case 0x50: // Down
                if (setting_selection < number_of_values - 1)
                    setting_selection++;
                break;
            case 0x0F: // TAB
                if ((scancode & 0x0F) == 0x09) { // Shift-TAB
                    if (setting_selection < number_of_values - 1)
                        setting_selection++;
                } else {
                    if (setting_selection > 0)
                        setting_selection--;
                }
                break;
            case 0x1C: // Return
            case 0xE0: // Numpad Enter
                change_function(s, setting_selection);
                s->has_changes = 1;
                return;
            default:
                break;
        }
        set_cursor_position(25, 0); // move the cursor below the last line
    }
}

int quit_without_saving(struct bios_settings *s)
{
    int previous_selection = selection;
    selection = -1;
    change_setting(s);
    selection = previous_selection;
    return exit_now;
}

void bios_setup_loop(struct bios_settings *s)
{
    int redraw_whole_screen = 1;
    for (;;) {
        if (redraw_whole_screen) {
            redraw_whole_screen = 0;
            clear_screen(COLOR(PASSIVE_TEXT, BACKGROUND));
            draw_frame();
            draw_static_text();
            draw_menu_items();
            draw_settings(s);
            set_cursor_position(25, 0);
        }
        int scancode = get_keystroke_full(1000);
        if (scancode == -1) continue;
        switch (scancode >> 8) {
            case 0x01: // ESC
                if (s->has_changes == 0 || quit_without_saving(s)) {
                    return;
                } else {
                  redraw_whole_screen = 1;
                }
                break;
            case 0x44: // F10
                save_settings(s);
                reboot();
                break;
            case 0x48: // Up
                if (dec_selection())
                    draw_menu_items();
                break;
            case 0x50: // Down
                if (inc_selection())
                    draw_menu_items();
                break;
            case 0x0F: // TAB
                if ((scancode & 0x0F) == 0x09) { // Shift-TAB
                    if (inc_selection())
                        draw_menu_items();
                } else {
                    if (dec_selection())
                        draw_menu_items();
                }
                break;
            case 0x1C: // Return/Enter
            case 0xE0: // Numpad Enter
                change_setting(s);
                redraw_whole_screen = 1;
                break;
            default:
                break;
        }
        set_cursor_position(25, 0); // move the cursor below the last line
    }
}

void bios_setup_main(struct bios_settings *s)
{
    load_custom_fonts(bios_fonts+bios_font_D0_pos, 0xD0, 1);
    load_custom_fonts(bios_fonts+bios_font_D2_pos, 0xD2, 7);
    load_custom_fonts(bios_fonts+bios_font_E0_pos, 0xE0, 2);
    bios_setup_loop(s);
    clear_screen(COLOR(LIGHT_GRAY, BLACK));
    load_custom_fonts(VGA8_F16+bios_font_D0_pos, 0xD0, 1);
    load_custom_fonts(VGA8_F16+bios_font_D2_pos, 0xD2, 7);
    load_custom_fonts(VGA8_F16+bios_font_E0_pos, 0xE0, 2);
    set_cursor_position(1, 0);
}

/*

Below is how we calculate the different crossbar values for CPU frequency

PLL Freq = 25 * NS / (MS * 2^RS)
CPU Freq = PLL / (CPU_DIV + 2)
DRAM Freq = PLL / (2 * (DRAM_DIV + 1))

0xB6: NS
0xB7: [6] - DRAM_DIV, [5:4] - CPU_DIV, [3:2] - RS, [1:0] - MS
0xBB: [7] - PLL2M, [6] - PLL1M, [5:4] - PCI_Mode, [3:0] - PCI_DIV
0xBC: [5] - DIS_SPIbp, [4] - DIS_D3GT, [3] - DIS_D3WL, [2:0] - PLL_1_IPSEL
0xBD: [7:4] - Checksum*
0xBE: BOARD_ID (Low)
0xBF: [3:0] - BOARD_ID (High)

* Checksum:

Example:

 50/125/100
PLL:  25 * 60 = 1500. 1500 / (3 * 2^1) = 1500 / (3 * 2) = 250
CPU:  250 / (3 + 2) = 50
DRAM: 250 / (2 * (0 + 1)) = 125
CHK:  0x3C + 0x37 + 0x23 + 0x02 = 0x98. 0x09 + 0x08 = 0x11. "0x1F"

//  60/150/100
// PLL:  25 * 72 = 1800. 1800 / (3 * 2^1) = 1800 / (3 * 2) = 300
// CPU:  300 / (3 + 2) = 60
// DRAM: 300 / (2 * (0 + 1)) = 150
// CHK:  0x48 + 0x37 + 0x23 + 0x02 = 0xA4. 0x0A + 0x04 = 0x0E. "0xEF"

100/200/100
PLL:  25 * 64 = 1600. 1600 / (2 * 2^1) = 1600 / (2 * 2) = 400
CPU:  400 / (2 + 2) = 100
DRAM: 400 / (2 * (0 + 1)) = 200
CHK:  0x40 + 0x26 + 0x23 + 0x02 = 0x8B. 0x08 + 0x0B = 0x13. "0x3F"

200/200/100
PLL:  25 * 48 = 1200. 1200 / (3 * 2^0) = 1200 / (3 * 1) = 400
CPU:  400 / (0 + 2) = 200
DRAM: 400 / (2 * (0 + 1)) = 200
CHK:  0x30 + 0x03 + 0x23 + 0x02 = 0x58. 0x05 + 0x08 = 0x0D. "0xDF"

300/300/100
PLL:  25 * 72 = 1800. 1800 / (3 * 2^0) = 1800 / (3 * 1) = 600
CPU:  600 / (0 + 2) = 300
DRAM: 600 / (2 * (0 + 1)) = 300
CHK:  0x48 + 0x03 + 0x23 + 0x02 = 0x70. 0x07 + 0x00 = 0x07. "0x7F"

400/400/100
PLL:  25 * 128 = 3200. 3200 / (2 * 2^0) = 3200 / (2 * 1) = 1600
CPU:  1600 / (2 + 2) = 400
DRAM: 1600 / (2 * (1 + 1)) = 1600 / 4 = 400
CHK:  0x80 + 0x62 + 0x23 + 0x02 = 0x107. 0x01 + 0x00 + 0x07 = 0x08. "0x8F"

466/350/100
PLL:  25 * 168 = 4200. 4200 / (3 * 2^0) = 4200 / (3 * 1) = 1400
CPU:  1400 / (1 + 2) = 466
DRAM: 1400 / (2 * (1 + 1)) = 1400 / 4 = 350
CHK:  0xA8 + 0x53 + 0x23 + 0x02 = 0x120. 0x01 + 0x02 + 0x00 = 0x03. "0x3F"

500/375/100
PLL:  25 * 120 = 3000. 3000 / (2 * 2^0) = 3000 / (2 * 1) = 1500
CPU:  1500 / (1 + 2) = 500
DRAM: 1500 / (2 * (1 + 1)) = 1500 / 4 = 375
CHK:  0x78 + 0x52 + 0x23 + 0x02 = 0xEF. 0x0E + 0x0F = 0x1D. "0xDF"

// 500/375/125 (PCI_DIV=, PCI_Mode=0)
// PLL:  25 * 120 = 3000. 3000 / (2 * 2^0) = 3000 / (2 * 1) = 1500
// CPU:  1500 / (1 + 2) = 500
// DRAM: 1500 / (2 * (1 + 1)) = 1500 / 4 = 375
// CHK:  0x78 + 0x52 + 0x04 + 0x02 = 0xD1. 0x0D + 0x00 = 0x0D. "0xDF"
*/