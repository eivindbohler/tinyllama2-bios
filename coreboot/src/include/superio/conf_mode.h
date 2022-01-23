/*
 * This file is part of the coreboot project.
 *
 * Copyright (C) 2013 Nico Huber <nico.h@gmx.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef DEVICE_PNP_CONF_MODE_H
#define DEVICE_PNP_CONF_MODE_H

#include <device/device.h>
#include <device/pnp.h>

/* Common enter/exit implementations */
void pnp_enter_conf_mode_55(device_t dev);
void pnp_enter_conf_mode_8787(device_t dev);
void pnp_exit_conf_mode_aa(device_t dev);

extern const struct pnp_mode_ops pnp_conf_mode_55_aa;
extern const struct pnp_mode_ops pnp_conf_mode_8787_aa;

#endif
