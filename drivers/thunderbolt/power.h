/*
 * Thunderbolt Cactus Ridge driver - power management
 *
 * Copyright (c) 2014 Andreas Noever <andreas.noever@gmail.com>
 */

#ifndef POWER_H
#define POWER_H

#include <linux/pm_runtime.h>

extern const struct dev_pm_ops nhi_pm_ops;

void nhi_runtime_pm_fini(struct tb_nhi *nhi);
void nhi_runtime_pm_init(struct tb_nhi *nhi);

#endif
