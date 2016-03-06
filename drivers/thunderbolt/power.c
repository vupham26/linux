/*
 * Thunderbolt Cactus Ridge driver - power management
 *
 * Copyright (c) 2014 Andreas Noever <andreas.noever@gmail.com>
 * Copyright (c) 2016 Lukas Wunner <lukas@wunner.de>
 */

#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>

#include "nhi.h"
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

/*
 * Runtime Power Management
 *
 * Apple provides the following means for runtime pm in ACPI:
 *
 * * XRPE method (TRPE on Cactus Ridge and newer), takes argument 1 or 0,
 *   toggles a GPIO pin to switch the controller on or off.
 * * XRIN named object (alternatively _GPE), contains number of a GPE which
 *   fires as long as something is plugged in (regardless of power state).
 * * XRIL method returns 0 as long as something is plugged in, 1 otherwise.
 * * XRIP + XRIO methods, unused by OS X driver. (Flip interrupt polarity?)
 *
 * If there are multiple Thunderbolt controllers (e.g. MacPro6,1), each NHI
 * device has a separate XRIN GPE and separate instances of these methods.
 *
 * We acquire a runtime pm ref for each newly allocated switch (except for
 * the root switch) and drop one when a switch is freed. The controller is
 * thus powered up as long as something is plugged in. This behaviour is
 * identical to the OS X driver.
 *
 * Powering the controller down is almost instantaneous, but powering up takes
 * about 2 sec. To handle situations gracefully where a device is unplugged
 * and immediately replaced by another one, we afford a grace period of 10 sec
 * before powering down. This autosuspend_delay_ms may be reduced to 0 via
 * sysfs and to handle that properly we need to wait during runtime_resume
 * since it takes about 0.7 sec after resuming until a hotplug event appears.
 *
 * When the system wakes from suspend-to-RAM, the controller's power state is
 * as it was before. However if it was powered down, calling XRPE once to power
 * it up is not sufficient: An additional call to XRPE is necessary to reset
 * the power switch first.
 */

static int nhi_prepare(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct tb *tb = pci_get_drvdata(pdev);
	acpi_status res;

	if (pm_runtime_active(dev))
		return 0;

	res = acpi_disable_gpe(NULL, tb->nhi->wake_gpe);
	if (ACPI_FAILURE(res)) {
		dev_err(dev, "cannot disable wake GPE, resuming\n");
		return 0;
	} else
		return 1; /* stay asleep if already runtime suspended */
}

static void nhi_complete(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct tb *tb = pci_get_drvdata(pdev);
	acpi_status res;

	if (pm_runtime_active(dev))
		return;

	tb_info(tb, "resetting power switch\n");
	res = acpi_execute_simple_method(tb->nhi->set_power, NULL, 0);
	if (ACPI_FAILURE(res)) {
		dev_err(dev, "cannot call set_power method\n");
		dev->power.runtime_error = -ENODEV;
	}

	res = acpi_enable_gpe(NULL, tb->nhi->wake_gpe);
	if (ACPI_FAILURE(res)) {
		dev_err(dev, "cannot enable wake GPE, resuming\n");
		pm_request_resume(dev);
	}
}

static int pci_save_state_cb(struct pci_dev *pdev, void *ptr)
{
	pci_save_state(pdev);
	if ((pdev->class >> 8) == PCI_CLASS_BRIDGE_PCI) {
		pm_runtime_disable(&pdev->dev);
		pm_runtime_set_suspended(&pdev->dev);
		pm_runtime_enable(&pdev->dev);
	}
	pdev->current_state = PCI_D3cold;
	return 0;
}

static int pci_restore_state_cb(struct pci_dev *pdev, void *ptr)
{
	pdev->current_state = PCI_D0;
	if ((pdev->class >> 8) == PCI_CLASS_BRIDGE_PCI) {
		pm_runtime_disable(&pdev->dev);
		pm_runtime_set_active(&pdev->dev);
		pm_runtime_enable(&pdev->dev);
	}
	pci_restore_state(pdev);
	return 0;
}

static int nhi_runtime_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct pci_bus *upstream_bridge = pdev->bus->parent->parent;
	struct tb *tb = pci_get_drvdata(pdev);
	acpi_status res;

	if (!pdev->d3cold_allowed)
		return -EAGAIN;

	thunderbolt_suspend(tb);
	pci_walk_bus(upstream_bridge, pci_save_state_cb, NULL);

	tb_info(tb, "powering down\n");
	res = acpi_execute_simple_method(tb->nhi->set_power, NULL, 0);
	if (ACPI_FAILURE(res)) {
		dev_err(dev, "cannot call set_power method, resuming\n");
		goto err;
	}

	res = acpi_enable_gpe(NULL, tb->nhi->wake_gpe);
	if (ACPI_FAILURE(res)) {
		dev_err(dev, "cannot enable wake GPE, resuming\n");
		goto err;
	}

	return 0;

