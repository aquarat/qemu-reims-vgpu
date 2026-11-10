/*
 * QTest for virtio-pci's x-ascending-capabilities property
 *
 * With the property on, a virtio PCI device's capability list is linked in
 * ascending address order; the capabilities themselves keep their offsets
 * and contents. Off (the default), the list is QEMU's usual one.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "hw/pci/pci_regs.h"

/* arm virt with highmem=off: the PCIe ECAM window, bus 0 */
#define ECAM_BASE 0x3f000000ULL
#define SLOT 2
#define CFG(off) (ECAM_BASE + ((uint64_t)(SLOT << 3) << 12) + (off))
#define MAX_CAPS 48

typedef struct CapList {
    int n;
    uint8_t offset[MAX_CAPS];
    uint8_t bytes[MAX_CAPS][4]; /* ID, next, and two bytes of contents */
} CapList;

static void read_caps(const char *device, const char *extra, CapList *l)
{
    QTestState *qts = qtest_initf("-M virt,highmem=off -nodefaults %s "
                                  "-device %s,addr=%d.0",
                                  extra, device, SLOT);
    uint8_t off;
    int i;

    g_assert(qtest_readw(qts, CFG(PCI_STATUS)) & PCI_STATUS_CAP_LIST);
    l->n = 0;
    for (off = qtest_readb(qts, CFG(PCI_CAPABILITY_LIST)) & ~3; off;
         off = qtest_readb(qts, CFG(off + PCI_CAP_LIST_NEXT)) & ~3) {
        g_assert_cmpint(l->n, <, MAX_CAPS);
        g_assert_cmpint(off, >=, 0x40); /* past the standard header */
        l->offset[l->n] = off;
        for (i = 0; i < 4; i++) {
            l->bytes[l->n][i] = qtest_readb(qts, CFG(off + i));
        }
        l->n++;
    }
    qtest_quit(qts);
}

static int find(const CapList *l, uint8_t offset)
{
    int i;

    for (i = 0; i < l->n; i++) {
        if (l->offset[i] == offset) {
            return i;
        }
    }
    return -1;
}

static void check(const char *device, const char *drive, const char *on)
{
    g_autofree char *args = g_strdup_printf("%s %s", drive, on);
    CapList def, asc;
    int i, j;

    read_caps(device, drive, &def);
    read_caps(device, args, &asc);

    /* Some virtio PCI device lists more than one capability. */
    g_assert_cmpint(def.n, >, 1);
    g_assert_cmpint(asc.n, ==, def.n);
    for (i = 0; i < asc.n; i++) {
        if (i > 0) {
            g_assert_cmpint(asc.offset[i], >, asc.offset[i - 1]);
        }
        /* The same capability, at the same offset, with the same contents. */
        j = find(&def, asc.offset[i]);
        g_assert_cmpint(j, >=, 0);
        g_assert_cmpint(asc.bytes[i][0], ==, def.bytes[j][0]);
        g_assert_cmpint(asc.bytes[i][2], ==, def.bytes[j][2]);
        g_assert_cmpint(asc.bytes[i][3], ==, def.bytes[j][3]);
    }
    /* QEMU's own order is high to low; the default must not change. */
    for (i = 1; i < def.n; i++) {
        g_assert_cmpint(def.offset[i], <, def.offset[i - 1]);
    }
}

static void test_property(const void *data)
{
    const char *device = data;
    g_autofree char *on = g_strdup_printf("-global %s.x-ascending-capabilities=on",
                                          device);

    check(device, "", on);
}

static void test_global(const void *data)
{
    check("virtio-rng-pci", "",
          "-global virtio-pci.x-ascending-capabilities=on");
}

static void test_vmapple_blk(const void *data)
{
    check("vmapple-virtio-blk-pci,variant=root,drive=d",
          "-blockdev null-co,node-name=d",
          "-global virtio-pci.x-ascending-capabilities=on");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine("virt") || !qtest_has_device("virtio-rng-pci")) {
        g_test_skip("no virt machine or virtio-rng-pci");
        return g_test_run();
    }
    qtest_add_data_func("/virtio-pci/cap-order/rng", "virtio-rng-pci",
                        test_property);
    qtest_add_data_func("/virtio-pci/cap-order/global", NULL, test_global);
    if (qtest_has_device("virtio-net-pci")) {
        qtest_add_data_func("/virtio-pci/cap-order/net", "virtio-net-pci",
                            test_property);
    }
    if (qtest_has_device("vmapple-virtio-blk-pci")) {
        qtest_add_data_func("/virtio-pci/cap-order/vmapple-blk", NULL,
                            test_vmapple_blk);
    }
    return g_test_run();
}
