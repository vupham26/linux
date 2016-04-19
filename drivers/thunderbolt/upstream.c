/*
 * upstream.c - thunderbolt upstream bridge driver (powers controller up/down)
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
 *   * XRIP and XRIO methods, unused by OS X driver.
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
 * * The controller is powered down once all child devices have suspended and
 *   its autosuspend delay has elapsed. The delay is user configurable via
 *   sysfs and should be lower or equal to that of the NHI since hotplug events
 *   are not acted upon if the NHI has suspended but the controller has not yet
 *   powered down. The delay should not be zero to avoid frequent power changes
 *   (e.g. multiple times just for lspci -vv) since powering up takes 2 sec.
 *   (Powering down is almost instantaneous.)
 */

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/pcieport_if.h>
#include <linux/pm_runtime.h>

struct tb_upstream {
	struct pci_dev *nhi;
	struct pci_dev *dsb0;
	unsigned long long wake_gpe; /* hotplug interrupt during powerdown */
	acpi_handle set_power; /* method to power controller up/down */
	acpi_handle get_power; /* method to query power state */
};

int nhi_resume_noirq(struct device *dev);

static int upstream_resume_noirq(struct pcie_device *dev)
{
	struct tb_upstream *upstream = get_service_data(dev);

	if (!upstream->nhi->dev.driver)
		return 0;

	/*
	 * During suspend the thunderbolt controller is reset and all pci
	 * tunnels are lost. The NHI driver needs to reestablish all tunnels
	 * before the downstream bridges resume. There is no parent child
	 * relationship between the NHI and the tunneled bridges, but there is
	 * between them and the upstream bridge. Hence the NHI needs to be
	 * resumed by us rather than the PM core.
	 */
	pci_set_power_state(upstream->dsb0, PCI_D0);
	pci_restore_state(upstream->dsb0);
	pci_set_power_state(upstream->nhi, PCI_D0);
	pci_restore_state(upstream->nhi);
	return nhi_resume_noirq(&upstream->nhi->dev);
}

static int upstream_prepare(struct pcie_device *dev)
{
	struct tb_upstream *upstream = get_service_data(dev);

	/* prevent interrupts during system sleep transition */
	if (pm_runtime_suspended(&dev->port->dev) &&
	    ACPI_FAILURE(acpi_disable_gpe(NULL, upstream->wake_gpe))) {
		dev_err(&dev->device, "cannot disable wake GPE, resuming\n");
		pm_request_resume(&dev->port->dev);
	}

	return 0;
}

static int upstream_complete(struct pcie_device *dev)
{
	struct tb_upstream *upstream = get_service_data(dev);

	/*
	 * If the controller was powered up before system sleep, we'll find it
	 * automatically powered up afterwards.
	 */
	if (pm_runtime_active(&dev->port->dev))
		return 0;

	/*
	 * If the controller was powered down before system sleep, calling XRPE
	 * to power it up will fail on the next runtime resume. An additional
	 * call to XRPE is necessary to reset the power switch first.
	 */
	dev_info(&dev->device, "resetting power switch\n");
	if (ACPI_FAILURE(acpi_execute_simple_method(upstream->set_power, NULL,
						    0))) {
		dev_err(&dev->device, "cannot call set_power method\n");
		dev->port->dev.power.runtime_error = -ENODEV;
	}

	if (ACPI_FAILURE(acpi_enable_gpe(NULL, upstream->wake_gpe))) {
		dev_err(&dev->device, "cannot enable wake GPE, resuming\n");
		pm_request_resume(&dev->port->dev);
	}

	return 0;
}

static int pm_set_d3cold_cb(struct pci_dev *pdev, void *ptr)
{
	pdev->current_state = PCI_D3cold;
	return 0;
}

static int pm_set_d3hot_and_resume_cb(struct pci_dev *pdev, void *ptr)
{
	pdev->current_state = PCI_D3hot;
	WARN_ON(pm_request_resume(&pdev->dev) < 0);
	return 0;
}

static int upstream_runtime_suspend(struct pcie_device *dev)
{
	struct tb_upstream *upstream = get_service_data(dev);
	unsigned long long powered_down;
	int i;

	if (!dev->port->d3cold_allowed)
		return -EAGAIN;

	pci_save_state(dev->port);
	pci_walk_bus(dev->port->bus, pm_set_d3cold_cb, NULL);

	dev_info(&dev->device, "powering down\n");
	if (ACPI_FAILURE(acpi_execute_simple_method(upstream->set_power, NULL,
						    0))) {
		dev_err(&dev->device, "cannot call set_power method, resuming\n");
		goto err;
	}

	/*
	 * On gen 2 controllers, the wake GPE fires as long as the controller
	 * is powered up. Poll until it's powered down before enabling the GPE.
	 */
	for (i = 0; i < 300; i++) {
		if (ACPI_FAILURE(acpi_evaluate_integer(upstream->get_power,
						       NULL, NULL,
						       &powered_down))) {
			dev_err(&dev->device, "cannot call get_power method, resuming\n");
			goto err;
		}
		if (powered_down)
			break;
		usleep_range(800, 1600);
	}
	if (!powered_down) {
		dev_err(&dev->device, "refused to power down, resuming\n");
		goto err;
	}

	if (ACPI_FAILURE(acpi_enable_gpe(NULL, upstream->wake_gpe))) {
		dev_err(&dev->device, "cannot enable wake GPE, resuming\n");
		goto err;
	}

	return 0;

err:
	acpi_execute_simple_method(upstream->set_power, NULL, 1);
	dev->port->current_state = PCI_D0;
	pci_restore_state(dev->port);
	pci_walk_bus(dev->port->subordinate, pm_set_d3hot_and_resume_cb, NULL);
	return -EAGAIN;
}

