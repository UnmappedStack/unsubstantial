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

/**
 * @brief Swap out a single range to the disk
 * @param range The virtual memory range to swap out
 */
void pager_swapOutRange(vmm_to_swap_range_t *range) {
    // Copy the range to disk (or wherever its saved)
//    uintptr_t paddr = arch_mmu_physical(range->);

    // Save in the region the offset on the disk its stored at
    

    // Remove from to-swap list
    

}

/**
 * @brief Entry point of the pager thread
 */
void pager_threadEntry(void) {
    // TODO: We'll need to also have this be alerted on page fault when a non
    // resident page is requested.
    // TODO: We need to mark some global value when we're ready so that the VMM
    // doesn't try swap out stuff before the pager exists (that'd be bad!)
    for (;;) {
        // Keep checking the to-swap region list and, well... swap it out
        vmm_to_swap_range_t *range = to_swap_head;
        if (!range) continue; // Maybe this should yield?

        // We have something to swap out
        pager_swapOutRange(range);
    }

    // We should never exit!
    assert(false && "Pager thread exited!");
}