err:
	acpi_execute_simple_method(tb->nhi->set_power, NULL, 1);
	pci_walk_bus(upstream_bridge, pci_restore_state_cb, NULL);
	thunderbolt_resume(tb);
	return -EAGAIN;
}

static int nhi_runtime_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct pci_bus *upstream_bridge = pdev->bus->parent->parent;
	struct tb *tb = pci_get_drvdata(pdev);
	acpi_status res;

	if (system_state >= SYSTEM_HALT)
		return -ESHUTDOWN;

	res = acpi_disable_gpe(NULL, tb->nhi->wake_gpe);
	if (ACPI_FAILURE(res)) {
		dev_err(dev, "cannot disable wake GPE, disabling runtime pm\n");
		pm_runtime_disable(dev);
	}

	tb_info(tb, "powering up\n");
	res = acpi_execute_simple_method(tb->nhi->set_power, NULL, 1);
	if (ACPI_FAILURE(res)) {
		dev_err(dev, "cannot call set_power method\n");
		return -ENODEV;
	}

	pci_walk_bus(upstream_bridge, pci_restore_state_cb, NULL);
	thunderbolt_resume(tb);
	msleep(1500); /* allow 1.5 sec for hotplug event to arrive */
	pm_runtime_mark_last_busy(dev);

	return 0;
}

static u32 nhi_runtime_wake(acpi_handle gpe_device, u32 gpe_number, void *ctx)
{
	struct device *dev = ctx;
	WARN_ON(pm_request_resume(dev) < 0);
	return ACPI_INTERRUPT_HANDLED;
}

static struct dev_pm_domain nhi_pm_domain;

void nhi_runtime_pm_init(struct tb_nhi *nhi)
{
	struct device *dev = &nhi->pdev->dev;
	struct acpi_handle *nhi_handle = ACPI_HANDLE(dev);
	acpi_status res;

	/* gen 1 controllers use XRPE, gen 2+ controllers use TRPE */
	if (nhi->pdev->device <= PCI_DEVICE_ID_INTEL_EAGLE_RIDGE)
		res = acpi_get_handle(nhi_handle, "XRPE", &nhi->set_power);
	else
		res = acpi_get_handle(nhi_handle, "TRPE", &nhi->set_power);
	if (ACPI_FAILURE(res)) {
		dev_warn(dev, "cannot find set_power method, disabling runtime pm\n");
		goto err;
	}

	res = acpi_evaluate_integer(nhi_handle, "XRIN", NULL, &nhi->wake_gpe);
	if (ACPI_FAILURE(res)) {
		dev_warn(dev, "cannot find wake GPE, disabling runtime pm\n");
		goto err;
	}

	res = acpi_install_gpe_handler(NULL, nhi->wake_gpe,
				       ACPI_GPE_LEVEL_TRIGGERED,
				       nhi_runtime_wake, dev);
	if (ACPI_FAILURE(res)) {
		dev_warn(dev, "cannot install GPE handler, disabling runtime pm\n");
		goto err;
	}

	nhi_pm_domain.ops		  = *pci_bus_type.pm;
	nhi_pm_domain.ops.prepare	  = nhi_prepare;
	nhi_pm_domain.ops.complete	  = nhi_complete;
	nhi_pm_domain.ops.runtime_suspend = nhi_runtime_suspend;
	nhi_pm_domain.ops.runtime_resume  = nhi_runtime_resume;
	dev_pm_domain_set(dev, &nhi_pm_domain);

	/* apply to upstream bridge and downstream bridge 0 */
	pm_suspend_ignore_children(dev->parent->parent, true);
	pm_suspend_ignore_children(dev->parent, true);

	pm_runtime_allow(dev);
	pm_runtime_set_autosuspend_delay(dev, 10000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put(dev);
	return;

err:
	nhi->wake_gpe = -1;
	if (pm_runtime_enabled(dev))
		pm_runtime_disable(dev);
}

void nhi_runtime_pm_fini(struct tb_nhi *nhi)
{
	struct device *dev = &nhi->pdev->dev;
	acpi_status res;

	if (nhi->wake_gpe == -1)
		return;

	res = acpi_remove_gpe_handler(NULL, nhi->wake_gpe, nhi_runtime_wake);
	if (ACPI_FAILURE(res))
		dev_warn(dev, "cannot remove GPE handler\n");

	pm_runtime_get(dev);
	pm_runtime_forbid(dev);
	dev_pm_domain_set(dev, NULL);
}