static int upstream_runtime_resume(struct pcie_device *dev)
{
	struct tb_upstream *upstream = get_service_data(dev);

	if (system_state >= SYSTEM_HALT)
		return -ESHUTDOWN;

	if (ACPI_FAILURE(acpi_disable_gpe(NULL, upstream->wake_gpe))) {
		dev_err(&dev->device, "cannot disable wake GPE, disabling runtime pm\n");
		pm_runtime_get_noresume(&upstream->nhi->dev);
	}

	dev_info(&dev->device, "powering up\n");
	if (ACPI_FAILURE(acpi_execute_simple_method(upstream->set_power, NULL,
						    1))) {
		dev_err(&dev->device, "cannot call set_power method\n");
		return -ENODEV;
	}

	dev->port->current_state = PCI_D0;
	pci_restore_state(dev->port);

	/* wake children to force pci_restore_state() after D3cold */
	pci_walk_bus(dev->port->subordinate, pm_set_d3hot_and_resume_cb, NULL);
	return 0;
}

static u32 upstream_wake_nhi(acpi_handle gpe_device, u32 gpe_number, void *ctx)
{
	struct pci_dev *nhi = ctx;
	WARN_ON(pm_request_resume(&nhi->dev) < 0);
	return ACPI_INTERRUPT_HANDLED;
}

static int pm_init_cb(struct pci_dev *pdev, void *ptr)
{
	/* opt out of mandatory runtime resume after system sleep */
	pdev->dev.power.direct_complete_noresume = true;
	return 0;
}

extern struct pci_device_id nhi_ids[];

static int upstream_probe(struct pcie_device *dev)
{
	struct tb_upstream *upstream;
	struct acpi_handle *nhi_handle;

	/* host controllers only */
	if (!dev->port->bus->self ||
	    pci_pcie_type(dev->port->bus->self) != PCI_EXP_TYPE_ROOT_PORT)
		return -ENODEV;

	upstream = devm_kzalloc(&dev->device, sizeof(*upstream), GFP_KERNEL);
	if (!upstream)
		return -ENOMEM;

	/* find Downstream Bridge 0 and NHI */
	upstream->dsb0 = pci_get_slot(dev->port->subordinate, 0);
	if (!upstream->dsb0 || !upstream->dsb0->subordinate)
		goto err;
	upstream->nhi = pci_get_slot(upstream->dsb0->subordinate, 0);
	if (!upstream->nhi || !pci_match_id(nhi_ids, upstream->nhi))
		goto err;
	nhi_handle = ACPI_HANDLE(&upstream->nhi->dev);
	if (!nhi_handle)
		goto err;

	/* Macs introduced 2011/2012 have XRPE, 2013+ have TRPE */
	if (ACPI_FAILURE(acpi_get_handle(nhi_handle, "XRPE",
					 &upstream->set_power)) &&
	    ACPI_FAILURE(acpi_get_handle(nhi_handle, "TRPE",
					 &upstream->set_power))) {
		dev_err(&dev->device, "cannot find set_power method\n");
		goto err;
	}

	if (ACPI_FAILURE(acpi_get_handle(nhi_handle, "XRIL",
					 &upstream->get_power))) {
		dev_err(&dev->device, "cannot find get_power method\n");
		goto err;
	}

	if (ACPI_FAILURE(acpi_evaluate_integer(nhi_handle, "XRIN", NULL,
					       &upstream->wake_gpe))) {
		dev_err(&dev->device, "cannot find wake GPE\n");
		goto err;
	}

	if (ACPI_FAILURE(acpi_install_gpe_handler(NULL, upstream->wake_gpe,
						  ACPI_GPE_LEVEL_TRIGGERED,
						  upstream_wake_nhi,
						  upstream->nhi))) {
		dev_err(&dev->device, "cannot install GPE handler\n");
		goto err;
	}

	set_service_data(dev, upstream);
	pci_walk_bus(dev->port->bus, pm_init_cb, NULL);
	return 0;

err:
	pci_dev_put(upstream->nhi);
	pci_dev_put(upstream->dsb0);
	return -ENODEV;
}

static void upstream_remove(struct pcie_device *dev)
{
	struct tb_upstream *upstream = get_service_data(dev);

	if (ACPI_FAILURE(acpi_remove_gpe_handler(NULL, upstream->wake_gpe,
						 upstream_wake_nhi)))
		dev_err(&dev->device, "cannot remove GPE handler\n");

	pci_dev_put(upstream->nhi);
	pci_dev_put(upstream->dsb0);
	set_service_data(dev, NULL);
}

struct pcie_port_service_driver upstream_driver = {
	.name			= "thunderbolt_upstream",
	.port_type		= PCI_EXP_TYPE_UPSTREAM,
	.service		= PCIE_PORT_SERVICE_TBT,
	.probe			= upstream_probe,
	.remove			= upstream_remove,
	.prepare		= upstream_prepare,
	.complete		= upstream_complete,
	.runtime_suspend	= upstream_runtime_suspend,
	.runtime_resume		= upstream_runtime_resume,
	.resume_noirq		= upstream_resume_noirq,
};
