/*
 *  Driver for the Elgato 4k60 Pro MK.2 HDMI capture card.
 *
 *  Copyright (c) 2021-2022 Steven Toth <stoth@kernellabs.com>
 *  Modifications Copyright (c) 2025-2026 Nakildias <nakildiaspro@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* Each FPGA descriptor is 8xDWORD.
 * We're going to have 8 descriptors per channel, where
 * each descriptor is either a frame of video or a 'chunk' (TBD)
 * of audio.
 * The entire descriptor pagetable for a single channel will fit
 * inside a single page of memory.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include "sc0710.h"

#define dprintk(level, fmt, arg...)\
        do { if (sc0710_debug_mode >= level)\
                printk(KERN_DEBUG "%s: " fmt, dev->name, ## arg);\
        } while (0)

/* Copy the chain contents into a target buffer, don't overflow.
 * Return numbers of bytes, or < 0 if overflow detected.
 */
int sc0710_dma_chain_dq_to_ptr(struct sc0710_dma_channel *ch, struct sc0710_dma_descriptor_chain *chain, u8 *dst, int dstlen)
{
	struct sc0710_dma_descriptor_chain_allocation *dca = &chain->allocations[0];
	int len = 0;
	int i;

	for (i = 0; i < chain->numAllocations; i++) {
		if (len + dca->buf_size <= dstlen) {
			memcpy(dst + len, dca->buf_cpu, dca->buf_size);
			len += dca->buf_size;
		} else {
			return -EOVERFLOW;
		}
		dca++;
	}

	return len;
}

void sc0710_dma_chain_dump(struct sc0710_dma_channel *ch, struct sc0710_dma_descriptor_chain *chain, int nr)
{
	struct sc0710_dma_descriptor_chain_allocation *dca = &chain->allocations[0];
	struct sc0710_dma_descriptor *desc;
	int i;

	printk("               chain[%02d]  %p -- enabled %d total_transfer_size 0x%x numAllocations %d\n",
		nr,
		chain, chain->enabled, chain->total_transfer_size, chain->numAllocations);

	for (i = 0; i < chain->numAllocations; i++) {
		desc = dca->desc;
		printk("                          [%02d] enabled %d buf_size 0x%x buf_dma %llx buf_cpu %p  wbm: %p / %p\n",
			i,
			dca->enabled,
			dca->buf_size,
			dca->buf_dma,
			dca->buf_cpu,
			dca->wbm[0],
			dca->wbm[1]);
		printk("                               %08x %08x %08x %08x %08x %08x %08x %08x\n",
			desc->control,
			desc->lengthBytes,
			desc->src_l,
			desc->src_h,
			desc->dst_l,
			desc->dst_h,
			desc->next_l,
			desc->next_h);
		dca++;
	}
}

void sc0710_dma_chain_free(struct sc0710_dma_channel *ch, int nr)
{
	struct sc0710_dev *dev = ch->dev;
	struct sc0710_dma_descriptor_chain *chain = &ch->chains[nr];
	struct sc0710_dma_descriptor_chain_allocation *dca = &chain->allocations[0];
	int i;

	dprintk(1, "%s(ch#%d)\n", __func__, nr);

	chain->enabled = 0;

	for (i = 0; i < chain->numAllocations; i++) {
		if (dca->buf_cpu) {
			dma_free_coherent(&dev->pci->dev, dca->buf_size, dca->buf_cpu, dca->buf_dma);
			dca->buf_cpu = NULL;
			dca->buf_dma = 0;
		}
		dca++;
	}
	chain->numAllocations = 0;
}

