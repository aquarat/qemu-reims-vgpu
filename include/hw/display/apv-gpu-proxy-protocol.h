/*
 * apv-gpu-proxy protocol: a paravirtual GPU device model running in a
 * separate process from the VMM.
 *
 * Copyright (c) 2026 aquarat
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Overview
 * --------
 * The VMM ("frontend") owns the bus attachment: register windows, interrupt
 * lines, guest RAM and the vCPUs. The backend owns everything the device
 * means. They share:
 *
 *  - a control socket (AF_UNIX, SOCK_SEQPACKET; the frontend connects). The
 *    frontend sends requests and the backend replies with the same type and
 *    APV_PROXY_F_REPLY set. While a READ or WRITE request is outstanding the
 *    backend may send callback requests (APV_PROXY_CB_*) on the same socket;
 *    the frontend answers them from the thread that issued the request, which
 *    is the vCPU that made the register access. Callbacks are only valid then.
 *  - an event socket (backend -> frontend, no replies), passed in HELLO.
 *  - a device page (memfd): hot register words, the posted-write ring and
 *    liveness, at offsets given in HELLO.
 *  - a dirty-tracking table (memfd) for guest-write witnessing.
 *  - a scanout memfd holding display buffers and the cursor glyph.
 *  - eventfds: kick (frontend -> backend: posted work or doorbells), and one
 *    per interrupt line (backend -> frontend, or straight into a KVM irqfd).
 *  - guest RAM, as the frontend's own memory-backend fds (MEM_TABLE).
 *
 * Every multi-byte field is little-endian and naturally aligned. Shared-page
 * words are accessed with atomic operations by both processes.
 *
 * The register *semantics* are not in this file: the backend describes each
 * register it wants handled outside a round trip with an ApvProxyBehaviour
 * entry, and everything not described goes to the backend.
 */
#ifndef APV_GPU_PROXY_PROTOCOL_H
#define APV_GPU_PROXY_PROTOCOL_H

#include <stdint.h>

#define APV_PROXY_VERSION 1u

/* Largest message payload, and most fds, either side sends in one message. */
#define APV_PROXY_MAX_PAYLOAD 8192u
#define APV_PROXY_MAX_FDS 16u

typedef struct ApvProxyHdr {
    uint32_t type;
    uint32_t flags;
    uint32_t size;  /* payload bytes following this header */
    uint32_t seq;   /* echoed in the reply */
} ApvProxyHdr;

#define APV_PROXY_F_REPLY 0x1u
#define APV_PROXY_F_ERROR 0x2u  /* reply carries ApvProxyError */

/* ---- requests, frontend -> backend, control socket ---- */
#define APV_PROXY_HELLO 1u
#define APV_PROXY_MEM_TABLE 2u
#define APV_PROXY_READ 3u
#define APV_PROXY_WRITE 4u
#define APV_PROXY_RESET 5u
#define APV_PROXY_SHUTDOWN 6u  /* no reply */

/* ---- callbacks, backend -> frontend, control socket, inside READ/WRITE ---- */
#define APV_PROXY_CB_XREGS 16u
#define APV_PROXY_CB_XLATE 17u

/* ---- events, backend -> frontend, event socket ---- */
#define APV_PROXY_EV_SCANOUT 32u
#define APV_PROXY_EV_CURSOR 33u
#define APV_PROXY_EV_CURSOR_GLYPH 34u
#define APV_PROXY_EV_FATAL 35u

typedef struct ApvProxyError {
    int32_t code;     /* negative errno-style */
    uint32_t reserved;
} ApvProxyError;

/* Register windows (sysbus MMIO index). */
#define APV_PROXY_WINDOW_GFX 0u
#define APV_PROXY_WINDOW_IOSFC 1u
#define APV_PROXY_MAX_WINDOWS 4u

/* HELLO request. */
typedef struct ApvProxyHello {
    uint32_t version;
    uint32_t page_shift;      /* guest page shift (14 on arm64 macOS) */
    uint32_t nr_windows;
    uint32_t nr_irqs;
    uint64_t window_size[APV_PROXY_MAX_WINDOWS];
} ApvProxyHello;

/*
 * Register behaviour.
 *
 * read kinds:  SYNC   a READ request (the default for unlisted offsets)
 *              XCHG0  atomic exchange of the hot word with 0 (read-to-clear)
 *              LOAD   atomic load of the hot word
 * write kinds: POSTED append to the posted ring and kick (no round trip)
 *              SYNC   a WRITE request
 *              ANDNOT atomic and of the hot word with ~data
 *              BITSET if arg_lo <= data < arg_hi: atomic or of (1 << data)
 *                     into the hot word, count it, kick; otherwise count a
 *                     refusal and record the value. Ordered behind posted
 *                     writes: while the ring is not empty it is posted.
 * flags:       HARVEST  fold the hypervisor dirty log into the dirty table
 *                       before performing the write
 *              SIZE4    the hot kinds apply only to 4-byte accesses; other
 *                       sizes use the window default
 *
 * An entry with offset APV_PROXY_OFFSET_DEFAULT gives a window's defaults.
 */
