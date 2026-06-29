/*
 * Main include file
 * This file contains various routines and globals
 *
 * Copyright (c) 2008-2017 Red Hat, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met :
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and / or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of their contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef ___VIOSTOR_H__
#define ___VIOSTOR_H__

#include <ntddk.h>
#include <storport.h>
#include <ntddscsi.h>

#include "osdep.h"
#include "virtio_pci.h"
#include "virtio.h"
#include "virtio_ring.h"
#include "virtio_stor_utils.h"
#include "viostor_bounce.h"

typedef struct VirtIOBufferDescriptor VIO_SG, *PVIO_SG;

/* Feature bits */
#define VIRTIO_BLK_F_BARRIER               0  /* Does host support barriers? */
#define VIRTIO_BLK_F_SIZE_MAX              1  /* Indicates maximum segment size */
#define VIRTIO_BLK_F_SEG_MAX               2  /* Indicates maximum # of segments */
#define VIRTIO_BLK_F_GEOMETRY              4  /* Legacy geometry available  */
#define VIRTIO_BLK_F_RO                    5  /* Disk is read-only */
#define VIRTIO_BLK_F_BLK_SIZE              6  /* Block size of disk is available*/
#define VIRTIO_BLK_F_SCSI                  7  /* Supports scsi command passthru */
#define VIRTIO_BLK_F_FLUSH                 9  /* Flush command supported */
#define VIRTIO_BLK_F_TOPOLOGY              10 /* Topology information is available */
#define VIRTIO_BLK_F_CONFIG_WCE            11 /* Writeback mode available in config */
#define VIRTIO_BLK_F_MQ                    12 /* support more than one vq */
#define VIRTIO_BLK_F_DISCARD               13 /* DISCARD is supported */
#define VIRTIO_BLK_F_WRITE_ZEROES          14 /* WRITE ZEROES is supported */

/* These two define direction. */
#define VIRTIO_BLK_T_IN                    0
#define VIRTIO_BLK_T_OUT                   1

#define VIRTIO_BLK_T_SCSI_CMD              2
#define VIRTIO_BLK_T_FLUSH                 4
#define VIRTIO_BLK_T_GET_ID                8
#define VIRTIO_BLK_T_DISCARD               11
#define VIRTIO_BLK_T_WRITE_ZEROES          13

#define VIRTIO_BLK_WRITE_ZEROES_FLAG_UNMAP 0x00000001

#define VIRTIO_BLK_S_OK                    0
#define VIRTIO_BLK_S_IOERR                 1
#define VIRTIO_BLK_S_UNSUPP                2

#define SECTOR_SIZE                        512
#define SECTOR_SHIFT                       9
#define IO_PORT_LENGTH                     0x40
#define MAX_CPU                            256u

/*
 * QEMU's virtio-blk implementation supports only a single segment (as of
 * QEMU 10.2), so this is more than enough.
 */
#define MAX_DISCARD_SEGMENTS               16u

#define VIRTIO_BLK_QUEUE_LAST              MAX_CPU

#define VIRTIO_BLK_MSIX_CONFIG_VECTOR      0
#define MIN_DISCARD_SECTOR_ALIGNMENT       8

#define BLOCK_SERIAL_STRLEN                20

#define MAX_PHYS_SEGMENTS                  512
#define VIRTIO_MAX_SG                      (3 + MAX_PHYS_SEGMENTS)

/* Keep advertised SRB transfers below the 1MiB boundary where DroidVM's
 * restricted-DMA virtio-blk path can fall back to watchdog-paced completion. */
#define VIOSTOR_MAX_TRANSFER_LENGTH_CAP    (960 * 1024)
#define VIOSTOR_SPLIT_CHILD_LENGTH         VIOSTOR_MAX_TRANSFER_LENGTH_CAP
#define VIOSTOR_SPLIT_MAX_TRANSFER_LENGTH  (1024 * 1024)
#define VIOSTOR_SPLIT_MAX_CHILDREN         2
#define VIOSTOR_SPLIT_MAX_SG               ((VIOSTOR_SPLIT_CHILD_LENGTH / PAGE_SIZE) + 2)

#define VIOBLK_POOL_TAG                    'BoiV'

/* Completion-poll fallback period, microseconds (worst-case extra latency for a
 * completion interrupt that the hypervisor failed to deliver to an idle vCPU). */
