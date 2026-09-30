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

// This is hardcoded, the second the partition is smaller than expected it will break...
// FIXME
#define FIXED_MAX_SWAP_SIZE (PAGE_SIZE * 100000ULL)
static size_t swapfile_offset_upto = 0;
vfs_file_t *backing = NULL;

/**
 * @brief Write some memory to the swap disk
 * @param range The batched pages to write
 */
void pager_writeToDisk(page_range_t *range) {
    assert(backing);
    backing->inode->c_ops->write_range(backing->inode, range);
}

/**
 * @brief Read some memory from the swap disk into a buffer
 * @param range The range to read into
 */
void pager_readFromDisk(page_range_t *range) {
    assert(backing);
    backing->inode->c_ops->read_range(backing->inode, range);
}

/**
 * @brief Swap out a single range to the disk
 * @param range The virtual memory range to swap out
 */
void pager_swapOutRange(vmm_to_swap_range_t *range) {
    mutex_acquire(&range->range->mut);
    if (!backing) {
        // swap disk isn't yet initialised, so try that first
        if (vfs_open("/device/sata0", O_RDWR, &backing)) {
            // still not initialised
            backing = NULL;
            mutex_release(&range->range->mut);
            return;
        }
    }

    uintptr_t range_bytes = range->range->end - range->range->start;
    vmm_context_t *context = vmm_spaceToContext(range->space);
    if (swapfile_offset_upto + range_bytes >= FIXED_MAX_SWAP_SIZE) {
        // Not enough swap space (later we can make this just grow)
        mutex_release(&range->range->mut);
        return;
    }

    // Copy the range to a buffer we can access from this memory space
    page_range_t *buf = vmm_map(NULL, range_bytes + sizeof(page_range_t), VM_FLAG_ALLOC, MMU_FLAG_WRITE | MMU_FLAG_PRESENT);
    assert(buf);
    buf->offset = 0;
    buf->npages = range_bytes / PAGE_SIZE;
    for (uintptr_t addr = range->range->start; addr < range->range->end; addr += PAGE_SIZE) {
        void *src = (void*) arch_mmu_remap_physical(arch_mmu_physical(context->dir, addr), PAGE_SIZE, REMAP_TEMPORARY);
        memcpy(&((uint8_t*)buf->pages)[addr - range->range->start], src, PAGE_SIZE);
    }
    
    // Save in the region the offset on the disk its stored at
    range->range->swap_loc_offset = swapfile_offset_upto;
    range->range->swapped_out = true;
    range->range->to_be_swapped_out = false;

    // Save it to wherever its being stored
    pager_writeToDisk(buf);
    swapfile_offset_upto += PAGE_ALIGN_UP(range_bytes);
    vmm_unmap(buf, range_bytes + sizeof(page_range_t));

    vmm_removeFromToSwapList(range->space, range->range);
    mutex_release(&range->range->mut);

    // Free the physical memory (woah the whole purpose of this thing :nekocatwoah:)
    for (uintptr_t addr = range->range->start; addr < range->range->end; addr += PAGE_SIZE) {
        if (range->range->vmm_flags & VM_FLAG_ALLOC) {
            uintptr_t pg = arch_mmu_physical(NULL, addr);
            range->space->metrics.anon_resident -= PAGE_SIZE;
            if (pg) {
                range->space->metrics.anon_resident -= PAGE_SIZE;
                pmm_freePage(pg);
            }
        }
        arch_mmu_unmap(NULL, addr);
    }
    arch_mmu_invalidate_range(range->range->start, range->range->end);
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

    // Sadly we need an intermediate buffer :( TODO: sassydallas please make your vfs nice so its possible
    // to copy into a buffer where the page_range_t doesn't have to be immediately before in memory
    page_range_t *buf = vmm_map(NULL, range_bytes + sizeof(page_range_t), VM_FLAG_ALLOC, MMU_FLAG_WRITE | MMU_FLAG_PRESENT);
    buf->offset = 0;
    buf->npages = range_bytes / PAGE_SIZE;
    pager_readFromDisk(buf);
    memcpy((void*)range->start, buf->pages, range_bytes);
    vmm_unmap(buf, range_bytes + sizeof(page_range_t));

    LOG(DEBUG, "deswap into %p (offset %p)\n",
            range->start, range->swap_loc_offset);
    range->to_be_swapped_out = range->swapped_out = false;

    arch_mmu_invalidate_range(range->start, range->end);

    range->swapped_out = false;

    // TODO: re-insert it into the resident page list, otherwise it can't be swapped out again
    mutex_release(&range->mut);
    return VMM_FAULT_RESOLVED;
}

/**
 * @brief Entry point of the pager thread
 */
void pager_threadEntry(void) {
    for (;;) {
        process_yield(1);

        // Keep checking the to-swap region list and, well... swap it out
        vmm_to_swap_range_t *range = to_swap_head;
        if (!range) continue;

        // We have something to swap out
        pager_swapOutRange(range);
    }

    // We should never exit!
    __builtin_unreachable();
}