#define APV_PROXY_R_SYNC 0u
#define APV_PROXY_R_XCHG0 1u
#define APV_PROXY_R_LOAD 2u
#define APV_PROXY_W_POSTED 0u
#define APV_PROXY_W_SYNC 1u
#define APV_PROXY_W_ANDNOT 2u
#define APV_PROXY_W_BITSET 3u
#define APV_PROXY_B_HARVEST 0x1u
#define APV_PROXY_B_SIZE4 0x2u
#define APV_PROXY_OFFSET_DEFAULT 0xffffffffu

typedef struct ApvProxyBehaviour {
    uint8_t window;
    uint8_t read_kind;
    uint8_t write_kind;
    uint8_t flags;
    uint32_t offset;
    uint32_t word;     /* byte offset of the hot word in the device page */
    uint16_t arg_lo;
    uint16_t arg_hi;
} ApvProxyBehaviour;

/*
 * HELLO reply, followed by nr_behaviour ApvProxyBehaviour entries.
 * fds, in order: device page, kick eventfd, event socket, scanout memfd,
 * dirty memfd, then one eventfd per interrupt line.
 */
typedef struct ApvProxyHelloReply {
    uint32_t version;
    uint32_t features;
    uint32_t nr_behaviour;
    uint32_t nr_irqs;
    uint64_t device_page_size;
    uint64_t scanout_size;
    uint64_t dirty_size;
    uint32_t posted_offset;   /* ApvProxyPostedRing in the device page */
    uint32_t reserved;
} ApvProxyHelloReply;

#define APV_PROXY_FD_DEVICE_PAGE 0u
#define APV_PROXY_FD_KICK 1u
#define APV_PROXY_FD_EVENTS 2u
#define APV_PROXY_FD_SCANOUT 3u
#define APV_PROXY_FD_DIRTY 4u
#define APV_PROXY_FD_IRQ0 5u

/* MEM_TABLE: guest RAM regions, one fd per region in order. Replaces any
 * previous table. Only writable RAM is described. */
typedef struct ApvProxyMemRegion {
    uint64_t gpa;
    uint64_t size;
    uint64_t fd_offset;
    uint64_t reserved;
} ApvProxyMemRegion;

typedef struct ApvProxyMemTable {
    uint32_t nr;
    uint32_t generation;
    /* ApvProxyMemRegion regions[nr]; */
} ApvProxyMemTable;

/* READ / WRITE. A READ reply carries ApvProxyValue. */
typedef struct ApvProxyAccess {
    uint32_t window;
    uint32_t size;
    uint64_t offset;
    uint64_t data;    /* WRITE only */
} ApvProxyAccess;

typedef struct ApvProxyValue {
    uint64_t value;
} ApvProxyValue;

/* CB_XREGS reply: the calling vCPU's general registers. */
typedef struct ApvProxyXregs {
    uint32_t ok;
    uint32_t nr;
    uint64_t x[31];
} ApvProxyXregs;

/* CB_XLATE request ApvProxyXlate{va}; reply ApvProxyXlateReply. Translates a
 * guest virtual address through the calling vCPU's current translation. */
typedef struct ApvProxyXlate {
    uint64_t va;
} ApvProxyXlate;

typedef struct ApvProxyXlateReply {
    uint32_t ok;
    uint32_t lg_page_size;  /* the mapping's block size, log2 */
    uint64_t pa;
} ApvProxyXlateReply;

/* EV_SCANOUT: buffer `buffer` of the scanout memfd (at `offset`) holds a
 * finished BGRA8 frame. */
typedef struct ApvProxyScanout {
    uint32_t buffer;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint64_t offset;
    uint64_t generation;
} ApvProxyScanout;

typedef struct ApvProxyCursor {
    int32_t x;
    int32_t y;
    uint32_t visible;
    uint32_t reserved;
} ApvProxyCursor;

/* EV_CURSOR_GLYPH: ARGB8888 (0xAARRGGBB) pixels at `offset` in the scanout
 * memfd. */
typedef struct ApvProxyCursorGlyph {
    uint32_t width;
    uint32_t height;
    uint32_t hot_x;
    uint32_t hot_y;
    uint64_t offset;
} ApvProxyCursorGlyph;

/* EV_FATAL: the backend is about to exit; NUL-terminated reason follows. */
typedef struct ApvProxyFatal {
    uint32_t code;
    uint32_t len;
} ApvProxyFatal;

/* ---- device page ---- */
#define APV_PROXY_PAGE_MAGIC 0x50565041u  /* "APVP" */

#define APV_PROXY_STATE_STARTING 0u
#define APV_PROXY_STATE_RUNNING 1u
#define APV_PROXY_STATE_FAILED 2u