#define VIOSTOR_POLL_INTERVAL_US           1000

/*
 * Inline busy-poll on the submit path.
 *
 * On this hypervisor a completion interrupt is not delivered to an idle vCPU,
 * and the StorPort timer that backs the poll fallback (VioStorCompletionPoll)
 * also does not fire promptly once the vCPU has halted (WFI). At low queue
 * depth — e.g. CrystalDiskMark SEQ Q1 or any single-threaded sequential stream —
 * the vCPU goes idle right after submitting, so each request is reaped only by
 * the ~250ms StorPort watchdog. That caps a ~960KB transfer at ~4-5 MB/s even
 * though random Q32 (which keeps the vCPU busy) reaches full speed.
 *
 * The fix that does not depend on the peer waking us: after notifying the
 * device, do NOT let the vCPU halt. Spin-drain the used ring directly (the
 * device publishes completions to shared memory regardless of interrupts) until
 * our request is reaped or a bounded budget elapses. This converts the idle
 * wait into an active wait, so the completion is observed in microseconds.
 *
 * Engaged only when few requests are in flight: at high queue depth the vCPU is
 * already busy submitting/draining and sees completions promptly, so spinning
 * would just waste cycles. At low queue depth the vCPU would otherwise be idle,
 * so the spin costs nothing the workload could have used.
 *
 * IMPORTANT — only for large (sequential) transfers. StorPort calls StartIo
 * serially, so draining to completion inside the submit path prevents the queue
 * from ever building depth: every submit would see a near-empty queue, busy-poll,
 * and complete before the next StartIo, collapsing a deep small-I/O workload to
 * QD1. Small-block workloads (e.g. 4K random) are run at high queue depth where
 * interrupts already work, so they must NOT busy-poll. Gate on transfer size:
 * the idle-vCPU stall only meaningfully caps throughput for big per-I/O payloads
 * (the classic sequential pattern), which is exactly where we want to spin.
 */
#define VIOSTOR_BUSYPOLL_ENABLE            0
/* Only busy-poll when at most this many requests are outstanding on the queue
 * (counts the just-submitted request, including each split child). */
#define VIOSTOR_BUSYPOLL_MAX_INFLIGHT      4
/* Only busy-poll transfers at least this large. Below this, skip so high-QD
 * small-I/O concurrency is preserved (see the serialization note above). */
#define VIOSTOR_BUSYPOLL_MIN_BYTES         (64 * 1024)
/* Total spin budget per submit, microseconds. Bounds DISPATCH_LEVEL spinning;
 * if the device is slower than this the request falls back to the poll timer /
 * IRQ / watchdog as before. */
#define VIOSTOR_BUSYPOLL_BUDGET_US         2000
/* Pause between drain attempts, microseconds. */
#define VIOSTOR_BUSYPOLL_STALL_US          4

#pragma pack(1)
typedef struct virtio_blk_config
{
    /* The capacity (in 512-byte sectors). */
    u64 capacity;
    /* The maximum segment size (if VIRTIO_BLK_F_SIZE_MAX) */
    u32 size_max;
    /* The maximum number of segments (if VIRTIO_BLK_F_SEG_MAX) */
    u32 seg_max;
    /* geometry the device (if VIRTIO_BLK_F_GEOMETRY) */
    struct virtio_blk_geometry
    {
        u16 cylinders;
        u8 heads;
        u8 sectors;
    } geometry;
    /* block size of device (if VIRTIO_BLK_F_BLK_SIZE) */
    u32 blk_size;
    u8 physical_block_exp;
    u8 alignment_offset;
    u16 min_io_size;
    u32 opt_io_size;
    /* writeback mode (if VIRTIO_BLK_F_CONFIG_WCE) */
    u8 wce;
    u8 unused;
    /* number of vqs, only available when VIRTIO_BLK_F_MQ is set */
    u16 num_queues;

    /* the next 3 entries are guarded by VIRTIO_BLK_F_DISCARD */
    /*
     * The maximum discard sectors (in 512-byte sectors) for
     * one segment.
     */
    u32 max_discard_sectors;
    /*
     * The maximum number of discard segments in a
     * discard command.
     */
    u32 max_discard_seg;
    /* Discard commands must be aligned to this number of sectors. */
    u32 discard_sector_alignment;

    /* the next 3 entries are guarded by VIRTIO_BLK_F_WRITE_ZEROES */
    /*
     * The maximum number of write zeroes sectors (in 512-byte sectors) in
     * one segment.
     */
    u32 max_write_zeroes_sectors;
    /*
     * The maximum number of segments in a write zeroes
     * command.
     */
    u32 max_write_zeroes_seg;
    /*
     * Set if a VIRTIO_BLK_T_WRITE_ZEROES request may result in the
     * deallocation of one or more of the sectors.
     */
    u8 write_zeroes_may_unmap;

    u8 unused1[3];
} blk_config, *pblk_config;
#pragma pack()

