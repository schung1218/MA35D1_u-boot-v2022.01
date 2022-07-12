// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2021 Nuvoton Technology Corp.
 *
 */

#include <common.h>
#include <dm.h>
#include <log.h>
#include <video.h>
#include <asm/io.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <clk.h>
#include <reset.h>
#include <dt-bindings/clock/ma35d1-clk.h>
#include <syscon.h>
#include <regmap.h>

DECLARE_GLOBAL_DATA_PTR;

enum {
	/* Maximum LCD size we support */
	LCD_MAX_WIDTH		= 1920,
	LCD_MAX_HEIGHT		= 1080,
};

#define dcregFrameBufferConfig0			0x1518
#define dcregFrameBufferAddress0		0x1400
#define dcregFrameBufferStride0			0x1408
#define dcregHDisplay0				0x1430
#define dcregHSync0				0x1438
#define dcregVDisplay0				0x1440
#define dcregVSync0				0x1448
#define dcregPanelConfig0			0x1418
#define dcregDpiConfig0				0x14B8
#define dcregFrameBufferSize0			0x1810
#define dcregDisplayIntrEnable			0x1480
#define dcregDisplayIntr			0x147C

#define REG_CLK_PLL5CTL0			0xB0
#define REG_CLK_PLL5CTL1			0xB4

#define OUTPUT_DISABLE				(0x0 << 0)
#define OUTPUT_ENABLE				(0x1 << 0)
#define GAMMA_DISABLE				(0x0 << 2)
#define GAMMA_ENABLE				(0x1 << 2)
#define RESET_DISABLE				(0x0 << 4)
#define RESET_ENABLE				(0x1 << 4)
#define YUV_709_BT709				(0x1 << 14)
#define YUV_2020_BT2020				(0x3 << 14)
#define FRAMEBUFFER_FORMAT_POS			(26)

#define DPI_DATA_FORMAT_D24			(0x5)
#define HSYNC_TOTAL_POS				16
#define VSYNC_TOTAL_POS				16
#define HSYNC_PULSE_END_POS			15
#define VSYNC_PULSE_END_POS			15
#define FRAMEBUFFER_HEIGHT_POS			15

#define HSYNC_PULSE_POLARITY_POS		(0x0 << 31)
#define HSYNC_PULSE_POLARITY_NEG		(0x1 << 31)
#define HSYNC_PULSE_DISABLE			(0x0 << 30)
#define HSYNC_PULSE_ENABLE			(0x1 << 30)
#define VSYNC_PULSE_POLARITY_POS		(0x0 << 31)
#define VSYNC_PULSE_POLARITY_NEG		(0x1 << 31)
#define VSYNC_PULSE_DISABLE			(0x0 << 30)
#define VSYNC_PULSE_ENABLE			(0x1 << 30)

#define PANELCONFIG_CLOCK_POLARITY_POS		(0x0 << 9)
#define PANELCONFIG_CLOCK_POLARITY_NEG		(0x1 << 9)
#define PANELCONFIG_CLOCK_DISABLE		(0x0 << 8)
#define PANELCONFIG_CLOCK_ENABLE		(0x1 << 8)
#define PANELCONFIG_DATA_POLARITY_POS		(0x0 << 5)
#define PANELCONFIG_DATA_POLARITY_NEG		(0x1 << 5)
#define PANELCONFIG_DATA_DISABLE		(0x0 << 4)
#define PANELCONFIG_DATA_ENABLE			(0x1 << 4)
#define PANELCONFIG_DE_POLARITY_POS		(0x0 << 1)
#define PANELCONFIG_DE_POLARITY_NEG		(0x1 << 1)
#define PANELCONFIG_DE_DATA_DISABLE		(0x0 << 0)
#define PANELCONFIG_DE_DATA_ENABLE		(0x1 << 0)

struct ma35d1_lcd_info {
	u32 fb_base;
	u32 x_res;
	u32 y_res;
	u32 x_fp;
	u32 y_fp;
	u32 x_bp;
	u32 y_bp;
	u32 hsync_len;
	u32 vsync_len;
	u32 format;
	u32  bpp;
};