typedef struct ApvProxyPageHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t state;
    uint32_t reserved0;
    uint64_t heartbeat_ns;      /* backend, CLOCK_MONOTONIC */
    uint64_t doorbells;         /* frontend: BITSET writes taken */
    uint64_t doorbells_refused; /* frontend: BITSET values out of range */
    uint32_t last_refused;      /* frontend: the last refused value */
    uint32_t reserved1;
    uint64_t posted_sync_fallbacks; /* frontend: ring full, sent as WRITE */
} ApvProxyPageHeader;

#define APV_PROXY_POSTED_ENTRIES 256u

typedef struct ApvProxyPostedEntry {
    uint32_t window;
    uint32_t size;
    uint64_t offset;
    uint64_t data;
    uint64_t seq;
} ApvProxyPostedEntry;

/* Single producer (the frontend serialises its writers), single consumer.
 * head and tail are free-running; slot = index % APV_PROXY_POSTED_ENTRIES. */
typedef struct ApvProxyPostedRing {
    uint32_t head;          /* frontend */
    uint32_t pad0[15];
    uint32_t tail;          /* backend */
    uint32_t pad1[15];
    ApvProxyPostedEntry entries[APV_PROXY_POSTED_ENTRIES];
} ApvProxyPostedRing;

/* ---- dirty table ----
 *
 * Slots are allocated and filled by the backend; the frontend arms them and
 * stamps generations when it harvests the hypervisor's dirty log.
 *
 *   FREE -> (backend writes pages, count, page_size) PENDING
 *   PENDING -> (frontend harvest: logging enabled for the pages' regions)
 *              ARMED, with arm_harvest = the harvest that armed it
 *   ARMED/PENDING -> (backend) RELEASING -> (frontend harvest) FREE
 *
 * set_gen is 0 ("cannot tell") until a harvest after arm_harvest has run,
 * then the harvest count at the most recent harvest that saw any page of the
 * set written (or 1 if none has). page_gen[i] is the same per page.
 * A page that does not resolve to logged guest RAM reads as written.
 * The frontend skips a harvest when gen_reads has not moved since the last
 * one; the backend bumps gen_reads every time it reads a generation.
 */
#define APV_PROXY_DIRTY_MAGIC 0x59545244u  /* "DRTY" */
#define APV_PROXY_DIRTY_FREE 0u
#define APV_PROXY_DIRTY_PENDING 1u
#define APV_PROXY_DIRTY_ARMED 2u
#define APV_PROXY_DIRTY_RELEASING 3u

typedef struct ApvProxyDirtyHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t nr_slots;
    uint32_t page_capacity;
    uint64_t harvests;          /* frontend */
    uint64_t gen_reads;         /* backend */
    uint32_t slots_offset;
    uint32_t pages_offset;      /* uint64_t gpa[page_capacity] */
    uint32_t page_gen_offset;   /* uint64_t page_gen[page_capacity] */
    uint32_t slots_used;        /* backend: slots at or above this are FREE */
} ApvProxyDirtyHeader;

typedef struct ApvProxyDirtySlot {
    uint32_t state;
    uint32_t page_size;
    uint32_t first_page;        /* index into gpa[] / page_gen[] */
    uint32_t count;
    uint64_t arm_harvest;       /* frontend */
    uint64_t set_gen;           /* frontend */
} ApvProxyDirtySlot;

/* Layout guards: the Rust mirror asserts the same sizes. */
_Static_assert(sizeof(ApvProxyHdr) == 16, "ApvProxyHdr");
_Static_assert(sizeof(ApvProxyHello) == 48, "ApvProxyHello");
_Static_assert(sizeof(ApvProxyBehaviour) == 16, "ApvProxyBehaviour");
_Static_assert(sizeof(ApvProxyHelloReply) == 48, "ApvProxyHelloReply");
_Static_assert(sizeof(ApvProxyMemRegion) == 32, "ApvProxyMemRegion");
_Static_assert(sizeof(ApvProxyMemTable) == 8, "ApvProxyMemTable");
_Static_assert(sizeof(ApvProxyAccess) == 24, "ApvProxyAccess");
_Static_assert(sizeof(ApvProxyXregs) == 256, "ApvProxyXregs");
_Static_assert(sizeof(ApvProxyXlateReply) == 16, "ApvProxyXlateReply");
_Static_assert(sizeof(ApvProxyScanout) == 32, "ApvProxyScanout");
_Static_assert(sizeof(ApvProxyCursorGlyph) == 24, "ApvProxyCursorGlyph");
_Static_assert(sizeof(ApvProxyPageHeader) == 56, "ApvProxyPageHeader");
_Static_assert(sizeof(ApvProxyPostedEntry) == 32, "ApvProxyPostedEntry");
_Static_assert(sizeof(ApvProxyPostedRing) == 128 + 256 * 32, "ApvProxyPostedRing");
_Static_assert(sizeof(ApvProxyDirtyHeader) == 48, "ApvProxyDirtyHeader");
_Static_assert(sizeof(ApvProxyDirtySlot) == 32, "ApvProxyDirtySlot");

#endif /* APV_GPU_PROXY_PROTOCOL_H */
