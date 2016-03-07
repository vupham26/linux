/*
 * Thunderbolt Cactus Ridge driver - power management
 *
 * Copyright (c) 2014 Andreas Noever <andreas.noever@gmail.com>
 */

#include <linux/pci.h>
#include <linux/pm_runtime.h>

#include "tb.h"

static int nhi_suspend_noirq(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct tb *tb = pci_get_drvdata(pdev);
	thunderbolt_suspend(tb);
	return 0;
}

static int nhi_resume_noirq(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct tb *tb = pci_get_drvdata(pdev);
	thunderbolt_resume(tb);
	return 0;
}

/*
 * The tunneled pci bridges are siblings of us. Use resume_noirq to reenable
 * the tunnels asap. A corresponding pci quirk blocks the downstream bridges
 * resume_noirq until we are done.
 */
const struct dev_pm_ops nhi_pm_ops = {
	.suspend_noirq = nhi_suspend_noirq,
	.resume_noirq  = nhi_resume_noirq,
	.freeze_noirq  = nhi_suspend_noirq, /*
					     * we just disable hotplug, the
					     * pci-tunnels stay alive.
					     */
	.restore_noirq = nhi_resume_noirq,
};
