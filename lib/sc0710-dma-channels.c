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

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include "sc0710.h"

int sc0710_dma_channels_resize(struct sc0710_dev *dev)
{
	int ret = 0;

	printk(KERN_DEBUG "%s()\n", __func__);
	switch (dev->board) {
	case SC0710_BOARD_ELGATEO_4KP60_MK2:
	case SC0710_BOARD_ELGATEO_4KP:
		/* REVERTED 2026-08-30 (see STATUS.md PICK UP HERE (14)): the
		 * video/audio base swap tried here was never actually tested
		 * on hardware (reload silently failed, confirmed via
		 * srcversion mismatch) -- and a later warm-restart test
		 * against the ORIGINAL un-swapped 0x1000/0x1100 bases got
		 * real wbm data on ch#0 (video, 0x1000) on a second
		 * stream-start within the same module load. That falsifies
		 * the "0x1000 never completes" premise the swap was based on.
		 * Back to the receiver-wired original bases. */
		ret = sc0710_dma_channel_resize(dev, 0, CHDIR_INPUT, 0x1000, CHTYPE_VIDEO);
		/* Audio uses fixed buffer size, do not resize as it may be active via ALSA */
		/* sc0710_dma_channel_resize(dev, 1, CHDIR_INPUT, 0x1100, CHTYPE_AUDIO); */
		break;
	}

	return ret;
}

int sc0710_dma_channels_alloc(struct sc0710_dev *dev)
{
	int ret = 0;

	switch (dev->board) {
	case SC0710_BOARD_ELGATEO_4KP60_MK2:
	case SC0710_BOARD_ELGATEO_4KP:
		/* REVERTED 2026-08-30 -- see comment in
		 * sc0710_dma_channels_resize() above and STATUS.md
		 * PICK UP HERE (14). The swap tried in PICK UP HERE (13) was
		 * never actually tested and is no longer well-supported --
		 * back to the original, receiver-wired bases. */
		ret = sc0710_dma_channel_alloc(dev, 0, CHDIR_INPUT, 0x1000, CHTYPE_VIDEO);
		if (ret == 0)
			ret = sc0710_dma_channel_alloc(dev, 1, CHDIR_INPUT, 0x1100, CHTYPE_AUDIO);
		if (ret < 0)
			sc0710_dma_channels_free(dev);
		break;
	}

	return ret;
}

void sc0710_dma_channels_free(struct sc0710_dev *dev)
{
	int i;

	for (i = 0; i < SC0710_MAX_CHANNELS; i++) {
		sc0710_dma_channel_free(dev, i);
	}
}

/* Port real-data-path enable/mode bits, BAR0+0xEC. Confirmed via Ghidra
 * decompile of the vendor Windows driver (FUN_140265050, the function behind
 * its "ENABLE CH%02d"/"DISABLE CH%02d" debug log lines -- see STATUS.md's
 * 2026-08-30 writeup): a single shared register where bit (port_index + 4)
 * gates whether that port's receiver is actually wired into the DMA path,
 * and a 2-bit field at (8 + port_index*2) selects a clock/scaler mode keyed
 * on pixel rate. This driver only ever *captures* from port 0 today (no
 * real input-switching yet), but STATUS.md PICK UP HERE (28): the user
 * confirmed the vendor Windows app keeps all 4 ports enabled/streaming
 * simultaneously at all times (that's how it can flip between live sources
 * instantly) -- every hardware test so far has instead run the FPGA with
 * only port 0's bit (0x10) set, a port-enable configuration the vendor
 * software may never actually exercise. Testing whether that matters to
 * port 0's own DMA reliability (the long-standing segment-tail hole bug):
 * enable bit (port_index + 4) for all 4 ports here, still only capturing
 * port 0. Leaves ports 1-3's clock/scaler mode field (bits 10-15) untouched
 * -- their real signal format/rate is unknown, and this test only cares
 * about the real-data-path enable bit, not correct video out of those ports.
 * The vendor driver's two branches for port 0's own 2-bit field are gated on
 * width*height*fps vs a ~55.3M threshold; every format observed on this
 * board (3840x2160p30) is far over it, so the "high" branch (mode = 1) is
 * hardcoded rather than replicated in full -- revisit if a lower-resolution
 * mode is ever needed.
 * Read-modify-write deliberately: the previous code here (and the probe-time
 * write in sc0710_cards.c) blindly overwrote this register with constants
 * that don't correspond to port 0's actual bit (0x10) -- plausibly the
 * reason channel 0 never received real DMA data despite successful
 * format/timing lock (see the "dormant 0x0000/0x0100 DMA channel" findings
 * earlier in STATUS.md).
 */
