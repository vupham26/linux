/*
 * power.c - power thunderbolt controller down when idle
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

/*
 * Apple provides the following means for power control in ACPI:
 *
 * * On Macs with Thunderbolt 1 Gen 1 controllers (Light Ridge, Eagle Ridge):
 *   * XRPE method ("Power Enable"), takes argument 1 or 0, toggles a GPIO pin
 *     to switch the controller on or off.
 *   * XRIN named object (alternatively _GPE), contains number of a GPE which
 *     fires as long as something is plugged in (regardless of power state).
 *   * XRIL method ("Interrupt Low"), returns 0 as long as something is
 *     plugged in, 1 otherwise.
 *   * XRIP and XRIO methods, unused by macOS driver.
 *
 * * On Macs with Thunderbolt 1 Gen 2 controllers (Cactus Ridge 4C):
 *   * XRIN not only fires as long as something is plugged in, but also as long
 *     as the controller's CIO switch is powered up.
 *   * XRIL method changed its meaning, it returns 0 as long as the CIO switch
 *     is powered up, 1 otherwise.
 *   * Additional SXFP method ("Force Power"), accepts only argument 0,
 *     switches the controller off. This carries out just the raw power change,
 *     unlike XRPE which disables the link on the PCIe Root Port in an orderly
 *     fashion before switching off the controller.
 *   * Additional SXLV, SXIO, SXIL methods to utilize the Go2Sx and Ok2Go2Sx
 *     pins (see background below). Apparently SXLV toggles the value given to
 *     the POC via Go2Sx (0 or 1), SXIO changes the direction (0 or 1) and SXIL
 *     returns the value received from the POC via Ok2Go2Sx.
 *   * On some Macs, additional XRST method, takes argument 1 or 0, asserts or
 *     deasserts a GPIO pin to reset the controller.
 *   * On Macs introduced 2013, XRPE was renamed TRPE.
 *
 * * On Macs with Thunderbolt 2 controllers (Falcon Ridge 4C and 2C):
 *   * SXLV, SXIO, SXIL methods to utilize Go2Sx and Ok2Go2Sx are gone.
 *   * On the MacPro6,1 which has multiple Thunderbolt controllers, each NHI
 *     device has a separate XRIN GPE and separate TRPE, SXFP and XRIL methods.
 *
 * Background:
 *
 * * Gen 1 controllers (Light Ridge, Eagle Ridge) had no power management
 *   and no ability to distinguish whether a DP or Thunderbolt device is
 *   plugged in. Apple put an ARM Cortex MCU (NXP LPC1112A) on the logic board
 *   which snoops on the connector lines and, depending on the type of device,
 *   sends an HPD signal to the GPU or rings the Thunderbolt XRIN doorbell
 *   interrupt. The switches for the 3.3V and 1.05V power rails to the
 *   Thunderbolt controller are toggled by a GPIO pin on the southbridge.
 *
 * * On gen 2 controllers (Cactus Ridge 4C), Intel integrated the MCU into the
 *   controller and called it POC. This caused a change of semantics for XRIN
 *   and XRIL. The POC is powered by a separate 3.3V rail which is active even
 *   in sleep state S4. It only draws a very small current. The regular 3.3V
 *   and 1.05V power rails are no longer controlled by the southbridge but by
 *   the POC. In other words the controller powers *itself* up and down! It is
 *   instructed to do so with the Go2Sx pin. Another pin, Ok2Go2Sx, allows the
 *   controller to indicate if it is ready to power itself down. Apple wires
 *   Go2Sx and Ok2Go2Sx to the same GPIO pin on the southbridge, hence the pin
 *   is used bidirectionally. A third pin, Force Power, is intended by Intel
 *   for debug only but Apple abuses it for XRPE/TRPE and SXFP. Perhaps it
 *   leads to larger power saving gains. They utilize Go2Sx and Ok2Go2Sx only
 *   on Cactus Ridge, presumably because the controller somehow requires that.
 *   On Falcon Ridge they forego these pins and rely solely on Force Power.
 *
 * Implementation Notes:
 *
 * * To conform to Linux' hierarchical power management model, power control
 *   is governed by the topmost PCI device of the controller, which is the
 *   upstream bridge. The controller is powered down once all child devices
 *   of the upstream bridge have suspended and its autosuspend delay has
 *   elapsed.
 *
 * * The autosuspend delay is user configurable via sysfs and should be lower
 *   or equal to that of the NHI since hotplug events are not acted upon if
 *   the NHI has suspended but the controller has not yet powered down.
 *   However the delay should not be zero to avoid frequent power changes
 *   (e.g. multiple times just for lspci -vv) since powering up takes 2 sec.
 *   (Powering down is almost instantaneous.)
 */