typedef struct virtio_blk_outhdr
{
    /* VIRTIO_BLK_T* */
    u32 type;
    /* io priority. */
    u32 ioprio;
    /* Sector (ie. 512 byte offset) */
    u64 sector;
} blk_outhdr, *pblk_outhdr;

/* Discard/write zeroes range for each request. */
typedef struct virtio_blk_discard_write_zeroes
{
    /* discard/write zeroes start sector */
    u64 sector;
    /* number of discard/write zeroes sectors */
    u32 num_sectors;
    /* flags for this range */
    u32 flags;
} blk_discard_write_zeroes, *pblk_discard_write_zeroes;

typedef struct virtio_blk_req
{
    LIST_ENTRY list_entry;
    PVOID req;
    ULONG_PTR id;
    blk_outhdr out_hdr;
    u8 status;
} blk_req, *pblk_req;

typedef struct virtio_bar
{
    PHYSICAL_ADDRESS BasePA;
    ULONG uLength;
    PVOID pBase;
    BOOLEAN bPortSpace;
} VIRTIO_BAR, *PVIRTIO_BAR;

typedef struct _SENSE_INFO
{
    UCHAR senseKey;
    UCHAR additionalSenseCode;
    UCHAR additionalSenseCodeQualifier;
} SENSE_INFO, *PSENSE_INFO;

typedef struct _REQUEST_LIST
{
    LIST_ENTRY srb_list;
    ULONG srb_cnt;
    ULONG_PTR next_id;
} REQUEST_LIST, *PREQUEST_LIST;

typedef struct _ADAPTER_EXTENSION
{
    VirtIODevice vdev;

    PVOID pageAllocationVa;
    ULONG pageAllocationSize;
    ULONG pageOffset;

    PVOID poolAllocationVa;
    ULONG poolAllocationSize;
    ULONG poolOffset;

    struct virtqueue *vq[VIRTIO_BLK_QUEUE_LAST];
    ULONG num_queues;
    INQUIRYDATA inquiry_data;
    blk_config info;
    ULONG queue_depth;
    BOOLEAN dump_mode;
    ULONG msix_vectors;
    BOOLEAN msix_enabled;
    /* Standard C type because we do arithmetics with it */
    _Bool msix_has_config_vector;
    ULONGLONG features;
    CHAR sn[BLOCK_SERIAL_STRLEN];
    BOOLEAN sn_ok;
    blk_req vbr;
    BOOLEAN indirect;
    ULONGLONG lastLBA;

    union {
        PCI_COMMON_HEADER pci_config;
        UCHAR pci_config_buf[sizeof(PCI_COMMON_CONFIG)];
    };
    VIRTIO_BAR pci_bars[PCI_TYPE0_ADDRESSES];
    ULONG system_io_bus_number;
    ULONG slot_number;
    ULONG perfFlags;
    PSTOR_DPC dpc;
    BOOLEAN dpc_ok;
    BOOLEAN check_condition;
    SENSE_INFO sense_info;
    BOOLEAN removed;
    BOOLEAN stopped;
    ULONG max_tx_length;
    PGROUP_AFFINITY pmsg_affinity;
    ULONG num_affinity;
    STOR_ADDR_BTL8 device_address;
    REQUEST_LIST processing_srbs[MAX_CPU];
    ULONG reset_in_progress_count;
    ULONGLONG fw_ver;

    /* Restricted DMA pool support */
    BOOLEAN rdmaPoolActive;
    PVOID rdmaPoolBaseVA;
    PHYSICAL_ADDRESS rdmaPoolBasePA;
    ULONG64 rdmaPoolSize;
    PDEVICE_OBJECT rdmaPoolDeviceObject;
    PFILE_OBJECT rdmaPoolFileObject;
    BOUNCE_ALLOCATOR bounce;

    /* Completion-poll fallback (see VioStorCompletionPoll). Some virtio
     * completion interrupts are not delivered to an idle vCPU on this
     * hypervisor, so a request can be reaped only by the ~250ms StorPort
     * watchdog. A lightweight timer drains the used rings while requests are
     * outstanding, turning a missed IRQ into ~1ms of extra latency. Adaptive:
     * armed on submit, self-stops when nothing is in flight. */
    PVOID completionPollTimer;
    LONG pollArmed;

    /* Diagnostic counters for the completion-path investigation: are completion
     * MSI-X interrupts actually reaching the guest, or are completions only being
     * reaped by the poll timer / inline busy-poll? Always compiled (the shipped
     * build is Release, so #ifdef DBG counters would be absent). Dumped
     * periodically via RhelDbgPrint(TRACE_LEVEL_FATAL). */
    volatile LONG dbgIsrCalls;        /* ISR (MSI-X / INTx) invocations */
    volatile LONG dbgCompletedTotal;  /* total requests reaped from used rings */
    volatile LONG dbgCompletedIsr;    /* reaped in ISR context (bIsr == TRUE) */
    volatile LONG dbgPollCalls;       /* completion-poll timer firings */
    volatile LONG dbgSubmitPollCalls; /* inline submit-path busy-poll engagements */

#ifdef DBG
    LONG srb_cnt;
    LONG inqueue_cnt;
#endif
} ADAPTER_EXTENSION, *PADAPTER_EXTENSION;

