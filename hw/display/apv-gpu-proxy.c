/*
 * apv-gpu-proxy: the bus side of a paravirtual GPU whose device model runs
 * in a separate process.
 *
 * Copyright (c) 2026 aquarat
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The guest sees two register windows and two interrupt lines (sysbus, the
 * layout of the vmapple machine's paravirtual GPU slot). Everything those
 * registers mean belongs to a backend process this device connects to; see
 * include/hw/display/apv-gpu-proxy-protocol.h for the protocol.
 *
 * This file holds no device semantics. At connect time the backend describes
 * which register offsets this side serves from shared memory and how (an
 * atomic exchange, an atomic load, a posted write, a doorbell bit); every
 * other access is a synchronous request the backend answers. What this side
 * does own is what only the VMM can do:
 *
 *  - guest RAM: it hands the backend the memory-backend fds;
 *  - the guest CPU: while a vCPU waits on a request, the backend may ask for
 *    its general registers or a virtual-to-physical translation;
 *  - the hypervisor dirty log: it folds guest writes into a shared table
 *    before every write the backend marks as a handoff of work;
 *  - interrupts and the console.
 *
 * When the backend dies or stops answering, the guest's GPU state is gone
 * with it; this device then applies the configured policy (stop the VM by
 * default) with the reason on stderr.
 */

#include "qemu/osdep.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/thread.h"
#include "qemu/rcu.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/cpu.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "exec/target_page.h"
#include "migration/blocker.h"
#include "qom/object.h"
#include "system/address-spaces.h"
#include "system/hw_accel.h"
#include "system/memory.h"
#include "system/physmem.h"
#include "system/ram_addr.h"
#include "system/ramblock.h"
#include "system/runstate.h"
#include "system/system.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "hw/vmapple/vmapple.h"
#include "hw/display/apv-gpu-proxy-protocol.h"

#define TYPE_APV_GPU_PROXY "apv-gpu-proxy"
OBJECT_DECLARE_SIMPLE_TYPE(ApvGpuProxyState, APV_GPU_PROXY)

#define APV_NR_WINDOWS 2
#define APV_NR_IRQS 2
#define APV_GFX_SIZE 0x4000
#define APV_IOSFC_SIZE 0x10000
#define APV_MAX_BEHAVIOUR 64

typedef struct ApvDirtySlice {
    MemoryRegion *mr;
    uint64_t gpa;
    uint64_t len;
    ram_addr_t ram_addr;
    hwaddr offset;
    bool logged;
    bool needs_log;
} ApvDirtySlice;

typedef struct ApvDirtyHit {
    uint64_t gpa;
    uint64_t page_size;
} ApvDirtyHit;

struct ApvGpuProxyState {
    SysBusDevice parent_obj;

    MemoryRegion iomem[APV_NR_WINDOWS];
    qemu_irq irq[APV_NR_IRQS];

    /* properties */
    char *socket_path;
    char *on_loss;
    uint32_t sync_timeout_ms;
    uint32_t watchdog_ms;

    /* connection */
    int ctl_fd;
    int ev_fd;
    int kick_fd;
    int irq_fd[APV_NR_IRQS];
    uint32_t seq;
    QemuMutex ctl_lock;     /* one synchronous exchange at a time */
    QemuMutex posted_lock;  /* the posted ring's single producer */

    /* shared memory */
    uint8_t *page;
    size_t page_size;
    uint8_t *scanout;
    size_t scanout_size;
    uint8_t *dirty;
    size_t dirty_size;
    ApvProxyPostedRing *ring;

    ApvProxyBehaviour behaviour[APV_MAX_BEHAVIOUR];
    unsigned nr_behaviour;
    ApvProxyBehaviour dflt[APV_NR_WINDOWS];

    /* loss */
    bool lost;
    char lost_reason[256];
    QEMUBH *loss_bh;
    QEMUTimer *watchdog;
    uint64_t last_heartbeat;
    int64_t heartbeat_seen_ms;

    /* console */
    QemuConsole *con;
    DisplaySurface *surface;
    ApvProxyScanout shown;
    bool frame_pending;

    /* dirty service */
    GHashTable *logged;
    ApvDirtySlice *slices;
    int slices_cap;
    uint64_t last_gen_reads;
    GArray *hits;
    GArray *written;

    Notifier machine_done;
    Error *migration_blocker;
};

/* ---------- loss ---------- */

static void apv_lost(ApvGpuProxyState *s, const char *fmt, ...)
    G_GNUC_PRINTF(2, 3);

static void apv_lost(ApvGpuProxyState *s, const char *fmt, ...)
{
    va_list ap;

    if (qatomic_xchg(&s->lost, true)) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(s->lost_reason, sizeof(s->lost_reason), fmt, ap);
    va_end(ap);
    qemu_bh_schedule(s->loss_bh);
}

static void apv_loss_bh(void *opaque)
{
    ApvGpuProxyState *s = opaque;
    int i;

    if (s->ev_fd >= 0) {
        qemu_set_fd_handler(s->ev_fd, NULL, NULL, NULL);
    }
    for (i = 0; i < APV_NR_IRQS; i++) {
        if (s->irq_fd[i] >= 0) {
            qemu_set_fd_handler(s->irq_fd[i], NULL, NULL, NULL);
        }
    }
    if (s->watchdog) {
        timer_del(s->watchdog);
    }
    error_report("apv-gpu-proxy: GPU backend lost: %s; on-backend-loss=%s",
                 s->lost_reason, s->on_loss);
    if (!strcmp(s->on_loss, "shutdown")) {
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_ERROR);
    } else if (!strcmp(s->on_loss, "reset")) {
        qemu_system_reset_request(SHUTDOWN_CAUSE_HOST_ERROR);
    } else {
        vm_stop(RUN_STATE_INTERNAL_ERROR);
    }
}

/* ---------- messages ---------- */

