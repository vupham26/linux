============
Device links
============

By default, the driver core only enforces dependencies between devices
that are borne out of a parent/child relationship within the device
hierarchy: When suspending, resuming or shutting down the system, devices
are ordered based on this relationship, i.e. children are always suspended
before their parent, and the parent is always resumed before its children.

Sometimes there is a need to represent device dependencies beyond the
mere parent/child relationship, e.g. between siblings, and have the
driver core automatically take care of them.

Secondly, the driver core by default does not enforce any driver presence
dependencies, i.e. that one device must be bound to a driver before
another one can probe or function correctly.

Often these two dependency types come together, so a device depends on
another one both with regards to driver presence *and* with regards to
suspend/resume and shutdown ordering.

Device links allow representation of such dependencies in the driver core.

In its standard form, a device link combines *both* dependency types:
It guarantees correct suspend/resume and shutdown ordering between a
"supplier" device and its "consumer" devices, and it guarantees driver
presence on the supplier.  The consumer devices are not probed before the
supplier is bound to a driver, and they're unbound before the supplier
is unbound.

When driver presence on the supplier is irrelevant and only correct
suspend/resume and shutdown ordering is needed, the device link may
simply be set up with the DEVICE_LINK_STATELESS flag.

A driver presence dependency between parent and child, i.e. within the
regular device hierarchy, could in principle also be represented in the
driver core using a device link.

If a device link is set up with the DEVICE_LINK_AUTOREMOVE flag, it is
automatically purged when the consumer fails to probe or later unbinds.
This is handy when adding a device link from the consumer's ->probe hook,
as it obviates the need to delete the link in the ->remove hook or in
the error path of the ->probe hook.


State machine
=============

"""
                .=============================.
                |                             |
                v                             |
DORMANT <=> AVAILABLE <=> CONSUMER_PROBE => ACTIVE
   ^                                          |
   |                                          |
   '============ SUPPLIER_UNBIND <============'
"""

* The initial state of a device link is passed in to device_link_add().
  If the link is created before any devices are probed, it must be set to
  DEVICE_LINK_DORMANT.

* When a supplier device is bound to a driver, links to its consumers
  progress to DEVICE_LINK_AVAILABLE.
  (Call to device_links_driver_bound() from driver_bound().)

* Before a consumer device is probed, presence of supplier drivers is
  verified by checking that links to suppliers are in DEVICE_LINK_AVAILABLE
  state.  The state of the links is updated to DEVICE_LINK_CONSUMER_PROBE.
  (Call to device_links_check_suppliers() from driver_probe_device().)
  This prevents the supplier from unbinding.
  (Call to wait_for_device_probe() in device_links_unbind_consumers().)

* If the probe fails, links to suppliers revert back to DEVICE_LINK_AVAILABLE.
  (Call to device_links_no_driver() from really_probe().)

* If the probe succeeds, links to suppliers progress to DEVICE_LINK_ACTIVE.
  (Call of device_links_driver_bound() from driver_bound().)

* When the consumer's driver is later on removed, links to suppliers revert
  back to DEVICE_LINK_AVAILABLE.
  (Call to device_links_no_driver() from __device_release_driver().)

* Before a supplier's driver is removed, links to consumers that are not
  bound to a driver are updated to DEVICE_LINK_SUPPLIER_UNBIND.
  (Call to device_links_busy() from __device_release_driver().)
  This prevents the consumers from binding.
  (Call to device_links_check_suppliers() from driver_probe_device().)
  Consumers that are bound are freed from their driver; consumers that are
  probing are waited for until they are done.
  (Call to device_links_unbind_consumers() from __device_release_driver().)
  Once all links to consumers are in DEVICE_LINK_SUPPLIER_UNBIND state,
  the supplier driver is released and the links revert to DEVICE_LINK_DORMANT.
  (Call to device_links_driver_gone() from __device_release_driver().)
