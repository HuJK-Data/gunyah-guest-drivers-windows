/*
 * Bounce buffer allocator for restricted DMA pool (rdmapool) support.
 *
 * In protected VMs, all DMA target buffers must reside in the restricted
 * DMA pool (shared memory). This module provides lock-free (SLIST-based)
 * allocators for:
 *   - Control slots: fixed-size contiguous blocks for request metadata
 *     (out_hdr, status, blk_discard, sn, indirect descriptor table)
 *   - Data pages: individual 4KB pages for I/O data bounce buffering
 *
 * Both allocators are safe at any IRQL including DIRQL.
 */

#ifndef _VIOSTOR_BOUNCE_H_
#define _VIOSTOR_BOUNCE_H_

#include <ntddk.h>

/* Number of pages per control slot:
 *   Page 0: out_hdr (16B) + status (1B) + blk_discard[16] (256B) + sn (20B)
 *   Pages 1-3: indirect descriptor table (up to 515*16 = 8240B)
 */
#define BOUNCE_CTL_PAGES           4
#define BOUNCE_CTL_SIZE            (BOUNCE_CTL_PAGES * PAGE_SIZE)

/* Offsets within a control slot (page 0) */
#define BOUNCE_CTL_OUTHDR_OFFSET   0
#define BOUNCE_CTL_STATUS_OFFSET   16 /* sizeof(blk_outhdr): u32 + u32 + u64 */
#define BOUNCE_CTL_DISCARD_OFFSET  32
#define BOUNCE_CTL_SN_OFFSET       288

/* Indirect descriptor table starts at page 1 */
#define BOUNCE_CTL_INDIRECT_OFFSET PAGE_SIZE

/*
 * Data bounce region is carved into fixed-size CONTIGUOUS chunks rather than
 * single 4KB pages. Each chunk is physically contiguous, so a chunk maps to a
 * single virtqueue descriptor (provided chunk size <= device size_max). This
 * collapses a large transfer from one-descriptor-per-page (e.g. 256 for 1MB)
 * down to ceil(transfer / chunk) descriptors (e.g. 6 for 1MB @ 192KB chunks),
 * which is the dominant cost for the device/hypervisor in a protected VM.
 * Keep chunks below 256KB so 1MB requests do not form a 4 x 256KB descriptor
 * pattern, which is watchdog-paced on the DroidVM restricted-DMA storage path.
 *
 * The chunk size is clamped down to the device's size_max, and further reduced
 * if needed so the pool yields at least CtlSlotCount (queue_depth) chunks (see
 * BounceInit) — so large transfers stay cheap without starving small-I/O
 * concurrency.
 */
#define BOUNCE_DATA_CHUNK_SIZE     (192 * 1024)

typedef struct _BOUNCE_ALLOCATOR
{
    PUCHAR BaseVA;
    PHYSICAL_ADDRESS BasePA;
    ULONG TotalPages;

    /* Control slot free list (lock-free SLIST) */
    DECLSPEC_ALIGN(MEMORY_ALLOCATION_ALIGNMENT) SLIST_HEADER CtlFreeList;
    ULONG CtlSlotCount;
    PUCHAR CtlBaseVA;

    /* Data chunk free list (lock-free SLIST). Each entry is one contiguous
     * DataChunkSize-byte chunk. */
    DECLSPEC_ALIGN(MEMORY_ALLOCATION_ALIGNMENT) SLIST_HEADER DataFreeList;
    ULONG DataChunkSize;  /* bytes per chunk (page-aligned, <= device size_max) */
    ULONG DataChunkCount; /* number of chunks carved from the data region */
    PUCHAR DataBaseVA;

    BOOLEAN Initialized;
} BOUNCE_ALLOCATOR, *PBOUNCE_ALLOCATOR;

/*
 * Initialize the bounce allocator.
 * baseVA/basePA: start of available rdmapool region (after ring buffers).
 * totalSize: bytes available.
 * ctlSlotCount: number of control slots to pre-allocate (typically queue_depth).
 * dataChunkSize: requested bytes per data chunk (clamped to PAGE_SIZE..region).
 */