static int apv_send(int fd, uint32_t type, uint32_t flags, uint32_t seq,
                    const void *body, size_t len, const int *fds, int nfds)
{
    ApvProxyHdr hdr = { .type = type, .flags = flags, .size = len, .seq = seq };
    struct iovec iov[2] = {
        { .iov_base = &hdr, .iov_len = sizeof(hdr) },
        { .iov_base = (void *)body, .iov_len = len },
    };
    union {
        char buf[CMSG_SPACE(sizeof(int) * APV_PROXY_MAX_FDS)];
        struct cmsghdr align;
    } u;
    struct msghdr mh = { .msg_iov = iov, .msg_iovlen = len ? 2 : 1 };
    ssize_t n;

    if (nfds > 0) {
        struct cmsghdr *cm;

        memset(&u, 0, sizeof(u));
        mh.msg_control = u.buf;
        mh.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
        cm = CMSG_FIRSTHDR(&mh);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
        memcpy(CMSG_DATA(cm), fds, sizeof(int) * nfds);
    }
    do {
        n = sendmsg(fd, &mh, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    return n < 0 ? -errno : 0;
}

/*
 * Receive one message into `body` (at most `cap` bytes) and up to `max_fds`
 * fds. Returns the payload size, 0 for EOF reported as -EPIPE, or -errno;
 * -ETIMEDOUT when nothing arrived within `timeout_ms` (-1: wait forever).
 */
static ssize_t apv_recv(int fd, ApvProxyHdr *hdr, void *body, size_t cap,
                        int *fds, int max_fds, int *nfds, int timeout_ms,
                        bool nonblock)
{
    uint8_t buf[sizeof(ApvProxyHdr) + APV_PROXY_MAX_PAYLOAD];
    struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf) };
    union {
        char buf[CMSG_SPACE(sizeof(int) * APV_PROXY_MAX_FDS)];
        struct cmsghdr align;
    } u;
    struct msghdr mh = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = u.buf, .msg_controllen = sizeof(u.buf),
    };
    struct cmsghdr *cm;
    ssize_t n;
    int got = 0;

    if (nfds) {
        *nfds = 0;
    }
    if (timeout_ms >= 0) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int r;

        do {
            r = poll(&p, 1, timeout_ms);
        } while (r < 0 && errno == EINTR);
        if (r == 0) {
            return -ETIMEDOUT;
        }
    }
    do {
        n = recvmsg(fd, &mh, MSG_CMSG_CLOEXEC | (nonblock ? MSG_DONTWAIT : 0));
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        return -errno;
    }
    for (cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
            int count = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            int *in = (int *)CMSG_DATA(cm);
            int i;

            for (i = 0; i < count; i++) {
                if (fds && got < max_fds) {
                    fds[got++] = in[i];
                } else {
                    close(in[i]);
                }
            }
        }
    }
    if (nfds) {
        *nfds = got;
    }
    if (n == 0) {
        return -EPIPE;
    }
    if ((size_t)n < sizeof(*hdr) || (mh.msg_flags & MSG_TRUNC)) {
        return -EPROTO;
    }
    memcpy(hdr, buf, sizeof(*hdr));
    if (hdr->size != n - sizeof(*hdr)) {
        return -EPROTO;
    }
    memcpy(body, buf + sizeof(*hdr), MIN(cap, (size_t)hdr->size));
    return hdr->size;
}

/* ---------- vCPU callbacks ---------- */

static void apv_cb_xregs(ApvGpuProxyState *s, const ApvProxyHdr *req)
{
    ApvProxyXregs r = { 0 };
    unsigned i;

    if (current_cpu) {
        r.ok = 1;
        r.nr = 31;
        for (i = 0; i < 31; i++) {
            if (!vmapple_read_current_xreg(i, &r.x[i])) {
                r.ok = 0;
                break;
            }
        }
    }
    apv_send(s->ctl_fd, APV_PROXY_CB_XREGS, APV_PROXY_F_REPLY, req->seq,
             &r, sizeof(r), NULL, 0);
}

static void apv_cb_xlate(ApvGpuProxyState *s, const ApvProxyHdr *req,
                         const ApvProxyXlate *x)
{
    ApvProxyXlateReply r = { 0 };
    TranslateForDebugResult tres;
    CPUState *cs = current_cpu;

    /*
     * Only the vCPU that made the access can translate through its own
     * current translation; there is deliberately no fallback to another CPU.
     */
    if (cs) {
        cpu_synchronize_state(cs);
        if (cpu_translate_for_debug(cs, x->va, &tres)) {
            r.ok = 1;
            r.pa = tres.physaddr;
            r.lg_page_size = tres.lg_page_size;
        }
    }
    apv_send(s->ctl_fd, APV_PROXY_CB_XLATE, APV_PROXY_F_REPLY, req->seq,
             &r, sizeof(r), NULL, 0);
}

/*
 * One synchronous exchange: send `type` with `body`, serve callbacks until
 * the reply arrives. Returns the reply payload size, or -errno (and reports
 * the backend lost). The caller holds ctl_lock.
 */
static ssize_t apv_exchange(ApvGpuProxyState *s, uint32_t type,
                            const void *body, size_t len,
                            const int *fds, int nfds,
                            void *out, size_t out_cap,
                            int *out_fds, int max_out_fds, int *n_out_fds)
{
    uint8_t buf[APV_PROXY_MAX_PAYLOAD];
    ApvProxyHdr hdr;
    uint32_t seq;
    ssize_t n;
    int r;

    if (qatomic_read(&s->lost) || s->ctl_fd < 0) {
        return -EPIPE;
    }
    seq = ++s->seq;
    r = apv_send(s->ctl_fd, type, 0, seq, body, len, fds, nfds);
    if (r < 0) {
        apv_lost(s, "send failed: %s", strerror(-r));
        return r;
    }
    for (;;) {
        int *fdp = out_fds;
        int maxf = max_out_fds;
        int got = 0;

        n = apv_recv(s->ctl_fd, &hdr, buf, sizeof(buf), fdp, maxf, &got,
                     s->sync_timeout_ms ? (int)s->sync_timeout_ms : -1, false);
        if (n == -ETIMEDOUT) {
            apv_lost(s, "no reply to request %u within %u ms", type,
                     s->sync_timeout_ms);
            return n;
        }
        if (n < 0) {
            apv_lost(s, "control socket: %s",
                     n == -EPIPE ? "closed by the backend" : strerror(-n));
            return n;
        }
        if (hdr.type == APV_PROXY_CB_XREGS && !(hdr.flags & APV_PROXY_F_REPLY)) {
            apv_cb_xregs(s, &hdr);
            continue;
        }
        if (hdr.type == APV_PROXY_CB_XLATE && !(hdr.flags & APV_PROXY_F_REPLY) &&
            n >= (ssize_t)sizeof(ApvProxyXlate)) {
            apv_cb_xlate(s, &hdr, (ApvProxyXlate *)buf);
            continue;
        }
        if (hdr.type != type || !(hdr.flags & APV_PROXY_F_REPLY) ||
            hdr.seq != seq) {
            apv_lost(s, "protocol error: got type %u flags %#x seq %u for "
                     "request %u seq %u", hdr.type, hdr.flags, hdr.seq,
                     type, seq);
            return -EPROTO;
        }
        if (n_out_fds) {
            *n_out_fds = got;
        }
        if (hdr.flags & APV_PROXY_F_ERROR) {
            ApvProxyError *e = (ApvProxyError *)buf;
            return n >= (ssize_t)sizeof(*e) && e->code < 0 ? e->code : -EIO;
        }
        memcpy(out, buf, MIN(out_cap, (size_t)n));
        return n;
    }
}

