/* The GX calls Starlet's gx_fifo leaves to the port (its gx_fifo.md): those that write through
 * the decompilation's own __GXData, as GXMisc.c has them, and the performance counters, which
 * Starlet's GPU does not model. */
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <stdio.h>

#include "__gx.h"

/* starlet::gx_fifo's GpuSetup::flush_dirty_state: GXFlush's work on the game's own GX state. */
void sunshine_gx_flush_dirty_state(void)
{
	if (gx->dirtyState) {
		__GXSetDirtyState();
	}
}

void GXSetMisc(GXMiscToken token, u32 val)
{
	switch (token) {
	case GX_MT_XF_FLUSH:
		gx->unk04  = val;
		gx->vNum   = !gx->unk04;
		gx->bpSent = 1;
		if (gx->unk04) {
			gx->dirtyState |= 8;
		}
		break;
	case GX_MT_DL_SAVE_CONTEXT:
		gx->dlSaveContext = (val > 0);
		break;
	default:
		break;
	}
}

void GXSetDrawSync(u16 token)
{
	BOOL enabled;
	u32 reg;

	enabled = OSDisableInterrupts();
	reg     = token | 0x48000000;
	GX_WRITE_BP_REG(reg);
	SET_REG_FIELD(0x16F, reg, 16, 0, token);
	SET_REG_FIELD(0x170, reg, 8, 24, 0x47);
	GX_WRITE_BP_REG(reg);
	GXFlush();
	OSRestoreInterrupts(enabled);
	gx->bpSent = 0;
}

void GXPixModeSync(void)
{
	GX_WRITE_BP_REG(gx->peCtrl);
	gx->bpSent = 0;
}

void GXTexModeSync(void)
{
	GX_WRITE_BP_REG(0x63000000);
	gx->bpSent = 0;
}

/* The pixel engine's counters. The game reads one of them: the goop counter
 * (TPollutionCounterBase) counts the pixels its stamps cover. Starlet does not count pixels yet,
 * so the counter reads zero and the game sees no goop left in those layers. */
void GXSetGPMetric(GXPerf0 perf0, GXPerf1 perf1) { (void)perf0; (void)perf1; }
void GXClearGPMetric(void) {}
void GXClearPixMetric(void) {}
void GXReadPixMetric(u32* top_pixels_in, u32* top_pixels_out, u32* bot_pixels_in, u32* bot_pixels_out,
                     u32* clr_pixels_in, u32* copy_clocks)
{
	static int reported;
	if (!reported) {
		reported = 1;
		printf("gx: pixel counters are not modelled; the goop counter reads 0\n");
	}
	*top_pixels_in = *top_pixels_out = *bot_pixels_in = *bot_pixels_out = *clr_pixels_in = *copy_clocks = 0;
}