static void sc0710_program_channel_enable(struct sc0710_dev *dev, bool enable)
{
	u32 before = sc_read(dev, 0, 0xEC);
	u32 v = before;

	if (enable) {
		v |= (0xf << 4);
		v = (v & ~(3 << 8)) | (1 << 8);
	} else {
		v &= ~(0xf << 4);
	}

	sc_write(dev, 0, 0xEC, v);

	if (sc0710_debug_mode) {
		u32 after = sc_read(dev, 0, 0xEC);

		printk(KERN_INFO "%s: DEBUG 0xEC channel-enable: enable=%d before=%08x wrote=%08x readback=%08x\n",
			dev->name, enable, before, v, after);
	}
}

void sc0710_dma_channels_stop(struct sc0710_dev *dev)
{
	int i, ret;

	printk(KERN_DEBUG "%s()\n", __func__);

	sc_clr(dev, 0, BAR0_00D0, 0x0001);

	if (dev->board == SC0710_BOARD_ELGATEO_4KP)
		sc0710_program_channel_enable(dev, false);

	for (i = 0; i < SC0710_MAX_CHANNELS; i++) {
		if (!dev->channel[i].enabled)
			continue;
		ret = sc0710_dma_channel_stop(&dev->channel[i]);
	}
}

/* Program the FPGA pipeline registers (height, scaler). GO (D0 bit 0) is NOT
 * set here: the vendor driver sets it only after the XDMA engines are running
 * (GO-last on every traced vendor-driver session start), so the caller does it.
 * This is the single authoritative place for the register sequence.
 * Called from both normal stream start and DMA resync paths.
 */
void sc0710_program_pipeline_regs(struct sc0710_dev *dev)
{
	u32 c8 = dev->fmt ? dev->fmt->height : 0x438;
	u32 d0 = dev->pixfmt->pipeline_d0;

	sc_write(dev, 0, BAR0_00C8, c8);

	if (dev->board == SC0710_BOARD_ELGATEO_4KP)
		sc_write(dev, 0, BAR0_00D8, 0x438);

	sc_write(dev, 0, BAR0_00D0, d0);
	sc_write(dev, 0, 0xCC, 0x00000000);
	if (dev->board != SC0710_BOARD_ELGATEO_4KP)
		sc_write(dev, 0, BAR0_00DC, 0x00000000);
	sc_write(dev, 0, BAR0_00D0, 0x4300);
	sc_write(dev, 0, BAR0_00D0, d0);

	if (dev->board == SC0710_BOARD_ELGATEO_4KP) {
		bool active = dev->channel[0].enabled || dev->channel[1].enabled;

		sc0710_program_channel_enable(dev, active);
	}
}

int sc0710_dma_channels_start(struct sc0710_dev *dev)
{
	int i, ret;

	printk(KERN_DEBUG "%s()\n", __func__);

	/* Bail before touching the pipeline if any enabled channel can't start
	 * (ring missing after a failed resize): GO must never be set over a
	 * channel whose descriptor pointer would be 0. */
	for (i = 0; i < SC0710_MAX_CHANNELS; i++) {
		if (!dev->channel[i].enabled)
			continue;
		ret = sc0710_dma_channel_start_prep(&dev->channel[i]);
		if (ret < 0) {
			printk(KERN_ERR "%s: channel %d start prep failed (%d), aborting session start\n",
				dev->name, i, ret);
			return ret;
		}
	}

	sc0710_program_pipeline_regs(dev);

	for (i = 0; i < SC0710_MAX_CHANNELS; i++) {
		if (!dev->channel[i].enabled)
			continue;
		ret = sc0710_dma_channel_start(&dev->channel[i]);
	}

	/* GO last, once the XDMA engines are running, matching the session
	 * template observed in every traced vendor-driver start (stop ->
	 * rebuild -> program -> run engines -> GO) so frames can't flow into a
	 * stopped engine. */
	sc_set(dev, 0, BAR0_00D0, 0x0001);

	if (dev->board == SC0710_BOARD_ELGATEO_4KP)
		sc0710_4kp_wait_pipeline(dev);

	return 0;
}

/* Align the running DMA engines with who actually wants them:
 *  - video: a V4L2 client is streaming (streaming_refcount > 0) and a signal
 *    is present (dev->fmt).
 *  - audio: an ALSA client holds the session (audio_users > 0, only ever taken
 *    while keep_audio_alive is set), or the video session is up. The second
 *    term is what makes the default path identical to the old
 *    channels_start/channels_stop coupling.
 * Either user keeps the shared FPGA GO bit asserted. Caller holds
 * kthread_dma_lock; may sleep (start_prep).
 */