static uint64_t apv_sync_access(ApvGpuProxyState *s, uint32_t window,
                                hwaddr offset, uint64_t data, unsigned size,
                                bool write)
{
    ApvProxyAccess a = {
        .window = window, .size = size, .offset = offset, .data = data,
    };
    ApvProxyValue v = { 0 };
    ssize_t n;

    qemu_mutex_lock(&s->ctl_lock);
    n = apv_exchange(s, write ? APV_PROXY_WRITE : APV_PROXY_READ, &a,
                     sizeof(a), NULL, 0, &v, sizeof(v), NULL, 0, NULL);
    qemu_mutex_unlock(&s->ctl_lock);
    if (n < 0 && n != -EPIPE && !qatomic_read(&s->lost)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "apv-gpu-proxy: %s window %u offset 0x%" HWADDR_PRIx
                      " size %u refused (%zd)\n", write ? "write" : "read",
                      window, offset, size, n);
    }
    return n < 0 ? 0 : v.value;
}

/* ---------- shared page ---------- */

static ApvProxyPageHeader *apv_hdr(ApvGpuProxyState *s)
{
    return (ApvProxyPageHeader *)s->page;
}

static uint32_t *apv_word(ApvGpuProxyState *s, const ApvProxyBehaviour *b)
{
    return (uint32_t *)(s->page + b->word);
}

static void apv_kick(ApvGpuProxyState *s)
{
    uint64_t one = 1;

    if (write(s->kick_fd, &one, sizeof(one)) < 0) {
        /* Only fails if the counter would overflow; the backend is behind. */
    }
}

/* Append a write to the posted ring; false when it is full. */
static bool apv_post_locked(ApvGpuProxyState *s, uint32_t window,
                            hwaddr offset, uint64_t data, unsigned size)
{
    ApvProxyPostedRing *r = s->ring;
    uint32_t head = r->head;
    uint32_t tail = qatomic_load_acquire(&r->tail);
    ApvProxyPostedEntry *e;

    if (head - tail >= APV_PROXY_POSTED_ENTRIES) {
        return false;
    }
    e = &r->entries[head % APV_PROXY_POSTED_ENTRIES];
    e->window = window;
    e->size = size;
    e->offset = offset;
    e->data = data;
    e->seq = head;
    qatomic_store_release(&r->head, head + 1);
    return true;
}

static void apv_post(ApvGpuProxyState *s, uint32_t window, hwaddr offset,
                     uint64_t data, unsigned size)
{
    bool ok;

    qemu_mutex_lock(&s->posted_lock);
    ok = apv_post_locked(s, window, offset, data, size);
    qemu_mutex_unlock(&s->posted_lock);
    if (ok) {
        apv_kick(s);
        return;
    }
    /* Full: a synchronous write is applied after everything queued. */
    qatomic_inc(&apv_hdr(s)->posted_sync_fallbacks);
    apv_sync_access(s, window, offset, data, size, true);
}

static void apv_doorbell(ApvGpuProxyState *s, const ApvProxyBehaviour *b,
                         uint32_t window, hwaddr offset, uint64_t data,
                         unsigned size)
{
    ApvProxyPageHeader *h = apv_hdr(s);
    bool posted = false;

    if (data < b->arg_lo || data >= b->arg_hi) {
        qatomic_set(&h->last_refused, (uint32_t)data);
        qatomic_inc(&h->doorbells_refused);
        apv_kick(s);
        return;
    }
    /*
     * A doorbell must not overtake a write the guest posted before it: while
     * the ring holds anything, the doorbell joins the ring.
     */
    qemu_mutex_lock(&s->posted_lock);
    if (s->ring->head != qatomic_load_acquire(&s->ring->tail)) {
        posted = apv_post_locked(s, window, offset, data, size);
    }
    if (!posted) {
        qatomic_or(apv_word(s, b), 1u << data);
        qatomic_inc(&h->doorbells);
    }
    qemu_mutex_unlock(&s->posted_lock);
    apv_kick(s);
}

/* ---------- dirty-log service ---------- */

static ApvProxyDirtyHeader *apv_dhdr(ApvGpuProxyState *s)
{
    return (ApvProxyDirtyHeader *)s->dirty;
}

typedef struct ApvSliceScan {
    ApvGpuProxyState *s;
    int n;
} ApvSliceScan;

static bool apv_slice_range(Int128 start, Int128 len, const MemoryRegion *mr,
                            hwaddr offset_in_region, void *opaque)
{
    ApvSliceScan *scan = opaque;
    ApvGpuProxyState *s = scan->s;
    ApvDirtySlice *sl;

    if (!mr->ram || !int128_nz(len)) {
        return false;
    }
    if (scan->n == s->slices_cap) {
        s->slices_cap = s->slices_cap ? s->slices_cap * 2 : 16;
        s->slices = g_renew(ApvDirtySlice, s->slices, s->slices_cap);
    }
    sl = &s->slices[scan->n++];
    sl->mr = (MemoryRegion *)mr;
    sl->gpa = int128_get64(start);
    sl->len = int128_get64(len);
    sl->offset = offset_in_region;
    sl->ram_addr = memory_region_get_ram_addr(sl->mr) + offset_in_region;
    sl->logged = g_hash_table_contains(s->logged, sl->mr);
    sl->needs_log = false;
    return false;
}

static int apv_slice_of(ApvGpuProxyState *s, int n, uint64_t gpa)
{
    int i;

    for (i = 0; i < n; i++) {
        if (gpa >= s->slices[i].gpa && gpa - s->slices[i].gpa < s->slices[i].len) {
            return i;
        }
    }
    return -1;
}

