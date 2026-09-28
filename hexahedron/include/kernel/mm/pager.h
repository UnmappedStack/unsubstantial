/**
 * @file hexahedron/include/kernel/mm/pager.h
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

/**
 * @brief Entry point of the pager thread
 */
void pager_threadEntry(void);
