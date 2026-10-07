/*
 * VMApple "avp,rtc" real-time clock
 *
 * The clock Virtualization.framework gives macOS guests. Without it the
 * guest only has the PL031, macOS reports "RTC reset likely", timed may
 * adopt the image's last filesystem timestamp, and boots can stall.
 *
 * Layout reverse-engineered from macOS 26.4 (AppleVirtualPlatformRTC in
 * com.apple.driver.AppleVirtualPlatform, and iBootStage2 for vma2):
 *
 *   0x00  R   64-bit time in microseconds; the guest's wall clock is this
 *             plus the offset it keeps in NVRAM com.apple.System.rtc-offset
 *             (zero if never set), so it counts from the Unix epoch here
 *   0x18  RW  control, bit 0 = enable (set by the driver at start)
 *   0x20  RW  interrupt enable mask (driver writes 0, later 0x6)
 *   0x30  R   interrupt status (bit 1, bit 2: events forwarded to timed)
 *   0x38  W   interrupt acknowledge, write-1-to-clear
 *   0x40  R   ID: iBoot keeps the avp-rtc device tree node (and deletes
 *             pl031-rtc) only if this reads 0xf001
 *
 * This model never raises an interrupt: host time changes are not signalled.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/vmapple/vmapple.h"
#include "migration/vmstate.h"
#include "trace.h"

OBJECT_DECLARE_SIMPLE_TYPE(VMAppleAvpRtcState, VMAPPLE_AVP_RTC)

#define AVP_RTC_TIME        0x00
#define AVP_RTC_CONTROL     0x18
#define AVP_RTC_IRQ_ENABLE  0x20
#define AVP_RTC_IRQ_STATUS  0x30
#define AVP_RTC_IRQ_ACK     0x38
#define AVP_RTC_ID          0x40
#define AVP_RTC_ID_VALUE    0xf001

struct VMAppleAvpRtcState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    uint64_t control;
    uint64_t irq_enable;
    uint64_t irq_status;
};

static void avp_rtc_update_irq(VMAppleAvpRtcState *s)
{
    qemu_set_irq(s->irq, !!(s->irq_status & s->irq_enable));
}

static uint64_t avp_rtc_read(void *opaque, hwaddr offset, unsigned size)
{
    VMAppleAvpRtcState *s = opaque;
    uint64_t val;

    switch (offset) {
    case AVP_RTC_TIME:
        val = (uint64_t)qemu_clock_get_us(QEMU_CLOCK_HOST);
        break;
    case AVP_RTC_CONTROL:
        val = s->control;
        break;
    case AVP_RTC_IRQ_ENABLE:
        val = s->irq_enable;
        break;
    case AVP_RTC_IRQ_STATUS:
        val = s->irq_status;
        break;
    case AVP_RTC_ID:
        val = AVP_RTC_ID_VALUE;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx
                      " size %u\n", __func__, offset, size);
        val = 0;
        break;
    }
    trace_avp_rtc_read(offset, size, val);
    return val;
}

static void avp_rtc_write(void *opaque, hwaddr offset, uint64_t val,
                          unsigned size)
{
    VMAppleAvpRtcState *s = opaque;

    trace_avp_rtc_write(offset, size, val);
    switch (offset) {
    case AVP_RTC_CONTROL:
        s->control = val;
        break;
    case AVP_RTC_IRQ_ENABLE:
        s->irq_enable = val;
        avp_rtc_update_irq(s);
        break;
    case AVP_RTC_IRQ_ACK:
        s->irq_status &= ~val;
        avp_rtc_update_irq(s);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx
                      " size %u value 0x%" PRIx64 "\n", __func__, offset, size,
                      val);
        break;
    }
}

static const MemoryRegionOps avp_rtc_ops = {
    .read = avp_rtc_read,
    .write = avp_rtc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

static void avp_rtc_reset(DeviceState *dev)
{
    VMAppleAvpRtcState *s = VMAPPLE_AVP_RTC(dev);

    s->control = 0;
    s->irq_enable = 0;
    s->irq_status = 0;
}

static void avp_rtc_init(Object *obj)
{
    VMAppleAvpRtcState *s = VMAPPLE_AVP_RTC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &avp_rtc_ops, s,
                          TYPE_VMAPPLE_AVP_RTC, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static const VMStateDescription vmstate_avp_rtc = {
    .name = TYPE_VMAPPLE_AVP_RTC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(control, VMAppleAvpRtcState),
        VMSTATE_UINT64(irq_enable, VMAppleAvpRtcState),
        VMSTATE_UINT64(irq_status, VMAppleAvpRtcState),
        VMSTATE_END_OF_LIST()
    },
};

static void avp_rtc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, avp_rtc_reset);
    dc->vmsd = &vmstate_avp_rtc;
    dc->desc = "VMApple real-time clock (avp,rtc)";
}

static const TypeInfo avp_rtc_info = {
    .name          = TYPE_VMAPPLE_AVP_RTC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(VMAppleAvpRtcState),
    .instance_init = avp_rtc_init,
    .class_init    = avp_rtc_class_init,
};

static void avp_rtc_register_types(void)
{
    type_register_static(&avp_rtc_info);
}

type_init(avp_rtc_register_types)