static bool apv_page_written(const ApvDirtySlice *sl, uint64_t gpa,
                             uint64_t page_size)
{
    size_t target = qemu_target_page_size();
    ram_addr_t base = sl->ram_addr + (gpa - sl->gpa);
    uint64_t off;

    for (off = 0; off < page_size; off += target) {
        if (physical_memory_get_dirty_flag(base + off, DIRTY_MEMORY_VGA)) {
            return true;
        }
    }
    return false;
}

static int apv_cmp_hit(gconstpointer a, gconstpointer b)
{
    const ApvDirtyHit *x = a, *y = b;

    if (x->gpa != y->gpa) {
        return x->gpa < y->gpa ? -1 : 1;
    }
    return x->page_size > y->page_size ? -1 : (x->page_size < y->page_size);
}

/*
 * Fold the hypervisor dirty log into the shared table. BQL held (the MMIO
 * handler's): this syncs the log and may turn logging on, both MemoryRegion
 * operations. The rules are the device contract's (ABI v12/v13): a set's
 * generation reads 0 until one harvest has passed since it was registered,
 * or one more if that harvest had to enable logging for its pages; a page
 * this host cannot vouch for reads as written; only pages that came back
 * written are cleared, after every set has seen them.
 */
static void apv_dirty_harvest(ApvGpuProxyState *s)
{
    ApvProxyDirtyHeader *h = apv_dhdr(s);
    ApvProxyDirtySlot *slots;
    uint64_t *page_gpa, *page_gen;
    uint64_t reads, harvests;
    uint32_t used, i;
    ApvSliceScan scan = { s, 0 };
    guint w;
    int n;

    if (!h || qatomic_load_acquire(&h->magic) != APV_PROXY_DIRTY_MAGIC) {
        return;
    }
    used = MIN(qatomic_load_acquire(&h->slots_used), h->nr_slots);
    reads = qatomic_load_acquire(&h->gen_reads);
    if (used == 0 || reads == s->last_gen_reads) {
        return;
    }
    s->last_gen_reads = reads;
    slots = (ApvProxyDirtySlot *)(s->dirty + h->slots_offset);
    page_gpa = (uint64_t *)(s->dirty + h->pages_offset);
    page_gen = (uint64_t *)(s->dirty + h->page_gen_offset);

    WITH_RCU_READ_LOCK_GUARD() {
        flatview_for_each_range(address_space_to_flatview(&address_space_memory),
                                apv_slice_range, &scan);
    }
    n = scan.n;

    memory_global_dirty_log_sync(false);

    harvests = qatomic_read(&h->harvests) + 1;
    qatomic_store_release(&h->harvests, harvests);
    g_array_set_size(s->written, 0);

    for (i = 0; i < used; i++) {
        ApvProxyDirtySlot *slot = &slots[i];
        uint32_t state = qatomic_load_acquire(&slot->state);
        bool any = false, unlogged = false;
        uint64_t gen;
        uint32_t p, first, count;
        uint64_t psize;

        if (state == APV_PROXY_DIRTY_FREE) {
            continue;
        }
        if (state == APV_PROXY_DIRTY_RELEASING) {
            qatomic_cmpxchg(&slot->state, APV_PROXY_DIRTY_RELEASING,
                            APV_PROXY_DIRTY_FREE);
            continue;
        }
        if (state == APV_PROXY_DIRTY_PENDING &&
            qatomic_cmpxchg(&slot->state, APV_PROXY_DIRTY_PENDING,
                            APV_PROXY_DIRTY_ARMED) != APV_PROXY_DIRTY_PENDING) {
            continue;
        }
        first = slot->first_page;
        count = slot->count;
        psize = slot->page_size;
        if ((uint64_t)first + count > h->page_capacity || psize == 0) {
            continue;
        }
        g_array_set_size(s->hits, 0);
        for (p = first; p < first + count; p++) {
            uint64_t gpa = qatomic_read(&page_gpa[p]);
            int si = apv_slice_of(s, n, gpa);

            if (si < 0) {
                any = true;
                g_array_append_val(s->hits, p);
                continue;
            }
            if (!s->slices[si].logged) {
                s->slices[si].needs_log = true;
                unlogged = true;
                any = true;
                g_array_append_val(s->hits, p);
                continue;
            }
            if (apv_page_written(&s->slices[si], gpa, psize)) {
                ApvDirtyHit rec = { gpa, psize };

                any = true;
                g_array_append_val(s->hits, p);
                g_array_append_val(s->written, rec);
            }
        }
        gen = qatomic_read(&slot->set_gen);
        if (gen == 0 && unlogged) {
            qatomic_set(&slot->arm_harvest, harvests + 1);
        }
        if (harvests < qatomic_read(&slot->arm_harvest)) {
            gen = 0;
        } else if (gen == 0) {
            gen = 1;
        } else if (any) {
            gen++;
        }
        if (gen != 0) {
            for (w = 0; w < s->hits->len; w++) {
                qatomic_store_release(&page_gen[g_array_index(s->hits, uint32_t, w)],
                                      gen);
            }
        }
        qatomic_store_release(&slot->set_gen, gen);
    }

    /* Clear only what came back written, merging exactly adjacent pages. */
    g_array_sort(s->written, apv_cmp_hit);
    w = 0;
    while (w < s->written->len) {
        ApvDirtyHit first = g_array_index(s->written, ApvDirtyHit, w);
        int si = apv_slice_of(s, n, first.gpa);
        uint64_t end = first.gpa + first.page_size;

        while (w + 1 < s->written->len) {
            ApvDirtyHit next = g_array_index(s->written, ApvDirtyHit, w + 1);

            if (next.gpa + next.page_size <= end) {
                w++;
                continue;
            }
            if (next.gpa != end || apv_slice_of(s, n, next.gpa) != si) {
                break;
            }
            end = next.gpa + next.page_size;
            w++;
        }
        if (si >= 0) {
            memory_region_reset_dirty(s->slices[si].mr,
                                      s->slices[si].offset +
                                      (first.gpa - s->slices[si].gpa),
                                      end - first.gpa, DIRTY_MEMORY_VGA);
        }
        w++;
    }

    /* Last: a MemoryRegion transaction rebuilds the flat view. */
    for (i = 0; i < (uint32_t)n; i++) {
        if (s->slices[i].needs_log && g_hash_table_add(s->logged, s->slices[i].mr)) {
            memory_region_ref(s->slices[i].mr);
            memory_region_set_log(s->slices[i].mr, true, DIRTY_MEMORY_VGA);
        }
    }
}

