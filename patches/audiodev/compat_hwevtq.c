/*
 * compat_hwevtq.c — reconstruction shim for the Moggie build.
 *
 * button_sim.c (from Sonos's common.txz, a newer generation than this kernel)
 * calls hwevtq_send_event() and hwevtq_send_event_defer(). Those live in a
 * hardware-event-queue module that Sonos did not publish in the GPL drop, and
 * the shipped 57.10 audiodev.ko has no reference to them at all.
 *
 * Rather than invent behaviour, these are no-ops. Button simulation is a
 * diagnostic path; the physical button path in fenway-gpio.c is unaffected.
 *
 * If a real hwevtq implementation is ever published, drop this file and remove
 * it from Kbuild.
 */

#include <linux/kernel.h>
#include <linux/module.h>

void hwevtq_send_event(int source, int info)
{
    printk_once(KERN_INFO "moggie: hwevtq_send_event(%d,%d) stub (no event queue built in)\n",
                source, info);
}

void hwevtq_send_event_defer(int source, int info)
{
    printk_once(KERN_INFO "moggie: hwevtq_send_event_defer(%d,%d) stub (no event queue built in)\n",
                source, info);
}