struct ma35d1_fb_priv {
	struct reset_ctl rst;
	void __iomem *regs;
	struct display_timing timing;
	struct udevice *panel;
	struct clk dcuclk;
	struct clk pixclock;
};

#define REG_SYS_RLKTZNS 0x1A4
static void CLK_UnLockReg(struct regmap *regmap)
{
	uint ret;

	/* Unlock PLL registers */
	do {
		regmap_write(regmap, REG_SYS_RLKTZNS, 0x59);
		regmap_write(regmap, REG_SYS_RLKTZNS, 0x16);
		regmap_write(regmap, REG_SYS_RLKTZNS, 0x88);
		regmap_read(regmap, REG_SYS_RLKTZNS, &ret);
	}while(ret == 0);
}

static void CLK_LockReg(struct regmap *regmap)
{
	/* Lock PLL registers */
	regmap_write(regmap, REG_SYS_RLKTZNS, 0x0);
}

static ulong ma35d1_fb_cal_pixel_clk_rate(ulong PllSrcClk, ulong rate,
        u32 *u32Reg)
{
	u32 u32M, u32N, u32P;
	u32 u32Tmp, u32Min, u32MinN, u32MinM, u32MinP;

	ulong u64basFreq, u64PllClk;
	ulong u64Con1, u64Con2, u64Con3;

	/* Find best solution */
	u32Min = (u32) - 1;
	u32MinM = 0UL;
	u32MinN = 0UL;
	u32MinP = 0UL;
	u64basFreq = rate;
	if((rate <= 2400000000) && (rate >= 85700000)) {
		for(u32M = 1UL; u32M < 64UL; u32M++) {
			u64Con1 = PllSrcClk/u32M;
			if(!((u64Con1 <= 40000000) && (u64Con1 >= 1000000)))  continue;

			for(u32N = 16UL; u32N < 2048UL; u32N++) {
				u64Con2 = u64Con1 * u32N;
				if(!((u64Con2 <= 2400000000) && (u64Con2 >= 600000000)))  continue;

				for(u32P = 1UL; u32P < 8UL; u32P++) {
					/* Break when get good results */
					if (u32Min == 0UL) {
						break;
					}

					u64Con3 = u64Con2 / u32P;
					if(!((u64Con3 <= 2400000000) && (u64Con3 >= 85700000)))	 continue;

					u32Tmp = (u64Con3 > rate) ? u64Con3 - rate : rate - u64Con3;
					if(u32Tmp < u32Min) {
						u32Min = u32Tmp;
						u32MinM = u32M;
						u32MinN = u32N;
						u32MinP = u32P;

					} else {
						if (u64Con3 < rate)  break;
					}
				}
			}
		}

		/* Enable and apply new PLL setting. */
		u32Reg[0] =	 (u32MinM << 12) | (u32MinN);
		u32Reg[1] =	 (u32MinP << 4);

		/* Actual PLL output clock frequency */
		u64PllClk = (PllSrcClk * u32MinN) / (u32MinP * (u32MinM));
	} else {
		u32Reg[0] = 0x1019;
		u32Reg[1] = 0x30;

		/* Actual PLL output clock frequency */
		u64PllClk = 200000000;
	}

	return u64PllClk;
}