/* ---------- MMIO ---------- */

static const ApvProxyBehaviour *apv_find(ApvGpuProxyState *s, uint32_t window,
                                         hwaddr offset, unsigned size)
{
    unsigned i;

    for (i = 0; i < s->nr_behaviour; i++) {
        const ApvProxyBehaviour *b = &s->behaviour[i];

        if (b->window == window && b->offset == offset) {
            if ((b->flags & APV_PROXY_B_SIZE4) && size != 4) {
                break;
            }
            return b;
        }
    }
    return &s->dflt[window];
}

static uint64_t apv_read(ApvGpuProxyState *s, uint32_t window, hwaddr offset,
                         unsigned size)
{
    const ApvProxyBehaviour *b = apv_find(s, window, offset, size);

    switch (b->read_kind) {
    case APV_PROXY_R_XCHG0:
        return qatomic_xchg(apv_word(s, b), 0);
    case APV_PROXY_R_LOAD:
        return qatomic_load_acquire(apv_word(s, b));
    default:
        return apv_sync_access(s, window, offset, 0, size, false);
    }
}

static void apv_write(ApvGpuProxyState *s, uint32_t window, hwaddr offset,
                      uint64_t data, unsigned size)
{
    const ApvProxyBehaviour *b = apv_find(s, window, offset, size);

    if (b->flags & APV_PROXY_B_HARVEST) {
        apv_dirty_harvest(s);
    }
    switch (b->write_kind) {
    case APV_PROXY_W_ANDNOT:
        qatomic_and(apv_word(s, b), ~(uint32_t)data);
        break;
    case APV_PROXY_W_BITSET:
        apv_doorbell(s, b, window, offset, data, size);
        break;
    case APV_PROXY_W_SYNC:
        apv_sync_access(s, window, offset, data, size, true);
        break;
    default:
        apv_post(s, window, offset, data, size);
        break;
    }
}

static uint64_t apv_gfx_read(void *opaque, hwaddr offset, unsigned size)
{
    ApvGpuProxyState *s = opaque;

    return s->page ? apv_read(s, APV_PROXY_WINDOW_GFX, offset, size) : 0;
}

static void apv_gfx_write(void *opaque, hwaddr offset, uint64_t data,
                          unsigned size)
{
    ApvGpuProxyState *s = opaque;

    if (s->page && !qatomic_read(&s->lost)) {
        apv_write(s, APV_PROXY_WINDOW_GFX, offset, data, size);
    }
}

static uint64_t apv_iosfc_read(void *opaque, hwaddr offset, unsigned size)
{
    ApvGpuProxyState *s = opaque;

    return s->page ? apv_read(s, APV_PROXY_WINDOW_IOSFC, offset, size) : 0;
}

static void apv_iosfc_write(void *opaque, hwaddr offset, uint64_t data,
                            unsigned size)
{
    ApvGpuProxyState *s = opaque;

    if (s->page && !qatomic_read(&s->lost)) {
        apv_write(s, APV_PROXY_WINDOW_IOSFC, offset, data, size);
    }
}

static const MemoryRegionOps apv_gfx_ops = {
    .read = apv_gfx_read,
    .write = apv_gfx_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 8 },
    .impl = { .min_access_size = 4, .max_access_size = 8 },
};

static const MemoryRegionOps apv_iosfc_ops = {
    .read = apv_iosfc_read,
    .write = apv_iosfc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 8 },
    .impl = { .min_access_size = 4, .max_access_size = 8 },
};

/* ---------- interrupts and events ---------- */

static void apv_irq_ready(void *opaque, int line)
{
    ApvGpuProxyState *s = opaque;
    uint64_t v;

    if (read(s->irq_fd[line], &v, sizeof(v)) == sizeof(v)) {
        qemu_irq_pulse(s->irq[line]);
    }
}

static void apv_irq0_ready(void *opaque)
{
    apv_irq_ready(opaque, 0);
}

static void apv_irq1_ready(void *opaque)
{
    apv_irq_ready(opaque, 1);
}

static void apv_show(ApvGpuProxyState *s, const ApvProxyScanout *sc)
{
    if (!s->con || sc->width == 0 || sc->height == 0 ||
        sc->offset + (uint64_t)sc->stride * sc->height > s->scanout_size) {
        return;
    }
    if (!s->surface || sc->buffer != s->shown.buffer ||
        sc->width != s->shown.width || sc->height != s->shown.height ||
        sc->stride != s->shown.stride) {
        s->surface = qemu_create_displaysurface_from(sc->width, sc->height,
                                                     PIXMAN_x8r8g8b8,
                                                     sc->stride,
                                                     s->scanout + sc->offset);
        qemu_console_set_surface(s->con, s->surface);
    }
    s->shown = *sc;
    s->frame_pending = true;
}

static void apv_glyph(ApvGpuProxyState *s, const ApvProxyCursorGlyph *g)
{
    uint64_t bytes = (uint64_t)g->width * g->height * 4;
    QEMUCursor *c;

    if (!s->con || g->width == 0 || g->height == 0 || g->width > 512 ||
        g->height > 512 || g->offset + bytes > s->scanout_size) {
        return;
    }
    c = cursor_alloc(g->width, g->height);
    if (!c) {
        return;
    }
    c->hot_x = g->hot_x;
    c->hot_y = g->hot_y;
    memcpy(c->data, s->scanout + g->offset, bytes);
    qemu_console_set_cursor(s->con, c);
    cursor_unref(c);
}

