/*
 * VMApple guests under KVM: the platform's pointer-authentication hypercalls.
 *
 * Copyright (c) 2026 aquarat
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * macOS on the vmapple platform manages its pointer-authentication keys
 * through vendor hypercalls in the SMCCC range 0xc1000000..0xc10000ff,
 * which Apple's hypervisor implements. Under KVM they are forwarded to
 * userspace (an SMCCC filter set at VM creation) and served here by writing
 * the guest's key registers. Two of those registers (APCTL_EL12 and the
 * kernel key pair) are Apple implementation-defined and exist only on
 * hosts whose kernel exposes them to KVM.
 *
 * Function numbers (the low byte of the call):
 *   0  set the initial state: keys to their defaults, EL0 keys not shared
 *   1  report the default A, B, EL0 and G keys in x1..x4
 *   2  set the A keys from x1        3  set the B keys from x1
 *   4  set the EL0 key from x1       5  set the EL0 key from x2 and whether
 *                                       EL0 shares the EL1 keys from x1
 *   6  set the G key from x1
 * A key "from x" fills its register group with x, x+1, ... in order.
 * The status goes back in x0: arm64 KVM does not read run->hypercall.ret.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qom/object.h"
#include "hw/core/boards.h"
#include "system/kvm.h"
#include "cpu.h"
#include "kvm_arm.h"

#define VMAPPLE_HVC_FIRST 0xc1000000u
#define VMAPPLE_HVC_COUNT 0x100u

#define CORE_REG(x) (KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE | \
                     KVM_REG_ARM_CORE_REG(x))

/* The platform's default keys, all derived from one constant. */
#define DEFAULT_KEY_BASE 0xfeedfacefeedfacfULL

enum {
    KEY_A,
    KEY_B,
    KEY_EL0,
    KEY_G,
    KEY_GROUPS
};

static const uint64_t default_key[KEY_GROUPS] = {
    [KEY_A] = DEFAULT_KEY_BASE + 6,
    [KEY_B] = DEFAULT_KEY_BASE,
    [KEY_EL0] = DEFAULT_KEY_BASE + 4,
    [KEY_G] = DEFAULT_KEY_BASE + 10,
};

/* Register groups, in fill order. */
static const uint64_t key_regs_a[] = {
    ARM64_SYS_REG(3, 0, 2, 1, 0),   /* APIAKeyLo_EL1 */
    ARM64_SYS_REG(3, 0, 2, 1, 1),   /* APIAKeyHi_EL1 */
    ARM64_SYS_REG(3, 0, 2, 2, 0),   /* APDAKeyLo_EL1 */
    ARM64_SYS_REG(3, 0, 2, 2, 1),   /* APDAKeyHi_EL1 */
};
static const uint64_t key_regs_b[] = {
    ARM64_SYS_REG(3, 0, 2, 1, 2),   /* APIBKeyLo_EL1 */
    ARM64_SYS_REG(3, 0, 2, 1, 3),   /* APIBKeyHi_EL1 */
    ARM64_SYS_REG(3, 0, 2, 2, 2),   /* APDBKeyLo_EL1 */
    ARM64_SYS_REG(3, 0, 2, 2, 3),   /* APDBKeyHi_EL1 */
};
static const uint64_t key_regs_el0[] = {
    ARM64_SYS_REG(3, 6, 15, 2, 3),  /* Apple kernel key lo, EL12 view */
    ARM64_SYS_REG(3, 6, 15, 2, 4),  /* Apple kernel key hi, EL12 view */
};
static const uint64_t key_regs_g[] = {
    ARM64_SYS_REG(3, 0, 2, 3, 0),   /* APGAKeyLo_EL1 */
    ARM64_SYS_REG(3, 0, 2, 3, 1),   /* APGAKeyHi_EL1 */
};

static const struct {
    const uint64_t *regs;
    size_t n;
} key_group[KEY_GROUPS] = {
    [KEY_A] = { key_regs_a, ARRAY_SIZE(key_regs_a) },
    [KEY_B] = { key_regs_b, ARRAY_SIZE(key_regs_b) },
    [KEY_EL0] = { key_regs_el0, ARRAY_SIZE(key_regs_el0) },
    [KEY_G] = { key_regs_g, ARRAY_SIZE(key_regs_g) },
};

/* Apple's pointer-authentication control register, EL12 view. */
#define APCTL_EL12 ARM64_SYS_REG(3, 6, 15, 15, 0)
#define APCTL_BASE 0x19
#define APCTL_EL0_SHARES_EL1_KEYS 0x2