#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>

#include "power.h"

#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME " %s: " fmt, dev_name(dev)

#define to_power(dev) container_of(dev->pm_domain, struct tb_power, pm_domain)

static int upstream_prepare(struct device *dev)
{
	struct tb_power *power = to_power(dev);

	if (pm_runtime_active(dev))
		return 0;

	/* prevent interrupts during system sleep transition */
	if (ACPI_FAILURE(acpi_disable_gpe(NULL, power->wake_gpe))) {
		pr_err("cannot disable wake GPE, resuming\n");
		pm_request_resume(dev);
		return -EAGAIN;
	}

	return DPM_DIRECT_COMPLETE;
}

static void upstream_complete(struct device *dev)
{
	struct tb_power *power = to_power(dev);

	if (pm_runtime_active(dev))
		return;

	/*
	 * If the controller was powered down before system sleep, calling XRPE
	 * to power it up will fail on the next runtime resume. An additional
	 * call to XRPE is necessary to reset the power switch first.
	 */
	pr_info("resetting power switch\n");
	if (ACPI_FAILURE(acpi_execute_simple_method(power->set, NULL, 0))) {
		pr_err("cannot call power->set method\n");
		dev->power.runtime_error = -EIO;
	}

	if (ACPI_FAILURE(acpi_enable_gpe(NULL, power->wake_gpe))) {
		pr_err("cannot enable wake GPE, resuming\n");
		pm_request_resume(dev);
	}
}

static int set_d3cold(struct pci_dev *pdev, void *ptr)
{
	pdev->current_state = PCI_D3cold;
	return 0;
}

static int request_resume(struct pci_dev *pdev, void *ptr)
{
	WARN_ON(pm_request_resume(&pdev->dev) < 0);
	return 0;
}

static int upstream_runtime_suspend(struct device *dev)
{
	struct tb_power *power = to_power(dev);
	struct pci_dev *pdev = to_pci_dev(dev);
	unsigned long long powered_down;
	int ret, i;

	/* children are effectively in D3cold once upstream goes to D3hot */
	pci_walk_bus(pdev->subordinate, set_d3cold, NULL);

	ret = dev->bus->pm->runtime_suspend(dev);
	if (ret) {
		pci_walk_bus(pdev->subordinate, request_resume, NULL);
		return ret;
	}

	pr_info("powering down\n");
	pdev->current_state = PCI_D3cold;
	if (ACPI_FAILURE(acpi_execute_simple_method(power->set, NULL, 0))) {
		pr_err("cannot call power->set method, resuming\n");
		goto err_resume;
	}

	/*
	 * On gen 2 controllers, the wake GPE fires as long as the controller
	 * is powered up. Poll until it's powered down before enabling the GPE.
	 * macOS polls up to 300 times with a 1 ms delay, just mimic that.
	 */
	for (i = 0; i < 300; i++) {
		if (ACPI_FAILURE(acpi_evaluate_integer(power->get,
					      NULL, NULL, &powered_down))) {
			pr_err("cannot call power->get method, resuming\n");
			goto err_resume;
		}
		if (powered_down)
			break;
		usleep_range(800, 1200);
	}
	if (!powered_down) {
		pr_err("refused to power down, resuming\n");
		goto err_resume;
	}

	if (ACPI_FAILURE(acpi_enable_gpe(NULL, power->wake_gpe))) {
		pr_err("cannot enable wake GPE, resuming\n");
		goto err_resume;
	}

	return 0;

err_resume:
	acpi_execute_simple_method(power->set, NULL, 1);
	dev->bus->pm->runtime_resume(dev);
	pci_walk_bus(pdev->subordinate, request_resume, NULL);
	return -EAGAIN;
}

static int upstream_runtime_resume(struct device *dev)
{
	struct tb_power *power = to_power(dev);
	struct pci_dev *pdev = to_pci_dev(dev);
	int ret;

	if (!dev->power.is_prepared &&
	    ACPI_FAILURE(acpi_disable_gpe(NULL, power->wake_gpe))) {
		pr_err("cannot disable wake GPE, disabling runtime pm\n");
		pm_runtime_get_noresume(&power->tb->nhi->pdev->dev);
	}

	pr_info("powering up\n");
	if (ACPI_FAILURE(acpi_execute_simple_method(power->set, NULL, 1))) {
		pr_err("cannot call power->set method\n");
		return -ENODEV;
	}

	ret = dev->bus->pm->runtime_resume(dev);

	/* wake children to force pci_restore_state() after D3cold */
	pci_walk_bus(pdev->subordinate, request_resume, NULL);

	return ret;
}