typedef struct _VRING_DESC_ALIAS
{
    union {
        ULONGLONG data[2];
        UCHAR chars[SIZE_OF_SINGLE_INDIRECT_DESC];
    } u;
} VRING_DESC_ALIAS;

typedef struct _VIOSTOR_SPLIT_CHILD
{
    blk_req vbr;
    ULONG out;
    ULONG in;
    VIO_SG sg[VIOSTOR_SPLIT_MAX_SG];
    PVOID bounceCtl;
    PVOID originalDataVA;
    ULONG bounceDataChunkCount;
    ULONG dataOffset;
    ULONG dataLength;
} VIOSTOR_SPLIT_CHILD, *PVIOSTOR_SPLIT_CHILD;

typedef struct _SRB_EXTENSION
{
    blk_req vbr;
    ULONG_PTR id;
    ULONG out;
    ULONG in;
    ULONG queue_number;
    BOOLEAN fua;
    VIO_SG sg[VIRTIO_MAX_SG];
    VRING_DESC_ALIAS desc[VIRTIO_MAX_SG];
    blk_discard_write_zeroes blk_discard[MAX_DISCARD_SEGMENTS];
    /* Bounce buffer tracking (rdmapool) */
    PVOID bounceCtl;            /* Control slot VA in rdmapool, NULL if not bouncing */
    PVOID originalDataVA;       /* Original data buffer VA for read copy-back */
    ULONG bounceDataChunkCount; /* Number of contiguous data chunks allocated from bounce pool */
    BOOLEAN split;
    ULONG splitChildCount;
    LONG splitRemaining;
    LONG splitStatus;
    VIOSTOR_SPLIT_CHILD splitChildren[VIOSTOR_SPLIT_MAX_CHILDREN];
} SRB_EXTENSION, *PSRB_EXTENSION;

BOOLEAN
VirtIoInterrupt(IN PVOID DeviceExtension);

#ifndef PCIX_TABLE_POINTER
typedef struct
{
    union {
        struct
        {
            ULONG BaseIndexRegister : 3;
            ULONG Reserved : 29;
        };
        ULONG TableOffset;
    };
} PCIX_TABLE_POINTER, *PPCIX_TABLE_POINTER;
#endif

#ifndef PCI_MSIX_CAPABILITY
typedef struct
{
    PCI_CAPABILITIES_HEADER Header;
    struct
    {
        USHORT TableSize : 11;
        USHORT Reserved : 3;
        USHORT FunctionMask : 1;
        USHORT MSIXEnable : 1;
    } MessageControl;
    PCIX_TABLE_POINTER MessageTable;
    PCIX_TABLE_POINTER PBATable;
} PCI_MSIX_CAPABILITY, *PPCI_MSIX_CAPABILITY;
#endif

#endif ___VIOSTOR__H__