static void apv_event_ready(void *opaque)
{
    ApvGpuProxyState *s = opaque;
    uint8_t buf[APV_PROXY_MAX_PAYLOAD];
    ApvProxyHdr hdr;
    ssize_t n;

    for (;;) {
        n = apv_recv(s->ev_fd, &hdr, buf, sizeof(buf), NULL, 0, NULL, -1, true);
        if (n == -EAGAIN || n == -EWOULDBLOCK) {
            return;
        }
        if (n < 0) {
            apv_lost(s, "event socket: %s",
                     n == -EPIPE ? "closed by the backend" : strerror(-n));
            qemu_set_fd_handler(s->ev_fd, NULL, NULL, NULL);
            return;
        }
        switch (hdr.type) {
        case APV_PROXY_EV_SCANOUT:
            if (n >= (ssize_t)sizeof(ApvProxyScanout)) {
                apv_show(s, (ApvProxyScanout *)buf);
            }
            break;
        case APV_PROXY_EV_CURSOR:
            if (n >= (ssize_t)sizeof(ApvProxyCursor) && s->con) {
                ApvProxyCursor *c = (ApvProxyCursor *)buf;

                qemu_console_set_mouse(s->con, c->x, c->y, c->visible);
            }
            break;
        case APV_PROXY_EV_CURSOR_GLYPH:
            if (n >= (ssize_t)sizeof(ApvProxyCursorGlyph)) {
                apv_glyph(s, (ApvProxyCursorGlyph *)buf);
            }
            break;
        case APV_PROXY_EV_FATAL:
            if (n >= (ssize_t)sizeof(ApvProxyFatal)) {
                buf[MIN((size_t)n, sizeof(buf) - 1)] = 0;
                apv_lost(s, "backend reported: %s",
                         (char *)buf + sizeof(ApvProxyFatal));
            }
            break;
        default:
            break;
        }
    }
}

static void apv_watchdog(void *opaque)
{
    ApvGpuProxyState *s = opaque;
    uint64_t hb = qatomic_load_acquire(&apv_hdr(s)->heartbeat_ns);
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

    if (hb != s->last_heartbeat) {
        s->last_heartbeat = hb;
        s->heartbeat_seen_ms = now;
    } else if (now - s->heartbeat_seen_ms > s->watchdog_ms) {
        apv_lost(s, "no heartbeat for %" PRId64 " ms",
                 now - s->heartbeat_seen_ms);
        return;
    }
    timer_mod(s->watchdog, now + 1000);
}

/* ---------- console ---------- */

static bool apv_gfx_update(void *opaque)
{
    ApvGpuProxyState *s = opaque;

    if (s->con && s->frame_pending) {
        s->frame_pending = false;
        qemu_console_update_full(s->con);
    }
    return true;
}

static const GraphicHwOps apv_fb_ops = {
    .gfx_update = apv_gfx_update,
};

/* ---------- connection ---------- */

static void *apv_map(int fd, size_t size, Error **errp)
{
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (p == MAP_FAILED) {
        error_setg_errno(errp, errno, "apv-gpu-proxy: mmap");
        return NULL;
    }
    return p;
}

static bool apv_connect(ApvGpuProxyState *s, Error **errp)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    struct {
        ApvProxyHelloReply r;
        ApvProxyBehaviour b[APV_MAX_BEHAVIOUR];
    } rep;
    ApvProxyHello hello = {
        .version = APV_PROXY_VERSION,
        .page_shift = 14,
        .nr_windows = APV_NR_WINDOWS,
        .nr_irqs = APV_NR_IRQS,
        .window_size = { APV_GFX_SIZE, APV_IOSFC_SIZE },
    };
    int fds[APV_PROXY_MAX_FDS];
    int nfds = 0, i;
    ssize_t n;
    bool ok = false;

    if (!s->socket_path || strlen(s->socket_path) >= sizeof(addr.sun_path)) {
        error_setg(errp, "apv-gpu-proxy: socket property missing or too long");
        return false;
    }
    strcpy(addr.sun_path, s->socket_path);
    s->ctl_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (s->ctl_fd < 0 ||
        connect(s->ctl_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        error_setg_errno(errp, errno, "apv-gpu-proxy: connect %s",
                         s->socket_path);
        return false;
    }
    qemu_mutex_lock(&s->ctl_lock);
    n = apv_exchange(s, APV_PROXY_HELLO, &hello, sizeof(hello), NULL, 0,
                     &rep, sizeof(rep), fds, APV_PROXY_MAX_FDS, &nfds);
    qemu_mutex_unlock(&s->ctl_lock);
    if (n < (ssize_t)sizeof(rep.r)) {
        error_setg(errp, "apv-gpu-proxy: HELLO failed (%zd): %s", n,
                   s->lost_reason);
        goto out;
    }
    if (rep.r.version != APV_PROXY_VERSION ||
        rep.r.nr_behaviour > APV_MAX_BEHAVIOUR ||
        n < (ssize_t)(sizeof(rep.r) + rep.r.nr_behaviour * sizeof(ApvProxyBehaviour)) ||
        rep.r.nr_irqs < APV_NR_IRQS || nfds < (int)(APV_PROXY_FD_IRQ0 + APV_NR_IRQS) ||
        rep.r.device_page_size < rep.r.posted_offset + sizeof(ApvProxyPostedRing)) {
        error_setg(errp, "apv-gpu-proxy: malformed HELLO reply");
        goto out;
    }
    s->page_size = rep.r.device_page_size;
    s->scanout_size = rep.r.scanout_size;
    s->dirty_size = rep.r.dirty_size;
    s->page = apv_map(fds[APV_PROXY_FD_DEVICE_PAGE], s->page_size, errp);
    s->scanout = s->page ? apv_map(fds[APV_PROXY_FD_SCANOUT], s->scanout_size, errp) : NULL;
    s->dirty = s->scanout ? apv_map(fds[APV_PROXY_FD_DIRTY], s->dirty_size, errp) : NULL;
    if (!s->dirty) {
        goto out;
    }
    s->ring = (ApvProxyPostedRing *)(s->page + rep.r.posted_offset);
    s->kick_fd = dup(fds[APV_PROXY_FD_KICK]);
    s->ev_fd = dup(fds[APV_PROXY_FD_EVENTS]);
    for (i = 0; i < APV_NR_IRQS; i++) {
        s->irq_fd[i] = dup(fds[APV_PROXY_FD_IRQ0 + i]);
    }
    s->dflt[APV_PROXY_WINDOW_GFX] = (ApvProxyBehaviour) {
        .window = APV_PROXY_WINDOW_GFX, .write_kind = APV_PROXY_W_SYNC,
    };
    s->dflt[APV_PROXY_WINDOW_IOSFC] = (ApvProxyBehaviour) {
        .window = APV_PROXY_WINDOW_IOSFC, .write_kind = APV_PROXY_W_SYNC,
    };
    s->nr_behaviour = 0;
    for (i = 0; i < (int)rep.r.nr_behaviour; i++) {
        ApvProxyBehaviour *b = &rep.b[i];

        if (b->window >= APV_NR_WINDOWS ||
            ((b->read_kind != APV_PROXY_R_SYNC || b->write_kind == APV_PROXY_W_ANDNOT ||
              b->write_kind == APV_PROXY_W_BITSET) &&
             (b->word % 4 || b->word + 4 > s->page_size)) ||
            (b->write_kind == APV_PROXY_W_BITSET && b->arg_hi > 32)) {
            error_setg(errp, "apv-gpu-proxy: behaviour entry %d invalid", i);
            goto out;
        }
        if (b->offset == APV_PROXY_OFFSET_DEFAULT) {
            s->dflt[b->window] = *b;
        } else {
            s->behaviour[s->nr_behaviour++] = *b;
        }
    }
    qemu_set_fd_handler(s->ev_fd, apv_event_ready, NULL, s);
    qemu_set_fd_handler(s->irq_fd[0], apv_irq0_ready, NULL, s);
    qemu_set_fd_handler(s->irq_fd[1], apv_irq1_ready, NULL, s);
    ok = true;
out:
    for (i = 0; i < nfds; i++) {
        close(fds[i]);
    }
    return ok;
}