/* Initialize MA35D1 disp registers */
static void ma35d1_disp_register_init(struct ma35d1_lcd_info *disp_info,
                                      void __iomem *regs)
{
	u32 data;
	u32 h_end, h_total, hsync_start, hsync_end;
	u32 v_end, v_total, vsync_start, vsync_end;

	writel(RESET_ENABLE, regs+dcregFrameBufferConfig0);
	udelay(10);
	writel(RESET_DISABLE, regs+dcregFrameBufferConfig0);

	/* Get the timing from var info */
	h_end = disp_info->x_res;
	h_total = disp_info->x_res + disp_info->x_fp + disp_info->x_bp +
	          disp_info->hsync_len;
	hsync_start = disp_info->x_res + disp_info->x_fp;
	hsync_end = hsync_start + disp_info->hsync_len;
	data = (h_total << HSYNC_TOTAL_POS) | h_end;
	writel(data, regs+dcregHDisplay0);

	data = HSYNC_PULSE_POLARITY_POS | HSYNC_PULSE_ENABLE | (hsync_end << HSYNC_PULSE_END_POS) | (hsync_start);
	writel(data, regs+dcregHSync0);

	v_end = disp_info->y_res;
	v_total = disp_info->y_res + disp_info->y_fp + disp_info->y_bp +
	          disp_info->vsync_len;
	vsync_start = disp_info->y_res + disp_info->y_fp;
	vsync_end = vsync_start + disp_info->vsync_len;

	data = (v_total << VSYNC_TOTAL_POS) | v_end;
	writel(data, regs+dcregVDisplay0);

	data = VSYNC_PULSE_POLARITY_POS | VSYNC_PULSE_ENABLE | (vsync_end << VSYNC_PULSE_END_POS) | (vsync_start);
	writel(data, regs+dcregVSync0);

	writel(DPI_DATA_FORMAT_D24, regs+dcregDpiConfig0);

	data = PANELCONFIG_DE_DATA_ENABLE | PANELCONFIG_DE_POLARITY_POS |
		PANELCONFIG_DATA_ENABLE | PANELCONFIG_DATA_POLARITY_POS |
		PANELCONFIG_CLOCK_ENABLE | PANELCONFIG_CLOCK_POLARITY_POS;
	writel(data, regs+dcregPanelConfig0);

	data = (disp_info->y_res << FRAMEBUFFER_HEIGHT_POS) | (disp_info->x_res);
	writel(data, regs+dcregFrameBufferSize0);

	data = disp_info->x_res*(disp_info->bpp >> 3);
	writel(data, regs+dcregFrameBufferStride0);

	data = (disp_info->format << FRAMEBUFFER_FORMAT_POS) | YUV_709_BT709 | RESET_ENABLE | OUTPUT_ENABLE;
	writel(data, regs+dcregFrameBufferConfig0);

	// Set frame buffer address registers
	writel(disp_info->fb_base, regs+dcregFrameBufferAddress0);
}

