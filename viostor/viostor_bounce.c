/*
 * Bounce buffer allocator for restricted DMA pool (rdmapool) support.
 *
 * Uses lock-free SLIST for both control slots and data pages,
 * making it safe to call at any IRQL including DIRQL.
 */

#include "virtio_stor.h"
#include "viostor_bounce.h"

#if defined(EVENT_TRACING)
#include "viostor_bounce.tmh"
#endif

NTSTATUS
BounceInit(PBOUNCE_ALLOCATOR Alloc,
           PUCHAR BaseVA,
           PHYSICAL_ADDRESS BasePA,
           SIZE_T TotalSize,
           ULONG CtlSlotCount,
           ULONG DataChunkSize)
{
    ULONG ctlRegionSize;
    ULONG totalPages;
    ULONG dataPages;
    ULONG chunkPages;
    ULONG chunkCount;
    ULONG i;
    PUCHAR ptr;

    RtlZeroMemory(Alloc, sizeof(*Alloc));

    totalPages = (ULONG)(TotalSize / PAGE_SIZE);
    ctlRegionSize = CtlSlotCount * BOUNCE_CTL_PAGES;

    if (totalPages <= ctlRegionSize)
    {
        RhelDbgPrint(TRACE_LEVEL_ERROR,
                     " BounceInit: insufficient pages (%u) for %u control slots\n",
                     totalPages,
                     CtlSlotCount);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    dataPages = totalPages - ctlRegionSize;

    /* Decide the final chunk size. A chunk must be physically contiguous (it is:
     * the whole rdmapool region is contiguous and chunks are laid out linearly).
     * Bigger chunks => fewer descriptors per large transfer, but fewer total
     * chunks => less small-I/O concurrency. Balance the two:
     *   - The caller already capped the request to the device size_max.
     *   - Reduce it further if needed so the pool yields at least CtlSlotCount
     *     chunks, i.e. one per in-flight request (queue_depth). Otherwise a
     *     request could hold a control slot but starve for a data chunk and
     *     bounce back as SRB_STATUS_BUSY.
     * Then page-align down and clamp into [PAGE_SIZE, dataRegion]. */
    if (CtlSlotCount > 0)
    {
        ULONG maxForConcurrency = (dataPages / CtlSlotCount) * PAGE_SIZE;
        if (DataChunkSize > maxForConcurrency)
        {
            DataChunkSize = maxForConcurrency;
        }
    }
    DataChunkSize &= ~(PAGE_SIZE - 1);
    if (DataChunkSize < PAGE_SIZE)
    {
        DataChunkSize = PAGE_SIZE;
    }
    chunkPages = DataChunkSize / PAGE_SIZE;
    if (chunkPages > dataPages)
    {
        chunkPages = dataPages;
        DataChunkSize = chunkPages * PAGE_SIZE;
    }
    chunkCount = dataPages / chunkPages;

    Alloc->BaseVA = BaseVA;
    Alloc->BasePA = BasePA;
    Alloc->TotalPages = totalPages;

    /* Control slot region starts at BaseVA */
    Alloc->CtlBaseVA = BaseVA;
    Alloc->CtlSlotCount = CtlSlotCount;
    InitializeSListHead(&Alloc->CtlFreeList);

    for (i = 0; i < CtlSlotCount; i++)
    {
        ptr = BaseVA + (SIZE_T)i * BOUNCE_CTL_SIZE;
        RtlZeroMemory(ptr, BOUNCE_CTL_SIZE);
        InterlockedPushEntrySList(&Alloc->CtlFreeList, (PSLIST_ENTRY)ptr);
    }

    /* Data chunk region starts after control slots. Each chunk is one
     * contiguous DataChunkSize run; tail pages that don't fill a whole chunk
     * are left unused. */
    Alloc->DataBaseVA = BaseVA + (SIZE_T)ctlRegionSize * PAGE_SIZE;
    Alloc->DataChunkSize = DataChunkSize;
    Alloc->DataChunkCount = chunkCount;
    InitializeSListHead(&Alloc->DataFreeList);

    for (i = 0; i < chunkCount; i++)
    {
        ptr = Alloc->DataBaseVA + (SIZE_T)i * DataChunkSize;
        InterlockedPushEntrySList(&Alloc->DataFreeList, (PSLIST_ENTRY)ptr);
    }

    Alloc->Initialized = TRUE;

    RhelDbgPrint(TRACE_LEVEL_INFORMATION,
                 " BounceInit: %u ctl slots (%u pages), %u data chunks x %u KB (%u pages), total %u pages\n",
                 CtlSlotCount,
                 ctlRegionSize,
                 chunkCount,
                 DataChunkSize / 1024,
                 dataPages,
                 totalPages);

    return STATUS_SUCCESS;
}

PVOID
BounceAllocCtl(PBOUNCE_ALLOCATOR Alloc)
{
    return (PVOID)InterlockedPopEntrySList(&Alloc->CtlFreeList);
}

VOID BounceFreeCtl(PBOUNCE_ALLOCATOR Alloc, PVOID CtlVA)
{
    InterlockedPushEntrySList(&Alloc->CtlFreeList, (PSLIST_ENTRY)CtlVA);
}

PVOID
BounceAllocDataChunk(PBOUNCE_ALLOCATOR Alloc)
{
    return (PVOID)InterlockedPopEntrySList(&Alloc->DataFreeList);
}

VOID BounceFreeDataChunk(PBOUNCE_ALLOCATOR Alloc, PVOID ChunkVA)
{
    InterlockedPushEntrySList(&Alloc->DataFreeList, (PSLIST_ENTRY)ChunkVA);
}