static bool vmapple_hvc_enabled;

static int set_key(CPUState *cs, int group, uint64_t key)
{
    size_t i;
    int ret;

    for (i = 0; i < key_group[group].n; i++) {
        uint64_t v = key + i;

        ret = kvm_set_one_reg(cs, key_group[group].regs[i], &v);
        if (ret) {
            return ret;
        }
    }
    return 0;
}

static int set_apctl(CPUState *cs, bool el0_shares_el1_keys)
{
    uint64_t v = APCTL_BASE |
                 (el0_shares_el1_keys ? APCTL_EL0_SHARES_EL1_KEYS : 0);

    return kvm_set_one_reg(cs, APCTL_EL12, &v);
}

static int get_x(CPUState *cs, int n, uint64_t *v)
{
    return kvm_get_one_reg(cs, CORE_REG(regs.regs[n]), v);
}

static int set_x(CPUState *cs, int n, uint64_t v)
{
    return kvm_set_one_reg(cs, CORE_REG(regs.regs[n]), &v);
}

static int vmapple_pac_call(CPUState *cs, unsigned fn)
{
    uint64_t x1 = 0, x2 = 0;
    int ret, g;

    if (fn >= 2 && fn <= 6) {
        ret = get_x(cs, 1, &x1);
        if (!ret && fn == 5) {
            ret = get_x(cs, 2, &x2);
        }
        if (ret) {
            return ret;
        }
    }
    switch (fn) {
    case 0:
        ret = set_apctl(cs, false);
        if (!ret) {
            ret = set_key(cs, KEY_B, default_key[KEY_B]);
        }
        if (!ret) {
            ret = set_key(cs, KEY_EL0, default_key[KEY_EL0]);
        }
        if (!ret) {
            ret = set_key(cs, KEY_A, default_key[KEY_A]);
        }
        if (!ret) {
            ret = set_key(cs, KEY_G, default_key[KEY_G]);
        }
        return ret;
    case 1:
        for (g = 0, ret = 0; g < KEY_GROUPS && !ret; g++) {
            ret = set_x(cs, 1 + g, default_key[g]);
        }
        return ret;
    case 2:
        return set_key(cs, KEY_A, x1);
    case 3:
        return set_key(cs, KEY_B, x1);
    case 4:
        return set_key(cs, KEY_EL0, x1);
    case 5:
        ret = set_key(cs, KEY_EL0, x2);
        return ret ? ret : set_apctl(cs, x1 != 0);
    case 6:
        return set_key(cs, KEY_G, x1);
    default:
        qemu_log_mask(LOG_UNIMP, "vmapple: unhandled platform hypercall "
                      "%#x\n", VMAPPLE_HVC_FIRST + fn);
        return 0;
    }
}

int kvm_arm_vmapple_init(MachineState *ms, KVMState *s)
{
    struct kvm_smccc_filter filter = {
        .base = VMAPPLE_HVC_FIRST,
        .nr_functions = VMAPPLE_HVC_COUNT,
        .action = KVM_SMCCC_FILTER_FWD_TO_USER,
    };
    struct kvm_device_attr attr = {
        .group = KVM_ARM_VM_SMCCC_CTRL,
        .attr = KVM_ARM_VM_SMCCC_FILTER,
        .addr = (uintptr_t)&filter,
    };
    int ret;

    if (!g_str_has_prefix(object_get_typename(OBJECT(ms)), "vmapple") &&
        !g_getenv("QEMU_VMAPPLE_KVM_HVC")) {
        return 0;
    }
    ret = kvm_vm_ioctl(s, KVM_SET_DEVICE_ATTR, &attr);
    if (ret < 0) {
        error_report("vmapple: cannot forward the platform hypercalls to "
                     "userspace: %s", strerror(-ret));
        return ret;
    }
    vmapple_hvc_enabled = true;
    return 0;
}

bool kvm_arm_vmapple_hypercall(CPUState *cs, struct kvm_run *run, int *ret)
{
    uint64_t nr = run->hypercall.nr;

    if (!vmapple_hvc_enabled || nr < VMAPPLE_HVC_FIRST ||
        nr >= VMAPPLE_HVC_FIRST + VMAPPLE_HVC_COUNT) {
        return false;
    }
    *ret = vmapple_pac_call(cs, nr - VMAPPLE_HVC_FIRST);
    if (!*ret) {
        *ret = set_x(cs, 0, 0);
    }
    if (*ret) {
        error_report("vmapple: platform hypercall %#" PRIx64 " failed: %s",
                     nr, strerror(-*ret));
    }
    return true;
}