NTSTATUS
BounceInit(PBOUNCE_ALLOCATOR Alloc,
           PUCHAR BaseVA,
           PHYSICAL_ADDRESS BasePA,
           SIZE_T TotalSize,
           ULONG CtlSlotCount,
           ULONG DataChunkSize);

/* Allocate a control slot. Returns VA or NULL if exhausted. */
PVOID BounceAllocCtl(PBOUNCE_ALLOCATOR Alloc);

/* Free a control slot back to the pool. */
VOID BounceFreeCtl(PBOUNCE_ALLOCATOR Alloc, PVOID CtlVA);

/* Allocate one contiguous data chunk (DataChunkSize bytes). NULL if exhausted. */
PVOID BounceAllocDataChunk(PBOUNCE_ALLOCATOR Alloc);

/* Free a data chunk back to the pool. */
VOID BounceFreeDataChunk(PBOUNCE_ALLOCATOR Alloc, PVOID ChunkVA);

/* Convert a bounce VA to its physical address. */
FORCEINLINE
PHYSICAL_ADDRESS
BounceVAtoPA(PBOUNCE_ALLOCATOR Alloc, PVOID VA)
{
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = Alloc->BasePA.QuadPart + ((PUCHAR)VA - Alloc->BaseVA);
    return pa;
}

/* Convert a bounce PA to its virtual address. */
FORCEINLINE
PVOID
BouncePAtoVA(PBOUNCE_ALLOCATOR Alloc, PHYSICAL_ADDRESS PA)
{
    return (PVOID)(Alloc->BaseVA + (PA.QuadPart - Alloc->BasePA.QuadPart));
}

/*
 * Cleanup macro: free all bounce resources for an SRB.
 * Safe to call even when bounceCtl is NULL (no-op).
 * Each data chunk occupies one sg[] entry (sg[1..bounceDataChunkCount]); the
 * entry's physAddr is the chunk base, so we recover the chunk VA from it.
 */
#define BOUNCE_CLEANUP_SRB(pAdaptExt, pSrbExt)                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((pSrbExt)->bounceCtl)                                                                                      \
        {                                                                                                              \
            ULONG _bci;                                                                                                \
            for (_bci = 1; _bci <= (pSrbExt)->bounceDataChunkCount; _bci++)                                            \
            {                                                                                                          \
                BounceFreeDataChunk(&(pAdaptExt)->bounce,                                                              \
                                    BouncePAtoVA(&(pAdaptExt)->bounce, (pSrbExt)->sg[_bci].physAddr));                 \
            }                                                                                                          \
            BounceFreeCtl(&(pAdaptExt)->bounce, (pSrbExt)->bounceCtl);                                                 \
            (pSrbExt)->bounceCtl = NULL;                                                                               \
            (pSrbExt)->bounceDataChunkCount = 0;                                                                       \
        }                                                                                                              \
    } while (0)

#define BOUNCE_CLEANUP_SPLIT_CHILD(pAdaptExt, pChild)                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((pChild)->bounceCtl)                                                                                       \
        {                                                                                                              \
            ULONG _bci;                                                                                                \
            for (_bci = 1; _bci <= (pChild)->bounceDataChunkCount; _bci++)                                             \
            {                                                                                                          \
                BounceFreeDataChunk(&(pAdaptExt)->bounce,                                                              \
                                    BouncePAtoVA(&(pAdaptExt)->bounce, (pChild)->sg[_bci].physAddr));                  \
            }                                                                                                          \
            BounceFreeCtl(&(pAdaptExt)->bounce, (pChild)->bounceCtl);                                                  \
            (pChild)->bounceCtl = NULL;                                                                                \
            (pChild)->bounceDataChunkCount = 0;                                                                        \
        }                                                                                                              \
    } while (0)

#endif /* _VIOSTOR_BOUNCE_H_ */