int sc0710_dma_sync_session(struct sc0710_dev *dev)
{
	struct sc0710_dma_channel *vch = NULL;
	struct sc0710_dma_channel *ach = NULL;
	bool want_video = false;
	bool alsa_hold = atomic_read(&dev->audio_users) > 0;
	bool need_video_dma, want_audio;
	bool any_running = false;
	int i, ret;

	for (i = 0; i < SC0710_MAX_CHANNELS; i++) {
		struct sc0710_dma_channel *ch = &dev->channel[i];

		if (!ch->enabled)
			continue;
		if (ch->mediatype == CHTYPE_VIDEO) {
			vch = ch;
			if (atomic_read(&ch->streaming_refcount) > 0)
				want_video = true;
		} else if (ch->mediatype == CHTYPE_AUDIO) {
			ach = ch;
		}
		if (ch->state == STATE_RUNNING)
			any_running = true;
	}

	need_video_dma = want_video && READ_ONCE(dev->fmt) != NULL;
	want_audio = alsa_hold || need_video_dma;

	/* TEMP DEBUG (see STATUS.md PICK UP HERE (14)): a warm-restart
	 * (second stream-start within one module load) got real DMA
	 * completions on ch#0/video where the cold-start (first start
	 * since insmod) got none, with no code difference at all -- trace
	 * which branch below (cold-start GO-together vs already-up
	 * add-individual-channel) each start actually takes, to find the
	 * mechanistic difference instead of guessing from static code. */
	if (sc0710_debug_mode)
		printk(KERN_INFO "%s: DEBUG dma_sync_session: any_running=%d need_video_dma=%d "
			"want_audio=%d alsa_hold=%d vch_state=%d ach_state=%d\n",
			dev->name, any_running, need_video_dma, want_audio, alsa_hold,
			vch ? vch->state : -1, ach ? ach->state : -1);

	if (!need_video_dma && !want_audio) {
		if (any_running)
			sc0710_dma_channels_stop(dev);
		return 0;
	}

	/* Bring video buffers to the current resolution before (re)starting it. */
	if (need_video_dma && vch && vch->state != STATE_RUNNING) {
		ret = sc0710_dma_channels_resize(dev);
		if (ret < 0) {
			printk(KERN_ERR "%s: DMA resize failed during session sync (%d)\n",
				dev->name, ret);
			/* Still try to keep audio alive if that is all we need. */
			if (!alsa_hold)
				return ret;
			need_video_dma = false;
		}
	}

	if (!any_running) {
		/* Cold start: prep only the channels we need, then GO. */
		if (sc0710_debug_mode)
			printk(KERN_INFO "%s: DEBUG dma_sync_session: taking COLD-START branch\n",
				dev->name);
		if (need_video_dma) {
			ret = sc0710_dma_channel_start_prep(vch);
			if (ret < 0)
				return ret;
		}
		if (want_audio && ach) {
			ret = sc0710_dma_channel_start_prep(ach);
			if (ret < 0) {
				if (need_video_dma)
					sc0710_dma_channel_stop(vch);
				return ret;
			}
		}

		sc0710_program_pipeline_regs(dev);

		if (need_video_dma)
			sc0710_dma_channel_start(vch);
		if (want_audio && ach)
			sc0710_dma_channel_start(ach);

		sc_set(dev, 0, BAR0_00D0, 0x0001);
		if (dev->board == SC0710_BOARD_ELGATEO_4KP)
			sc0710_4kp_wait_pipeline(dev);
		return 0;
	}

	/* Session already up: add or drop individual channels. */
	if (sc0710_debug_mode)
		printk(KERN_INFO "%s: DEBUG dma_sync_session: taking ALREADY-UP branch\n",
			dev->name);
	if (need_video_dma && vch && vch->state != STATE_RUNNING) {
		ret = sc0710_dma_channel_start_prep(vch);
		if (ret < 0)
			return ret;
		sc0710_program_pipeline_regs(dev);
		sc0710_dma_channel_start(vch);
		sc_set(dev, 0, BAR0_00D0, 0x0001);
	} else if (!need_video_dma && vch && vch->state == STATE_RUNNING) {
		sc0710_dma_channel_stop(vch);
	}

	if (want_audio && ach && ach->state != STATE_RUNNING) {
		ret = sc0710_dma_channel_start_prep(ach);
		if (ret < 0)
			return ret;
		sc0710_dma_channel_start(ach);
		sc_set(dev, 0, BAR0_00D0, 0x0001);
	} else if (!want_audio && ach && ach->state == STATE_RUNNING) {
		sc0710_dma_channel_stop(ach);
	}

	return 0;
}

/* Check each dma channel. If writeback metadata suggests a transfer
 * has completed, process it and hand the audio/video to linux
 * subsystems. Returns the number of chain completions consumed.
 */
int sc0710_dma_channels_service(struct sc0710_dev *dev)
{
	int i, ret, consumed = 0;

	for (i = 0; i < SC0710_MAX_CHANNELS; i++) {
		if (!dev->channel[i].enabled)
			continue;
		ret = sc0710_dma_channel_service(&dev->channel[i]);
		if (ret > 0)
			consumed += ret;
	}

	return consumed;
}
