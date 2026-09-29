/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Embeds the raw XRGB8888 boot splash pixel data (see
 * spacemit_bootsplash.c) directly into spacemit-drm.ko.
 *
 * SPACEMIT_BOOTSPLASH_RGB_PATH is passed in as an absolute, $(srctree)-
 * prefixed path by the Makefile (AFLAGS_spacemit_bootsplash_data.o) --
 * a bare tree-relative path isn't reliably resolved by .incbin in this
 * build (Bazel/Kleaf's out-of-tree sandbox doesn't search $(srctree) the
 * way a plain in-tree build does). This mirrors the same technique used
 * by drivers/base/firmware_loader/builtin/Makefile for builtin firmware.
 */
#ifndef SPACEMIT_BOOTSPLASH_RGB_PATH
#define SPACEMIT_BOOTSPLASH_RGB_PATH "drivers/gpu/drm/spacemit/spacemit_bootsplash.rgb"
#endif

	.section .rodata, "a"

	.globl spacemit_bootsplash_data
	.globl spacemit_bootsplash_data_end
	.balign 8
spacemit_bootsplash_data:
	/* cache-bust marker: bump this comment whenever .rgb content
	 * changes -- Bazel does not track .incbin's file dependency, so
	 * this is what actually invalidates the cached object. rev 5:
	 * new pre-cropped k1_riscv_kernel_splash.bmp source image. */
	.incbin SPACEMIT_BOOTSPLASH_RGB_PATH
spacemit_bootsplash_data_end:

	.previous