static u32 nhi_wake(acpi_handle gpe_device, u32 gpe_number, void *ctx)
{
	struct device *nhi_dev = ctx;
	WARN_ON(pm_request_resume(nhi_dev) < 0);
	return ACPI_INTERRUPT_HANDLED;
}

static int disable_pme_poll(struct pci_dev *pdev, void *ptr)
{
	struct pci_bus *downstream_bus = (struct pci_bus *)ptr;

	/* PME# pin is not connected, the wake GPE is used instead */
	if (pdev->bus == downstream_bus	||		/* downstream bridge */
	    pdev->subordinate == downstream_bus ||	  /* upstream bridge */
	    (pdev->bus->parent == downstream_bus &&
	     pdev->class == PCI_CLASS_SYSTEM_OTHER << 8))	      /* NHI */
		pdev->pme_poll = false;

	return 0;
}

void thunderbolt_power_init(struct tb *tb)
{
	struct device *upstream_dev, *nhi_dev = &tb->nhi->pdev->dev;
	struct tb_power *power = NULL;
	struct acpi_handle *nhi_handle;

	power = kzalloc(sizeof(*power), GFP_KERNEL);
	if (!power) {
		dev_err(nhi_dev, "cannot allocate power data\n");
		goto err_free;
	}

	nhi_handle = ACPI_HANDLE(nhi_dev);
	if (!nhi_handle) {
		dev_err(nhi_dev, "cannot find ACPI handle\n");
		goto err_free;
	}

	if (!nhi_dev->parent || !nhi_dev->parent->parent) {
		dev_err(nhi_dev, "cannot find upstream bridge\n");
		goto err_free;
	}
	upstream_dev = nhi_dev->parent->parent;

	/* Macs introduced 2011/2012 have XRPE, 2013+ have TRPE */
	if (ACPI_FAILURE(acpi_get_handle(nhi_handle, "XRPE", &power->set)) &&
	    ACPI_FAILURE(acpi_get_handle(nhi_handle, "TRPE", &power->set))) {
		dev_err(nhi_dev, "cannot find power->set method\n");
		goto err_free;
	}

	if (ACPI_FAILURE(acpi_get_handle(nhi_handle, "XRIL", &power->get))) {
		dev_err(nhi_dev, "cannot find power->get method\n");
		goto err_free;
	}

	if (ACPI_FAILURE(acpi_evaluate_integer(nhi_handle, "XRIN", NULL,
							&power->wake_gpe))) {
		dev_err(nhi_dev, "cannot find wake GPE\n");
		goto err_free;
	}

	if (ACPI_FAILURE(acpi_install_gpe_handler(NULL, power->wake_gpe,
			     ACPI_GPE_LEVEL_TRIGGERED, nhi_wake, nhi_dev))) {
		dev_err(nhi_dev, "cannot install GPE handler\n");
		goto err_free;
	}

	pci_walk_bus(to_pci_dev(upstream_dev)->bus, disable_pme_poll,
		     to_pci_dev(upstream_dev)->subordinate);

	power->pm_domain.ops		     = *upstream_dev->bus->pm;
	power->pm_domain.ops.prepare	     =  upstream_prepare;
	power->pm_domain.ops.complete	     =  upstream_complete;
	power->pm_domain.ops.runtime_suspend =  upstream_runtime_suspend;
	power->pm_domain.ops.runtime_resume  =  upstream_runtime_resume;
	power->tb			     =  tb;
	dev_pm_domain_set(upstream_dev, &power->pm_domain);

	tb->power = power;

	return;

err_free:
	dev_err(nhi_dev, "controller will stay powered up permanently\n");
	kfree(power);
}

void thunderbolt_power_fini(struct tb *tb)
{
	struct device *nhi_dev = &tb->nhi->pdev->dev;
	struct device *upstream_dev = nhi_dev->parent->parent;
	struct tb_power *power = tb->power;

	if (!power)
		return; /* thunderbolt_power_init() failed */

	tb->power = NULL;
	dev_pm_domain_set(upstream_dev, NULL);

	if (ACPI_FAILURE(acpi_remove_gpe_handler(NULL, power->wake_gpe,
						 nhi_wake)))
		dev_err(nhi_dev, "cannot remove GPE handler\n");

	kfree(power);
}
