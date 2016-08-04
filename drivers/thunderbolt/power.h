/*
 * power.h - power thunderbolt controller down when idle
 * Copyright (C) 2016 Lukas Wunner <lukas@wunner.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License (version 2) as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef THUNDERBOLT_POWER_H
#define THUNDERBOLT_POWER_H

#include <linux/acpi.h>
#include <linux/pm_domain.h>

#include "tb.h"

struct tb_power {
	struct tb *tb;
	struct dev_pm_domain pm_domain; /* assigned to upstream bridge */
	unsigned long long wake_gpe; /* hotplug interrupt during powerdown */
	acpi_handle set; /* method to power controller up/down */
	acpi_handle get; /* method to query power state */
};

void thunderbolt_power_init(struct tb *tb);
void thunderbolt_power_fini(struct tb *tb);

#endif
