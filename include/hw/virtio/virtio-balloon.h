/*
 * Virtio Support
 *
 * Copyright IBM, Corp. 2007-2008
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *  Rusty Russell     <rusty@rustcorp.com.au>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#ifndef QEMU_VIRTIO_BALLOON_H
#define QEMU_VIRTIO_BALLOON_H

#include "standard-headers/linux/virtio_balloon.h"
#include "hw/core/resettable.h"
#include "hw/virtio/virtio.h"
#include "system/iothread.h"
#include "qom/object.h"

#define TYPE_VIRTIO_BALLOON "virtio-balloon-device"
OBJECT_DECLARE_SIMPLE_TYPE(VirtIOBalloon, VIRTIO_BALLOON)

#define VIRTIO_BALLOON_FREE_PAGE_HINT_CMD_ID_MIN 0x80000000

typedef struct virtio_balloon_stat VirtIOBalloonStat;

typedef struct virtio_balloon_stat_modern {
       uint16_t tag;
       uint8_t reserved[6];
       uint64_t val;
} VirtIOBalloonStatModern;

enum virtio_balloon_free_page_hint_status {
    FREE_PAGE_HINT_S_STOP = 0,
    FREE_PAGE_HINT_S_REQUESTED = 1,
    FREE_PAGE_HINT_S_START = 2,
    FREE_PAGE_HINT_S_DONE = 3,
    FREE_PAGE_HINT_S_UNREALIZE = 4,
};

typedef struct VirtIOBalloonHeldElem {
    VirtQueueElement elem;      /* must be first (virtqueue_pop) */
    uint32_t pfns;
    QTAILQ_ENTRY(VirtIOBalloonHeldElem) next;
} VirtIOBalloonHeldElem;

struct VirtIOBalloon {
    VirtIODevice parent_obj;
    VirtQueue *ivq, *dvq, *svq, *free_page_vq, *reporting_vq;
    uint32_t free_page_hint_status;
    uint32_t num_pages;
    uint32_t actual;
    uint32_t free_page_hint_cmd_id;
    uint64_t stats[VIRTIO_BALLOON_S_NR];
    VirtQueueElement *stats_vq_elem;
    size_t stats_vq_offset;
    QEMUTimer *stats_timer;
    IOThread *iothread;
    QEMUBH *free_page_bh;
    /*
     * Lock to synchronize threads to access the free page reporting related
     * fields (e.g. free_page_hint_status).
     */
    QemuMutex free_page_lock;
    QemuCond  free_page_cond;
    /*
     * Set to block iothread to continue reading free page hints as the VM is
     * stopped.
     */
    bool block_iothread;
    NotifierWithReturn free_page_hint_notify;
    int64_t stats_last_update;
    int64_t stats_poll_interval;
    uint32_t host_features;
    /*
     * macOS guests (AppleVirtIOBalloon, see virtio-balloon.c): the device
     * counts the 4 KiB PFNs it receives into `actual`, presents num_pages in
     * the driver's units one bounded step at a time, and (macos_hold) keeps
     * inflate buffers instead of completing them.
     */
    bool macos_units;
    bool macos_hold;
    uint32_t macos_step;        /* max 4 KiB pages per inflate step */
    uint32_t macos_held_pfns;   /* PFNs in held inflate buffers */
    QTAILQ_HEAD(, VirtIOBalloonHeldElem) macos_held;

    uint32_t poison_val;

    /* State of the resettable container */
    ResettableState reset_state;
};

#endif