static int ma35d1_fb_video_probe(struct udevice *dev)
{
	struct video_uc_platdata *plat = dev_get_uclass_platdata(dev);
	struct video_priv *uc_priv = dev_get_uclass_priv(dev);
	struct ma35d1_fb_priv *priv = dev_get_priv(dev);
	struct regmap *regmap, *regmap_sys;
	u32 u32Reg[2] = {0};
	struct ofnode_phandle_args args;
	ofnode node;
	struct display_timing timings;
	struct ma35d1_lcd_info disp_info;
	u32 pix_fmt;
	int ret;

	priv->regs = (void *)dev_read_addr(dev);
	if ((fdt_addr_t)priv->regs == FDT_ADDR_T_NONE) {
		dev_err(dev, "dt register address error\n");
		return -EINVAL;
	}

	node = ofnode_by_compatible(ofnode_null(), "nuvoton,ma35d1-fb");
	if (!ofnode_valid(node)) {
		dev_err(dev, "missing 'nuvoton,ma35d1-fb' node\n");
		return -ENXIO;
	}

	ret = ofnode_decode_display_timing(dev_ofnode(dev), 0, &timings);
	if (ret) {
		dev_err(dev, "failed to get any display timings\n");
		return -EINVAL;
	}

	/* Use DT timing (resolution) in internal info struct */
	disp_info.fb_base = plat->base;
	disp_info.x_res = timings.hactive.typ;
	disp_info.x_fp = timings.hfront_porch.typ;
	disp_info.x_bp = timings.hback_porch.typ;
	disp_info.y_res = timings.vactive.typ;
	disp_info.y_fp = timings.vfront_porch.typ;
	disp_info.y_bp = timings.vback_porch.typ;
	disp_info.hsync_len = timings.hsync_len.typ;
	disp_info.vsync_len = timings.vsync_len.typ;
	priv->pixclock.rate = timings.pixelclock.typ;

	if(disp_info.x_res > LCD_MAX_WIDTH)
		disp_info.x_res = LCD_MAX_WIDTH;
	if(disp_info.y_res > LCD_MAX_HEIGHT)
		disp_info.y_res = LCD_MAX_HEIGHT;

	ret = ofnode_read_u32(node, "pixel-fmt", &pix_fmt);
	if (ret) {
		dev_err(dev, "Failed to read bpp, pixel_fmt and buswidth from DT\n");
		return ret;
	}

	disp_info.format = pix_fmt;
	if (pix_fmt == 4) { // R5G6B5
		uc_priv->bpix = VIDEO_BPP16;
		disp_info.bpp = 16;
	} else if (pix_fmt == 6) { // A8R8G8B8
		uc_priv->bpix = VIDEO_BPP32;
		disp_info.bpp = 32;
	} else if (pix_fmt == 8) { // UYVY
		uc_priv->bpix = VIDEO_BPP16;
		disp_info.bpp = 16;
	} else {
		printf("%s: invalid format: %d\n", __func__, pix_fmt);
		return -EINVAL;
	}

	uc_priv->xsize = disp_info.x_res;
	uc_priv->ysize = disp_info.y_res;
	uc_priv->rot = 0;
	plat->size = uc_priv->xsize*uc_priv->ysize*(disp_info.bpp >> 3);

	/* enable Display Controller Ultra Core Clock Source */
	ret = clk_get_by_index(dev, 0, &priv->dcuclk);
	if (ret)
		return ret;

	ret = clk_enable(&priv->dcuclk);
	if (ret)
		return ret;

	/* Get CLK register */
	ret = dev_read_phandle_with_args(dev, "nuvoton,clk", NULL,
	                                 0, 0, &args);
	if (ret) {
		dev_err(dev, "Failed to get syscon: %d\n", ret);
		return ret;
	}

	regmap = syscon_node_to_regmap(args.node);
	if (IS_ERR(regmap)) {
		ret = PTR_ERR(regmap);
		dev_err(dev, "can't get syscon: %d\n", ret);
		return ret;
	}

	/* Calculate Pixel Clock, DCUP = VPLL/2 */
	priv->pixclock.rate *= 2;
	priv->pixclock.rate = ma35d1_fb_cal_pixel_clk_rate(24000000,
	                      priv->pixclock.rate, u32Reg);

	regmap_sys = syscon_regmap_lookup_by_phandle(dev,"nuvoton,sys");
	CLK_UnLockReg(regmap_sys);
	/* Set Pixel Clock */
	regmap_write(regmap, REG_CLK_PLL5CTL0, u32Reg[0]);
	regmap_write(regmap, REG_CLK_PLL5CTL1, u32Reg[1]);
	CLK_LockReg(regmap_sys);

	/* Reset */
	ret = reset_get_by_index(dev, 0, &priv->rst);
	if (ret) {
		dev_err(dev, "missing ma35d1_disp hardware reset\n");
		return ret;
	}

	reset_assert(&priv->rst);
	udelay(100);
	reset_deassert(&priv->rst);


	/* Initialize the LCD controller */
	ma35d1_disp_register_init(&disp_info, priv->regs);

	video_set_flush_dcache(dev, true);

	return 0;
}

static int ma35d1_fb_video_bind(struct udevice *dev)
{
	struct video_uc_platdata *uc_plat = dev_get_uclass_platdata(dev);

	uc_plat->size = LCD_MAX_WIDTH * LCD_MAX_HEIGHT * 4;
	debug("\n%s: frame buffer max size %d bytes\n", __func__, uc_plat->size);

	return 0;
}

static const struct udevice_id ma35d1_fb_ids[] = {
	{.compatible = "nuvoton,ma35d1-fb"},
	{ }
};

U_BOOT_DRIVER(ma35d1_fb) = {
	.name	= "ma35d1_fb",
	.id	= UCLASS_VIDEO,
	.of_match = ma35d1_fb_ids,
	.probe	= ma35d1_fb_video_probe,
	.bind	= ma35d1_fb_video_bind,
	.priv_auto_alloc_size	= sizeof(struct ma35d1_fb_priv),
	.flags	= DM_FLAG_PRE_RELOC,
};