/* Guest RAM, as the memory-backend fds this process holds. */
typedef struct ApvMemScan {
    ApvProxyMemRegion r[APV_PROXY_MAX_FDS];
    int fd[APV_PROXY_MAX_FDS];
    int n;
    int skipped;
} ApvMemScan;

static bool apv_mem_range(Int128 start, Int128 len, const MemoryRegion *mr,
                          hwaddr offset_in_region, void *opaque)
{
    ApvMemScan *scan = opaque;
    ram_addr_t block_off;
    RAMBlock *rb;
    void *hva;
    int fd;

    if (!mr || !memory_region_is_ram(mr) || mr->readonly ||
        memory_region_is_romd(mr) || memory_region_is_ram_device(mr)) {
        return false;
    }
    hva = (uint8_t *)memory_region_get_ram_ptr((MemoryRegion *)mr) + offset_in_region;
    rb = qemu_ram_block_from_host(hva, false, &block_off);
    fd = rb ? qemu_ram_get_fd(rb) : -1;
    if (fd < 0 || scan->n == APV_PROXY_MAX_FDS) {
        warn_report("apv-gpu-proxy: guest RAM 0x%" PRIx64 "+0x%" PRIx64
                    " (%s) is not fd-backed and is invisible to the GPU "
                    "backend", int128_get64(start), int128_get64(len),
                    memory_region_name(mr));
        scan->skipped++;
        return false;
    }
    scan->r[scan->n] = (ApvProxyMemRegion) {
        .gpa = int128_get64(start),
        .size = int128_get64(len),
        .fd_offset = qemu_ram_get_fd_offset(rb) + block_off,
    };
    scan->fd[scan->n] = fd;
    scan->n++;
    return false;
}

static void apv_send_mem_table(ApvGpuProxyState *s)
{
    struct {
        ApvProxyMemTable t;
        ApvProxyMemRegion r[APV_PROXY_MAX_FDS];
    } msg;
    ApvMemScan scan = { 0 };
    ApvProxyValue v;
    ssize_t n;

    WITH_RCU_READ_LOCK_GUARD() {
        flatview_for_each_range(address_space_to_flatview(&address_space_memory),
                                apv_mem_range, &scan);
    }
    if (scan.n == 0) {
        warn_report("apv-gpu-proxy: no fd-backed guest RAM; the GPU backend "
                    "cannot reach guest memory (use memory-backend-memfd,"
                    "share=on)");
    }
    msg.t.nr = scan.n;
    msg.t.generation = 1;
    memcpy(msg.r, scan.r, sizeof(ApvProxyMemRegion) * scan.n);
    qemu_mutex_lock(&s->ctl_lock);
    n = apv_exchange(s, APV_PROXY_MEM_TABLE, &msg,
                     sizeof(msg.t) + sizeof(ApvProxyMemRegion) * scan.n,
                     scan.fd, scan.n, &v, sizeof(v), NULL, 0, NULL);
    qemu_mutex_unlock(&s->ctl_lock);
    if (n < 0) {
        apv_lost(s, "MEM_TABLE refused (%zd)", n);
    }
}

static void apv_machine_done(Notifier *notifier, void *data)
{
    ApvGpuProxyState *s = container_of(notifier, ApvGpuProxyState, machine_done);

    apv_send_mem_table(s);
}

/* ---------- lifecycle ---------- */
/*
 * Drop everything the lost backend gave us, then connect to its successor
 * (the launcher restarts it) and hand it guest RAM again. Used on guest reset
 * with on-backend-loss=reset: the guest's GPU state died with the old process,
 * so it starts again from a reset device. Main loop, BQL held.
 */
static bool apv_reconnect(ApvGpuProxyState *s, Error **errp)
{
    int i, tries;

    if (s->con) {
        s->surface = qemu_create_displaysurface(1920, 1080);
        qemu_console_set_surface(s->con, s->surface);
        memset(&s->shown, 0, sizeof(s->shown));
    }
    if (s->page) {
        munmap(s->page, s->page_size);
        s->page = NULL;
    }
    if (s->scanout) {
        munmap(s->scanout, s->scanout_size);
        s->scanout = NULL;
    }
    if (s->dirty) {
        munmap(s->dirty, s->dirty_size);
        s->dirty = NULL;
    }
    s->ring = NULL;
    if (s->ev_fd >= 0) {
        qemu_set_fd_handler(s->ev_fd, NULL, NULL, NULL);
        close(s->ev_fd);
        s->ev_fd = -1;
    }
    for (i = 0; i < APV_NR_IRQS; i++) {
        if (s->irq_fd[i] >= 0) {
            qemu_set_fd_handler(s->irq_fd[i], NULL, NULL, NULL);
            close(s->irq_fd[i]);
            s->irq_fd[i] = -1;
        }
    }
    if (s->kick_fd >= 0) {
        close(s->kick_fd);
        s->kick_fd = -1;
    }
    if (s->ctl_fd >= 0) {
        close(s->ctl_fd);
        s->ctl_fd = -1;
    }
    s->last_gen_reads = 0;
    s->nr_behaviour = 0;
    qatomic_set(&s->lost, false);
    s->lost_reason[0] = 0;

    /* The launcher restarts the backend; give it time to listen again. */
    for (tries = 0; tries < 300; tries++) {
        Error *local = NULL;

        if (apv_connect(s, &local)) {
            break;
        }
        if (s->ctl_fd >= 0) {
            close(s->ctl_fd);
            s->ctl_fd = -1;
        }
        if (tries == 299) {
            error_propagate(errp, local);
            return false;
        }
        error_free(local);
        g_usleep(100 * 1000);
    }
    apv_send_mem_table(s);
    if (qatomic_read(&s->lost)) {
        error_setg(errp, "apv-gpu-proxy: new backend refused guest RAM: %s",
                   s->lost_reason);
        return false;
    }
    if (s->watchdog) {
        s->heartbeat_seen_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        s->last_heartbeat = 0;
        timer_mod(s->watchdog, s->heartbeat_seen_ms + 1000);
    }
    info_report("apv-gpu-proxy: reconnected to a new GPU backend");
    return true;
}


