/**
 * @file hexahedron/mm/pager.c
 * @brief Hexahedron virtual memory swapper
 *
 * Responsible for the kernel thread which moves some memory regions to disk
 * when resident memory is getting full (but hexahedron/mm/vmm.c is responsible
 * for actually finding said memory regions to swap out!).
 * 
 * @copyright
 * This file is part of the Hexahedron kernel, which is part of the Ethereal Operating System.
 * It is released under the terms of the BSD 3-clause license.
 * Please see the LICENSE file in the main repository for more details.
 * 
 * Copyright (C) 2026 Jake Steinburger and Samuel Stuart
 */

#include <kernel/mm/vmm.h>
#include <kernel/debug.h>
#include <kernel/task/process.h>
#include <stdarg.h>

/* Log method */
#define LOG(status, ...) dprintf_module(status, "MM:PAGER", __VA_ARGS__)

#define HERE(x) LOG(DEBUG, "HERE: %u", x)

// Address of memory where everything will (temporarily) be swapped out to
// rather than disk, for testing
#define FIXED_MAX_SWAP_SIZE (PAGE_SIZE * 100000ULL)
static uint8_t *swap_target;
static size_t swapfile_offset_upto = 0;

/**
 * @brief Swap out a single range to the disk
 * @param range The virtual memory range to swap out
 */
void pager_swapOutRange(vmm_to_swap_range_t *range) {
    mutex_acquire(&range->range->mut);
    if (range->space->start >= vmm_kernel_context->space->start) {
        // Kind of hacky solution, but sometimes ranges in kernel memory
        // are *magically* put into the to-swap queue despite being supposedly
        // filtered out first... FIXME
        vmm_removeFromToSwapList(range->space, range->range);
        mutex_release(&range->range->mut);
        return;
    }
    uintptr_t range_bytes = range->range->end - range->range->start;
    vmm_context_t *context = vmm_spaceToContext(range->space);
    assert(context->space == range->space && "this will prolly fail idek");
    if (swapfile_offset_upto + range_bytes >= FIXED_MAX_SWAP_SIZE) {
        // Not enough swap space (later we can make this just grow)
        mutex_release(&range->range->mut);
        return;
    }

    // Copy the range to a buffer we can access from this memory space
    uint8_t *buf = vmm_map(NULL, range_bytes, VM_FLAG_ALLOC, MMU_FLAG_WRITE | MMU_FLAG_PRESENT);
    for (uintptr_t addr = range->range->start; addr < range->range->end; addr += PAGE_SIZE) {
        void *src = (void*) arch_mmu_remap_physical(arch_mmu_physical(context->dir, addr), PAGE_SIZE, REMAP_TEMPORARY);
        memcpy(&buf[addr - range->range->start], src, PAGE_SIZE);
    }
    
    // Save in the region the offset on the disk its stored at
    range->range->swap_loc_offset = swapfile_offset_upto;
    range->range->swapped_out = true;

    // Save it to wherever its being stored (temporarily just some place in
    // memory for testing, should be disk later)
    memcpy(&swap_target[swapfile_offset_upto], buf, range_bytes);
    LOG(DEBUG, "SWAP REGION: swap addr %p to offset in swapfile %p\n", range->range->start, swapfile_offset_upto);
    swapfile_offset_upto += PAGE_ALIGN_UP(range_bytes);
    vmm_unmap(buf, range_bytes);

    vmm_removeFromToSwapList(range->space, range->range);

    // Free the physical memory (woah the whole purpose of this thing :nekocatwoah:)
    for (uintptr_t addr = range->range->start; addr < range->range->end; addr += PAGE_SIZE) {
        uint64_t new_flags = arch_mmu_read_flags(context->dir, addr) & ~MMU_FLAG_PRESENT;
        arch_mmu_setflags(context->dir, addr, new_flags);
        uintptr_t phys = arch_mmu_physical(context->dir, addr);
        if (phys) pmm_freePage(phys);
    }
    mutex_release(&range->range->mut);
}

/**
 * @brief Swap back in a range from disk
 * @param sp The space range is within
 * @param range The range to swap in
 * @returns VMM_FAULT_RESOLVED on success and VMM_FAULT_UNRESOLVED on failure
 */
int pager_swapBackIn(vmm_space_t *sp, vmm_memory_range_t *range) {
    LOG(WARN, "range->is_swapped_out = %u, start = %p\n", range->swapped_out, range->start);
    mutex_acquire(&range->mut);
    uintptr_t range_bytes = range->end - range->start;
    range->mmu_flags |= MMU_FLAG_PRESENT;

    uintptr_t new_phys = pmm_allocatePages(PAGE_ALIGN_UP(range_bytes)/PAGE_SIZE, ZONE_DEFAULT);
    if (!new_phys) {
        LOG(WARN, "failed to allocate memory to swap region back in, likely oom\n");
        mutex_release(&range->mut);
        return VMM_FAULT_UNRESOLVED;
    }

    vmm_context_t *ctx = vmm_spaceToContext(sp);
    for (uintptr_t offset = 0; offset < range_bytes; offset += PAGE_SIZE) {
        arch_mmu_map(ctx->dir, range->start + offset, new_phys + offset, range->mmu_flags);
    }

    memcpy((void*)range->start, &swap_target[range->swap_loc_offset], range_bytes);
    LOG(DEBUG, "deswap into %p (offset %p)\n",
            range->start, range->swap_loc_offset);
    range->to_be_swapped_out = range->swapped_out = false;

    arch_mmu_invalidate_range(range->start, range->end);

    // TODO: re-insert it into the resident page list, otherwise it can't be swapped out again
    mutex_release(&range->mut);
    return VMM_FAULT_RESOLVED;
}

/**
 * @brief Entry point of the pager thread
 */
void pager_threadEntry(void) {
    swap_target = vmm_map(NULL, FIXED_MAX_SWAP_SIZE, VM_FLAG_ALLOC,
                                    MMU_FLAG_WRITE | MMU_FLAG_PRESENT);

    for (;;) {
        process_yield(1);

        // Keep checking the to-swap region list and, well... swap it out
        vmm_to_swap_range_t *range = to_swap_head;
        if (!range) continue; // Maybe this should yield?

        // We have something to swap out
        pager_swapOutRange(range);
    }

    // We should never exit!
    __builtin_unreachable();
}