int sc0710_dma_chain_alloc(struct sc0710_dma_channel *ch, int nr, int total_transfer_size)
{
	struct sc0710_dev *dev = ch->dev;
	struct sc0710_dma_descriptor_chain *chain = &ch->chains[nr];
	struct sc0710_dma_descriptor_chain_allocation *dca = &chain->allocations[0];
	int rem = total_transfer_size;
	int size;
	/* Was 4 * 1048576 (2^22), then 1572864 (1.5 MiB) - see STATUS.md PICK UP
	 * HERE (21) for that earlier history. Settled here at 1920 KiB (STATUS.md
	 * PICK UP HERE (64)/(68)/(69)): the sc0710_dma_segsize_kb bisection sweep
	 * ((47)-(64)) never found a deterministic segsize ceiling - every
	 * single-sample "bad" verdict at nearby sizes was later overturned by a
	 * multi-run retest, pointing to a real but still-unexplained intermittent
	 * hardware/timing issue rather than a fixable segsize choice. 1920 KiB and
	 * 1888 KiB were the two sizes that came out multi-run-clean (130/130
	 * steady-state frames, only the known segsize-independent warm-up
	 * artifact in frames 0-3). Three separate Ghidra passes on YUAN's own
	 * SC0710 driver ((66)-(69)) didn't find a different completion strategy
	 * or a wrong-firmware explanation either - this value is the practical
	 * resolution, not a proven-correct one. */
	int segsize = 1920 * 1024; /* 1920 KiB */

	/* Zero-copy: split video chains finer. More (smaller) descriptors per
	 * chain keep a rewritten chain's ring slots beyond the device's
	 * descriptor read-ahead, and shrink the contiguous pieces a client
	 * buffer must provide to back a chain. */
	if (zero_copy && zc_split && ch->mediatype == CHTYPE_VIDEO) {
		u32 split = min_t(u32, zc_split, SC0710_MAX_CHAIN_DESCRIPTORS);

		segsize = ALIGN(DIV_ROUND_UP(total_transfer_size, split), 4096);
	} else if (sc0710_dma_segsize_kb) {
		/* TEST (STATUS.md PICK UP HERE 47): bisect the real safe single-DMA-
		 * burst ceiling directly via a load-time size instead of the two
		 * extremes ((38)'s whole-transfer single segment vs. the original
		 * 1.5 MiB) tried so far. */
		segsize = sc0710_dma_segsize_kb * 1024;
	} else if (sc0710_single_dma_segment) {
		/* TEST (STATUS.md PICK UP HERE 38): the vendor driver's own
		 * per-frame DMA buffer allocation scheme couldn't be confirmed
		 * from the Windows binary (its AllocateCommonBuffer call goes
		 * through an untyped DMA_ADAPTER vtable dispatch) - the
		 * 1.5 MiB chained-segment split above is entirely this port's
		 * own invention (PICK UP HERE 21), not known to match what
		 * the hardware actually expects. Force exactly one segment,
		 * sized to the whole transfer, to test empirically whether
		 * the persistent segment-boundary tail-zero holes are an
		 * artifact of chaining multiple descriptors together at all,
		 * independent of whatever segment size is chosen. */
		segsize = total_transfer_size;
	}

	chain->enabled = 1;
	chain->total_transfer_size = total_transfer_size;
	chain->numAllocations = 0;

	/* First thing we should do is determine all of the allocations
	 * for the total transfer_size, build the segment sizes and alloc
	 * in the PCI DMA space.
	 */
	while (rem > 0) {
		/* Determine the size of this dma allocation segment. */
		if (rem > segsize)
			size = segsize;
		else
			size = rem;

		/* We can't fit the transfer in our statically allocated structs. */
		if (chain->numAllocations == SC0710_MAX_CHAIN_DESCRIPTORS)
			return -ENOMEM;

		dca->enabled = 1;
		dca->buf_size = size;
		/* GFP_KERNEL: every caller (probe, STREAMON, the kthreads) is
		 * sleepable process context. */
		dca->buf_cpu = dma_alloc_coherent(&dev->pci->dev, dca->buf_size, &dca->buf_dma, GFP_KERNEL);
		if (dca->buf_cpu == 0)
			return -ENOMEM;

		memset(dca->buf_cpu, 0, dca->buf_size);

		chain->numAllocations++;
		dca++;
		rem -= size;
	}
	return 0; /* Success */
}