static void apv_init(Object *obj)
{
    ApvGpuProxyState *s = APV_GPU_PROXY(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    int i;

    memory_region_init_io(&s->iomem[0], obj, &apv_gfx_ops, s,
                          TYPE_APV_GPU_PROXY ".gfx", APV_GFX_SIZE);
    memory_region_init_io(&s->iomem[1], obj, &apv_iosfc_ops, s,
                          TYPE_APV_GPU_PROXY ".iosfc", APV_IOSFC_SIZE);
    for (i = 0; i < APV_NR_WINDOWS; i++) {
        sysbus_init_mmio(sbd, &s->iomem[i]);
    }
    for (i = 0; i < APV_NR_IRQS; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
        s->irq_fd[i] = -1;
    }
    s->ctl_fd = s->ev_fd = s->kick_fd = -1;
}

static void apv_realize(DeviceState *dev, Error **errp)
{
    ApvGpuProxyState *s = APV_GPU_PROXY(dev);

    if (!s->on_loss) {
        s->on_loss = g_strdup("stop");
    }
    qemu_mutex_init(&s->ctl_lock);
    qemu_mutex_init(&s->posted_lock);
    s->loss_bh = qemu_bh_new(apv_loss_bh, s);
    s->logged = g_hash_table_new(NULL, NULL);
    s->hits = g_array_new(false, false, sizeof(uint32_t));
    s->written = g_array_new(false, false, sizeof(ApvDirtyHit));
    if (strcmp(s->on_loss, "stop") && strcmp(s->on_loss, "reset") &&
        strcmp(s->on_loss, "shutdown")) {
        error_setg(errp, "apv-gpu-proxy: on-backend-loss must be stop, reset "
                   "or shutdown");
        return;
    }
    if (!apv_connect(s, errp)) {
        return;
    }
    error_setg(&s->migration_blocker,
               "apv-gpu-proxy: the GPU's state lives in its backend process");
    if (migrate_add_blocker(&s->migration_blocker, errp) < 0) {
        return;
    }
    s->machine_done.notify = apv_machine_done;
    qemu_add_machine_init_done_notifier(&s->machine_done);

    s->con = qemu_graphic_console_create(dev, 0, &apv_fb_ops, s);
    s->surface = qemu_create_displaysurface(1920, 1080);
    qemu_console_set_surface(s->con, s->surface);
    qemu_console_set_cursor(s->con, cursor_builtin_hidden());
    qemu_console_set_mouse(s->con, 0, 0, false);

    if (s->watchdog_ms) {
        s->watchdog = timer_new_ms(QEMU_CLOCK_REALTIME, apv_watchdog, s);
        s->heartbeat_seen_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        timer_mod(s->watchdog, s->heartbeat_seen_ms + 1000);
    }
}

static void apv_unrealize(DeviceState *dev)
{
    ApvGpuProxyState *s = APV_GPU_PROXY(dev);
    GHashTableIter it;
    gpointer key;

    if (s->ctl_fd >= 0 && !qatomic_read(&s->lost)) {
        apv_send(s->ctl_fd, APV_PROXY_SHUTDOWN, 0, ++s->seq, NULL, 0, NULL, 0);
    }
    if (s->logged) {
        g_hash_table_iter_init(&it, s->logged);
        while (g_hash_table_iter_next(&it, &key, NULL)) {
            memory_region_set_log(key, false, DIRTY_MEMORY_VGA);
            memory_region_unref(key);
        }
    }
}

static void apv_reset(DeviceState *dev)
{
    ApvGpuProxyState *s = APV_GPU_PROXY(dev);
    ApvProxyValue v;
    int i;

    for (i = 0; i < APV_NR_IRQS; i++) {
        qemu_set_irq(s->irq[i], 0);
    }
    if (qatomic_read(&s->lost) && s->on_loss && !strcmp(s->on_loss, "reset")) {
        Error *err = NULL;

        if (!apv_reconnect(s, &err)) {
            error_report_err(err);
            vm_stop(RUN_STATE_INTERNAL_ERROR);
            return;
        }
    }
    if (s->ctl_fd < 0 || qatomic_read(&s->lost)) {
        return;
    }
    qemu_mutex_lock(&s->posted_lock);
    qemu_mutex_lock(&s->ctl_lock);
    apv_exchange(s, APV_PROXY_RESET, NULL, 0, NULL, 0, &v, sizeof(v),
                 NULL, 0, NULL);
    qemu_mutex_unlock(&s->ctl_lock);
    qemu_mutex_unlock(&s->posted_lock);
}

static const Property apv_properties[] = {
    DEFINE_PROP_STRING("socket", ApvGpuProxyState, socket_path),
    DEFINE_PROP_STRING("on-backend-loss", ApvGpuProxyState, on_loss),
    DEFINE_PROP_UINT32("sync-timeout-ms", ApvGpuProxyState, sync_timeout_ms,
                       30000),
    DEFINE_PROP_UINT32("watchdog-ms", ApvGpuProxyState, watchdog_ms, 60000),
};

static void apv_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Paravirtual GPU, device model in a separate process";
    dc->realize = apv_realize;
    dc->unrealize = apv_unrealize;
    device_class_set_legacy_reset(dc, apv_reset);
    device_class_set_props(dc, apv_properties);
    dc->user_creatable = false;
    dc->hotpluggable = false;
}

static const TypeInfo apv_types[] = {
    {
        .name = TYPE_APV_GPU_PROXY,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(ApvGpuProxyState),
        .instance_init = apv_init,
        .class_init = apv_class_init,
    },
};

DEFINE_TYPES(apv_types)
