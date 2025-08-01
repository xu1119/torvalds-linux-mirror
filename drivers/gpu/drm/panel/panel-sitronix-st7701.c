// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019, Amarula Solutions.
 * Author: Jagan Teki <jagan@amarulasolutions.com>
 */

#include <drm/drm_mipi_dbi.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#include <linux/bitfield.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>

#include <video/mipi_display.h>

/* Command2 BKx selection command */
#define ST7701_CMD2BKX_SEL			0xFF
#define ST7701_CMD1				0
#define ST7701_CMD2				BIT(4)
#define ST7701_CMD2BK_MASK			GENMASK(3, 0)

/* Command2, BK0 commands */
#define ST7701_CMD2_BK0_PVGAMCTRL		0xB0 /* Positive Voltage Gamma Control */
#define ST7701_CMD2_BK0_NVGAMCTRL		0xB1 /* Negative Voltage Gamma Control */
#define ST7701_CMD2_BK0_LNESET			0xC0 /* Display Line setting */
#define ST7701_CMD2_BK0_PORCTRL			0xC1 /* Porch control */
#define ST7701_CMD2_BK0_INVSEL			0xC2 /* Inversion selection, Frame Rate Control */

/* Command2, BK1 commands */
#define ST7701_CMD2_BK1_VRHS			0xB0 /* Vop amplitude setting */
#define ST7701_CMD2_BK1_VCOM			0xB1 /* VCOM amplitude setting */
#define ST7701_CMD2_BK1_VGHSS			0xB2 /* VGH Voltage setting */
#define ST7701_CMD2_BK1_TESTCMD			0xB3 /* TEST Command Setting */
#define ST7701_CMD2_BK1_VGLS			0xB5 /* VGL Voltage setting */
#define ST7701_CMD2_BK1_PWCTLR1			0xB7 /* Power Control 1 */
#define ST7701_CMD2_BK1_PWCTLR2			0xB8 /* Power Control 2 */
#define ST7701_CMD2_BK1_SPD1			0xC1 /* Source pre_drive timing set1 */
#define ST7701_CMD2_BK1_SPD2			0xC2 /* Source EQ2 Setting */
#define ST7701_CMD2_BK1_MIPISET1		0xD0 /* MIPI Setting 1 */

/* Command2, BK0 bytes */
#define ST7701_CMD2_BK0_GAMCTRL_AJ_MASK		GENMASK(7, 6)
#define ST7701_CMD2_BK0_GAMCTRL_VC0_MASK	GENMASK(3, 0)
#define ST7701_CMD2_BK0_GAMCTRL_VC4_MASK	GENMASK(5, 0)
#define ST7701_CMD2_BK0_GAMCTRL_VC8_MASK	GENMASK(5, 0)
#define ST7701_CMD2_BK0_GAMCTRL_VC16_MASK	GENMASK(4, 0)
#define ST7701_CMD2_BK0_GAMCTRL_VC24_MASK	GENMASK(4, 0)
#define ST7701_CMD2_BK0_GAMCTRL_VC52_MASK	GENMASK(3, 0)
#define ST7701_CMD2_BK0_GAMCTRL_VC80_MASK	GENMASK(5, 0)
#define ST7701_CMD2_BK0_GAMCTRL_VC108_MASK	GENMASK(3, 0)
#define ST7701_CMD2_BK0_GAMCTRL_VC147_MASK	GENMASK(3, 0)
#define ST7701_CMD2_BK0_GAMCTRL_VC175_MASK	GENMASK(5, 0)
#define ST7701_CMD2_BK0_GAMCTRL_VC203_MASK	GENMASK(3, 0)
#define ST7701_CMD2_BK0_GAMCTRL_VC231_MASK	GENMASK(4, 0)
#define ST7701_CMD2_BK0_GAMCTRL_VC239_MASK	GENMASK(4, 0)
#define ST7701_CMD2_BK0_GAMCTRL_VC247_MASK	GENMASK(5, 0)
#define ST7701_CMD2_BK0_GAMCTRL_VC251_MASK	GENMASK(5, 0)
#define ST7701_CMD2_BK0_GAMCTRL_VC255_MASK	GENMASK(4, 0)
#define ST7701_CMD2_BK0_LNESET_LINE_MASK	GENMASK(6, 0)
#define ST7701_CMD2_BK0_LNESET_LDE_EN		BIT(7)
#define ST7701_CMD2_BK0_LNESET_LINEDELTA	GENMASK(1, 0)
#define ST7701_CMD2_BK0_PORCTRL_VBP_MASK	GENMASK(7, 0)
#define ST7701_CMD2_BK0_PORCTRL_VFP_MASK	GENMASK(7, 0)
#define ST7701_CMD2_BK0_INVSEL_ONES_MASK	GENMASK(5, 4)
#define ST7701_CMD2_BK0_INVSEL_NLINV_MASK	GENMASK(2, 0)
#define ST7701_CMD2_BK0_INVSEL_RTNI_MASK	GENMASK(4, 0)

/* Command2, BK1 bytes */
#define ST7701_CMD2_BK1_VRHA_MASK		GENMASK(7, 0)
#define ST7701_CMD2_BK1_VCOM_MASK		GENMASK(7, 0)
#define ST7701_CMD2_BK1_VGHSS_MASK		GENMASK(3, 0)
#define ST7701_CMD2_BK1_TESTCMD_VAL		BIT(7)
#define ST7701_CMD2_BK1_VGLS_ONES		BIT(6)
#define ST7701_CMD2_BK1_VGLS_MASK		GENMASK(3, 0)
#define ST7701_CMD2_BK1_PWRCTRL1_AP_MASK	GENMASK(7, 6)
#define ST7701_CMD2_BK1_PWRCTRL1_APIS_MASK	GENMASK(3, 2)
#define ST7701_CMD2_BK1_PWRCTRL1_APOS_MASK	GENMASK(1, 0)
#define ST7701_CMD2_BK1_PWRCTRL2_AVDD_MASK	GENMASK(5, 4)
#define ST7701_CMD2_BK1_PWRCTRL2_AVCL_MASK	GENMASK(1, 0)
#define ST7701_CMD2_BK1_SPD1_ONES_MASK		GENMASK(6, 4)
#define ST7701_CMD2_BK1_SPD1_T2D_MASK		GENMASK(3, 0)
#define ST7701_CMD2_BK1_SPD2_ONES_MASK		GENMASK(6, 4)
#define ST7701_CMD2_BK1_SPD2_T3D_MASK		GENMASK(3, 0)
#define ST7701_CMD2_BK1_MIPISET1_ONES		BIT(7)
#define ST7701_CMD2_BK1_MIPISET1_EOT_EN		BIT(3)

#define CFIELD_PREP(_mask, _val)					\
	(((typeof(_mask))(_val) << (__builtin_ffsll(_mask) - 1)) & (_mask))

enum op_bias {
	OP_BIAS_OFF = 0,
	OP_BIAS_MIN,
	OP_BIAS_MIDDLE,
	OP_BIAS_MAX
};

struct st7701;

struct st7701_panel_desc {
	const struct drm_display_mode *mode;
	unsigned int lanes;
	enum mipi_dsi_pixel_format format;
	unsigned int panel_sleep_delay;

	/* TFT matrix driver configuration, panel specific. */
	const u8	pv_gamma[16];	/* Positive voltage gamma control */
	const u8	nv_gamma[16];	/* Negative voltage gamma control */
	const u8	nlinv;		/* Inversion selection */
	const u32	vop_uv;		/* Vop in uV */
	const u32	vcom_uv;	/* Vcom in uV */
	const u16	vgh_mv;		/* Vgh in mV */
	const s16	vgl_mv;		/* Vgl in mV */
	const u16	avdd_mv;	/* Avdd in mV */
	const s16	avcl_mv;	/* Avcl in mV */
	const enum op_bias	gamma_op_bias;
	const enum op_bias	input_op_bias;
	const enum op_bias	output_op_bias;
	const u16	t2d_ns;		/* T2D in ns */
	const u16	t3d_ns;		/* T3D in ns */
	const bool	eot_en;

	/* GIP sequence, fully custom and undocumented. */
	void		(*gip_sequence)(struct st7701 *st7701);
};

struct st7701 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct mipi_dbi dbi;
	const struct st7701_panel_desc *desc;

	struct regulator_bulk_data supplies[2];
	struct gpio_desc *reset;
	unsigned int sleep_delay;
	enum drm_panel_orientation orientation;

	int (*write_command)(struct st7701 *st7701, u8 cmd, const u8 *seq,
			     size_t len);
};

static inline struct st7701 *panel_to_st7701(struct drm_panel *panel)
{
	return container_of(panel, struct st7701, panel);
}

static int st7701_dsi_write(struct st7701 *st7701, u8 cmd, const u8 *seq,
			    size_t len)
{
	return mipi_dsi_dcs_write(st7701->dsi, cmd, seq, len);
}

static int st7701_dbi_write(struct st7701 *st7701, u8 cmd, const u8 *seq,
			    size_t len)
{
	return mipi_dbi_command_stackbuf(&st7701->dbi, cmd, seq, len);
}

#define ST7701_WRITE(st7701, cmd, seq...)				\
	{								\
		const u8 d[] = { seq };					\
		st7701->write_command(st7701, cmd, d, ARRAY_SIZE(d));	\
	}

static u8 st7701_vgls_map(struct st7701 *st7701)
{
	const struct st7701_panel_desc *desc = st7701->desc;
	struct {
		s32	vgl;
		u8	val;
	} map[16] = {
		{ -7060, 0x0 }, { -7470, 0x1 },
		{ -7910, 0x2 }, { -8140, 0x3 },
		{ -8650, 0x4 }, { -8920, 0x5 },
		{ -9210, 0x6 }, { -9510, 0x7 },
		{ -9830, 0x8 }, { -10170, 0x9 },
		{ -10530, 0xa }, { -10910, 0xb },
		{ -11310, 0xc }, { -11730, 0xd },
		{ -12200, 0xe }, { -12690, 0xf }
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(map); i++)
		if (desc->vgl_mv == map[i].vgl)
			return map[i].val;

	return 0;
}

static void st7701_switch_cmd_bkx(struct st7701 *st7701, bool cmd2, u8 bkx)
{
	u8 val;

	if (cmd2)
		val = ST7701_CMD2 | FIELD_PREP(ST7701_CMD2BK_MASK, bkx);
	else
		val = ST7701_CMD1;

	ST7701_WRITE(st7701, ST7701_CMD2BKX_SEL, 0x77, 0x01, 0x00, 0x00, val);
}

static void st7701_init_sequence(struct st7701 *st7701)
{
	const struct st7701_panel_desc *desc = st7701->desc;
	const struct drm_display_mode *mode = desc->mode;
	const u8 linecount8 = mode->vdisplay / 8;
	const u8 linecountrem2 = (mode->vdisplay % 8) / 2;

	ST7701_WRITE(st7701, MIPI_DCS_SOFT_RESET, 0x00);

	/* We need to wait 5ms before sending new commands */
	msleep(5);

	ST7701_WRITE(st7701, MIPI_DCS_EXIT_SLEEP_MODE, 0x00);

	msleep(st7701->sleep_delay);

	/* Command2, BK0 */
	st7701_switch_cmd_bkx(st7701, true, 0);

	st7701->write_command(st7701, ST7701_CMD2_BK0_PVGAMCTRL, desc->pv_gamma,
			      ARRAY_SIZE(desc->pv_gamma));
	st7701->write_command(st7701, ST7701_CMD2_BK0_NVGAMCTRL, desc->nv_gamma,
			      ARRAY_SIZE(desc->nv_gamma));
	/*
	 * Vertical line count configuration:
	 * Line[6:0]: select number of vertical lines of the TFT matrix in
	 *            multiples of 8 lines
	 * LDE_EN: enable sub-8-line granularity line count
	 * Line_delta[1:0]: add 0/2/4/6 extra lines to line count selected
	 *                  using Line[6:0]
	 *
	 * Total number of vertical lines:
	 * LN = ((Line[6:0] + 1) * 8) + (LDE_EN ? Line_delta[1:0] * 2 : 0)
	 */
	ST7701_WRITE(st7701, ST7701_CMD2_BK0_LNESET,
		   FIELD_PREP(ST7701_CMD2_BK0_LNESET_LINE_MASK, linecount8 - 1) |
		   (linecountrem2 ? ST7701_CMD2_BK0_LNESET_LDE_EN : 0),
		   FIELD_PREP(ST7701_CMD2_BK0_LNESET_LINEDELTA, linecountrem2));
	ST7701_WRITE(st7701, ST7701_CMD2_BK0_PORCTRL,
		   FIELD_PREP(ST7701_CMD2_BK0_PORCTRL_VBP_MASK,
			      mode->vtotal - mode->vsync_end),
		   FIELD_PREP(ST7701_CMD2_BK0_PORCTRL_VFP_MASK,
			      mode->vsync_start - mode->vdisplay));
	/*
	 * Horizontal pixel count configuration:
	 * PCLK = 512 + (RTNI[4:0] * 16)
	 * The PCLK is number of pixel clock per line, which matches
	 * mode htotal. The minimum is 512 PCLK.
	 */
	ST7701_WRITE(st7701, ST7701_CMD2_BK0_INVSEL,
		   ST7701_CMD2_BK0_INVSEL_ONES_MASK |
		   FIELD_PREP(ST7701_CMD2_BK0_INVSEL_NLINV_MASK, desc->nlinv),
		   FIELD_PREP(ST7701_CMD2_BK0_INVSEL_RTNI_MASK,
			      (clamp((u32)mode->htotal, 512U, 1008U) - 512) / 16));

	/* Command2, BK1 */
	st7701_switch_cmd_bkx(st7701, true, 1);

	/* Vop = 3.5375V + (VRHA[7:0] * 0.0125V) */
	ST7701_WRITE(st7701, ST7701_CMD2_BK1_VRHS,
		   FIELD_PREP(ST7701_CMD2_BK1_VRHA_MASK,
			      DIV_ROUND_CLOSEST(desc->vop_uv - 3537500, 12500)));

	/* Vcom = 0.1V + (VCOM[7:0] * 0.0125V) */
	ST7701_WRITE(st7701, ST7701_CMD2_BK1_VCOM,
		   FIELD_PREP(ST7701_CMD2_BK1_VCOM_MASK,
			      DIV_ROUND_CLOSEST(desc->vcom_uv - 100000, 12500)));

	/* Vgh = 11.5V + (VGHSS[7:0] * 0.5V) */
	ST7701_WRITE(st7701, ST7701_CMD2_BK1_VGHSS,
		   FIELD_PREP(ST7701_CMD2_BK1_VGHSS_MASK,
			      DIV_ROUND_CLOSEST(clamp(desc->vgh_mv,
						      (u16)11500,
						      (u16)17000) - 11500,
						500)));

	ST7701_WRITE(st7701, ST7701_CMD2_BK1_TESTCMD, ST7701_CMD2_BK1_TESTCMD_VAL);

	/* Vgl is non-linear */
	ST7701_WRITE(st7701, ST7701_CMD2_BK1_VGLS,
		   ST7701_CMD2_BK1_VGLS_ONES |
		   FIELD_PREP(ST7701_CMD2_BK1_VGLS_MASK, st7701_vgls_map(st7701)));

	ST7701_WRITE(st7701, ST7701_CMD2_BK1_PWCTLR1,
		   FIELD_PREP(ST7701_CMD2_BK1_PWRCTRL1_AP_MASK,
			      desc->gamma_op_bias) |
		   FIELD_PREP(ST7701_CMD2_BK1_PWRCTRL1_APIS_MASK,
			      desc->input_op_bias) |
		   FIELD_PREP(ST7701_CMD2_BK1_PWRCTRL1_APOS_MASK,
			      desc->output_op_bias));

	/* Avdd = 6.2V + (AVDD[1:0] * 0.2V) , Avcl = -4.4V - (AVCL[1:0] * 0.2V) */
	ST7701_WRITE(st7701, ST7701_CMD2_BK1_PWCTLR2,
		   FIELD_PREP(ST7701_CMD2_BK1_PWRCTRL2_AVDD_MASK,
			      DIV_ROUND_CLOSEST(desc->avdd_mv - 6200, 200)) |
		   FIELD_PREP(ST7701_CMD2_BK1_PWRCTRL2_AVCL_MASK,
			      DIV_ROUND_CLOSEST(-4400 - desc->avcl_mv, 200)));

	/* T2D = 0.2us * T2D[3:0] */
	ST7701_WRITE(st7701, ST7701_CMD2_BK1_SPD1,
		   ST7701_CMD2_BK1_SPD1_ONES_MASK |
		   FIELD_PREP(ST7701_CMD2_BK1_SPD1_T2D_MASK,
			      DIV_ROUND_CLOSEST(desc->t2d_ns, 200)));

	/* T3D = 4us + (0.8us * T3D[3:0]) */
	ST7701_WRITE(st7701, ST7701_CMD2_BK1_SPD2,
		   ST7701_CMD2_BK1_SPD2_ONES_MASK |
		   FIELD_PREP(ST7701_CMD2_BK1_SPD2_T3D_MASK,
			      DIV_ROUND_CLOSEST(desc->t3d_ns - 4000, 800)));

	ST7701_WRITE(st7701, ST7701_CMD2_BK1_MIPISET1,
		   ST7701_CMD2_BK1_MIPISET1_ONES |
		   (desc->eot_en ? ST7701_CMD2_BK1_MIPISET1_EOT_EN : 0));
}

static void ts8550b_gip_sequence(struct st7701 *st7701)
{
	/**
	 * ST7701_SPEC_V1.2 is unable to provide enough information above this
	 * specific command sequence, so grab the same from vendor BSP driver.
	 */
	ST7701_WRITE(st7701, 0xE0, 0x00, 0x00, 0x02);
	ST7701_WRITE(st7701, 0xE1, 0x0B, 0x00, 0x0D, 0x00, 0x0C, 0x00, 0x0E,
		   0x00, 0x00, 0x44, 0x44);
	ST7701_WRITE(st7701, 0xE2, 0x33, 0x33, 0x44, 0x44, 0x64, 0x00, 0x66,
		   0x00, 0x65, 0x00, 0x67, 0x00, 0x00);
	ST7701_WRITE(st7701, 0xE3, 0x00, 0x00, 0x33, 0x33);
	ST7701_WRITE(st7701, 0xE4, 0x44, 0x44);
	ST7701_WRITE(st7701, 0xE5, 0x0C, 0x78, 0x3C, 0xA0, 0x0E, 0x78, 0x3C,
		   0xA0, 0x10, 0x78, 0x3C, 0xA0, 0x12, 0x78, 0x3C, 0xA0);
	ST7701_WRITE(st7701, 0xE6, 0x00, 0x00, 0x33, 0x33);
	ST7701_WRITE(st7701, 0xE7, 0x44, 0x44);
	ST7701_WRITE(st7701, 0xE8, 0x0D, 0x78, 0x3C, 0xA0, 0x0F, 0x78, 0x3C,
		   0xA0, 0x11, 0x78, 0x3C, 0xA0, 0x13, 0x78, 0x3C, 0xA0);
	ST7701_WRITE(st7701, 0xEB, 0x02, 0x02, 0x39, 0x39, 0xEE, 0x44, 0x00);
	ST7701_WRITE(st7701, 0xEC, 0x00, 0x00);
	ST7701_WRITE(st7701, 0xED, 0xFF, 0xF1, 0x04, 0x56, 0x72, 0x3F, 0xFF,
		   0xFF, 0xFF, 0xFF, 0xF3, 0x27, 0x65, 0x40, 0x1F, 0xFF);
}

static void dmt028vghmcmi_1a_gip_sequence(struct st7701 *st7701)
{
	ST7701_WRITE(st7701, 0xEE, 0x42);
	ST7701_WRITE(st7701, 0xE0, 0x00, 0x00, 0x02);

	ST7701_WRITE(st7701, 0xE1,
		   0x04, 0xA0, 0x06, 0xA0,
			   0x05, 0xA0, 0x07, 0xA0,
			   0x00, 0x44, 0x44);
	ST7701_WRITE(st7701, 0xE2,
		   0x00, 0x00, 0x00, 0x00,
			   0x00, 0x00, 0x00, 0x00,
			   0x00, 0x00, 0x00, 0x00);
	ST7701_WRITE(st7701, 0xE3,
		   0x00, 0x00, 0x22, 0x22);
	ST7701_WRITE(st7701, 0xE4, 0x44, 0x44);
	ST7701_WRITE(st7701, 0xE5,
		   0x0C, 0x90, 0xA0, 0xA0,
			   0x0E, 0x92, 0xA0, 0xA0,
			   0x08, 0x8C, 0xA0, 0xA0,
			   0x0A, 0x8E, 0xA0, 0xA0);
	ST7701_WRITE(st7701, 0xE6,
		   0x00, 0x00, 0x22, 0x22);
	ST7701_WRITE(st7701, 0xE7, 0x44, 0x44);
	ST7701_WRITE(st7701, 0xE8,
		   0x0D, 0x91, 0xA0, 0xA0,
			   0x0F, 0x93, 0xA0, 0xA0,
			   0x09, 0x8D, 0xA0, 0xA0,
			   0x0B, 0x8F, 0xA0, 0xA0);
	ST7701_WRITE(st7701, 0xEB,
		   0x00, 0x00, 0xE4, 0xE4,
			   0x44, 0x00, 0x00);
	ST7701_WRITE(st7701, 0xED,
		   0xFF, 0xF5, 0x47, 0x6F,
			   0x0B, 0xA1, 0xAB, 0xFF,
			   0xFF, 0xBA, 0x1A, 0xB0,
			   0xF6, 0x74, 0x5F, 0xFF);
	ST7701_WRITE(st7701, 0xEF,
		   0x08, 0x08, 0x08, 0x40,
			   0x3F, 0x64);

	st7701_switch_cmd_bkx(st7701, false, 0);

	st7701_switch_cmd_bkx(st7701, true, 3);
	ST7701_WRITE(st7701, 0xE6, 0x7C);
	ST7701_WRITE(st7701, 0xE8, 0x00, 0x0E);

	st7701_switch_cmd_bkx(st7701, false, 0);
	ST7701_WRITE(st7701, 0x11);
	msleep(120);

	st7701_switch_cmd_bkx(st7701, true, 3);
	ST7701_WRITE(st7701, 0xE8, 0x00, 0x0C);
	msleep(10);
	ST7701_WRITE(st7701, 0xE8, 0x00, 0x00);

	st7701_switch_cmd_bkx(st7701, false, 0);
	ST7701_WRITE(st7701, 0x11);
	msleep(120);
	ST7701_WRITE(st7701, 0xE8, 0x00, 0x00);

	st7701_switch_cmd_bkx(st7701, false, 0);

	ST7701_WRITE(st7701, 0x3A, 0x70);
}

static void kd50t048a_gip_sequence(struct st7701 *st7701)
{
	/**
	 * ST7701_SPEC_V1.2 is unable to provide enough information above this
	 * specific command sequence, so grab the same from vendor BSP driver.
	 */
	ST7701_WRITE(st7701, 0xE0, 0x00, 0x00, 0x02);
	ST7701_WRITE(st7701, 0xE1, 0x08, 0x00, 0x0A, 0x00, 0x07, 0x00, 0x09,
		   0x00, 0x00, 0x33, 0x33);
	ST7701_WRITE(st7701, 0xE2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		   0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	ST7701_WRITE(st7701, 0xE3, 0x00, 0x00, 0x33, 0x33);
	ST7701_WRITE(st7701, 0xE4, 0x44, 0x44);
	ST7701_WRITE(st7701, 0xE5, 0x0E, 0x60, 0xA0, 0xA0, 0x10, 0x60, 0xA0,
		   0xA0, 0x0A, 0x60, 0xA0, 0xA0, 0x0C, 0x60, 0xA0, 0xA0);
	ST7701_WRITE(st7701, 0xE6, 0x00, 0x00, 0x33, 0x33);
	ST7701_WRITE(st7701, 0xE7, 0x44, 0x44);
	ST7701_WRITE(st7701, 0xE8, 0x0D, 0x60, 0xA0, 0xA0, 0x0F, 0x60, 0xA0,
		   0xA0, 0x09, 0x60, 0xA0, 0xA0, 0x0B, 0x60, 0xA0, 0xA0);
	ST7701_WRITE(st7701, 0xEB, 0x02, 0x01, 0xE4, 0xE4, 0x44, 0x00, 0x40);
	ST7701_WRITE(st7701, 0xEC, 0x02, 0x01);
	ST7701_WRITE(st7701, 0xED, 0xAB, 0x89, 0x76, 0x54, 0x01, 0xFF, 0xFF,
		   0xFF, 0xFF, 0xFF, 0xFF, 0x10, 0x45, 0x67, 0x98, 0xBA);
}

static void rg_arc_gip_sequence(struct st7701 *st7701)
{
	st7701_switch_cmd_bkx(st7701, true, 3);
	ST7701_WRITE(st7701, 0xEF, 0x08);
	st7701_switch_cmd_bkx(st7701, true, 0);
	ST7701_WRITE(st7701, 0xC7, 0x04);
	ST7701_WRITE(st7701, 0xCC, 0x38);
	st7701_switch_cmd_bkx(st7701, true, 1);
	ST7701_WRITE(st7701, 0xB9, 0x10);
	ST7701_WRITE(st7701, 0xBC, 0x03);
	ST7701_WRITE(st7701, 0xC0, 0x89);
	ST7701_WRITE(st7701, 0xE0, 0x00, 0x00, 0x02);
	ST7701_WRITE(st7701, 0xE1, 0x04, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00,
		   0x00, 0x00, 0x20, 0x20);
	ST7701_WRITE(st7701, 0xE2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		   0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	ST7701_WRITE(st7701, 0xE3, 0x00, 0x00, 0x33, 0x00);
	ST7701_WRITE(st7701, 0xE4, 0x22, 0x00);
	ST7701_WRITE(st7701, 0xE5, 0x04, 0x5C, 0xA0, 0xA0, 0x06, 0x5C, 0xA0,
		   0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	ST7701_WRITE(st7701, 0xE6, 0x00, 0x00, 0x33, 0x00);
	ST7701_WRITE(st7701, 0xE7, 0x22, 0x00);
	ST7701_WRITE(st7701, 0xE8, 0x05, 0x5C, 0xA0, 0xA0, 0x07, 0x5C, 0xA0,
		   0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	ST7701_WRITE(st7701, 0xEB, 0x02, 0x00, 0x40, 0x40, 0x00, 0x00, 0x00);
	ST7701_WRITE(st7701, 0xEC, 0x00, 0x00);
	ST7701_WRITE(st7701, 0xED, 0xFA, 0x45, 0x0B, 0xFF, 0xFF, 0xFF, 0xFF,
		   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xB0, 0x54, 0xAF);
	ST7701_WRITE(st7701, 0xEF, 0x08, 0x08, 0x08, 0x45, 0x3F, 0x54);
	st7701_switch_cmd_bkx(st7701, false, 0);
	ST7701_WRITE(st7701, MIPI_DCS_SET_ADDRESS_MODE, 0x17);
	ST7701_WRITE(st7701, MIPI_DCS_SET_PIXEL_FORMAT, 0x77);
	ST7701_WRITE(st7701, MIPI_DCS_EXIT_SLEEP_MODE, 0x00);
	msleep(120);
}

static void rg28xx_gip_sequence(struct st7701 *st7701)
{
	st7701_switch_cmd_bkx(st7701, true, 3);
	ST7701_WRITE(st7701, 0xEF, 0x08);

	st7701_switch_cmd_bkx(st7701, true, 0);
	ST7701_WRITE(st7701, 0xC3, 0x02, 0x10, 0x02);
	ST7701_WRITE(st7701, 0xC7, 0x04);
	ST7701_WRITE(st7701, 0xCC, 0x10);

	st7701_switch_cmd_bkx(st7701, true, 1);
	ST7701_WRITE(st7701, 0xEE, 0x42);
	ST7701_WRITE(st7701, 0xE0, 0x00, 0x00, 0x02);

	ST7701_WRITE(st7701, 0xE1, 0x04, 0xA0, 0x06, 0xA0, 0x05, 0xA0, 0x07, 0xA0,
		   0x00, 0x44, 0x44);
	ST7701_WRITE(st7701, 0xE2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		   0x00, 0x00, 0x00, 0x00);
	ST7701_WRITE(st7701, 0xE3, 0x00, 0x00, 0x22, 0x22);
	ST7701_WRITE(st7701, 0xE4, 0x44, 0x44);
	ST7701_WRITE(st7701, 0xE5, 0x0C, 0x90, 0xA0, 0xA0, 0x0E, 0x92, 0xA0, 0xA0,
		   0x08, 0x8C, 0xA0, 0xA0, 0x0A, 0x8E, 0xA0, 0xA0);
	ST7701_WRITE(st7701, 0xE6, 0x00, 0x00, 0x22, 0x22);
	ST7701_WRITE(st7701, 0xE7, 0x44, 0x44);
	ST7701_WRITE(st7701, 0xE8, 0x0D, 0x91, 0xA0, 0xA0, 0x0F, 0x93, 0xA0, 0xA0,
		   0x09, 0x8D, 0xA0, 0xA0, 0x0B, 0x8F, 0xA0, 0xA0);
	ST7701_WRITE(st7701, 0xEB, 0x00, 0x00, 0xE4, 0xE4, 0x44, 0x00, 0x40);
	ST7701_WRITE(st7701, 0xED, 0xFF, 0xF5, 0x47, 0x6F, 0x0B, 0xA1, 0xBA, 0xFF,
		   0xFF, 0xAB, 0x1A, 0xB0, 0xF6, 0x74, 0x5F, 0xFF);
	ST7701_WRITE(st7701, 0xEF, 0x08, 0x08, 0x08, 0x45, 0x3F, 0x54);

	st7701_switch_cmd_bkx(st7701, false, 0);

	st7701_switch_cmd_bkx(st7701, true, 3);
	ST7701_WRITE(st7701, 0xE6, 0x16);
	ST7701_WRITE(st7701, 0xE8, 0x00, 0x0E);

	st7701_switch_cmd_bkx(st7701, false, 0);
	ST7701_WRITE(st7701, MIPI_DCS_SET_ADDRESS_MODE, 0x10);
	ST7701_WRITE(st7701, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(120);

	st7701_switch_cmd_bkx(st7701, true, 3);
	ST7701_WRITE(st7701, 0xE8, 0x00, 0x0C);
	msleep(10);
	ST7701_WRITE(st7701, 0xE8, 0x00, 0x00);
	st7701_switch_cmd_bkx(st7701, false, 0);
}

static void wf40eswaa6mnn0_gip_sequence(struct st7701 *st7701)
{
	ST7701_WRITE(st7701, 0xE0, 0x00, 0x28, 0x02);
	ST7701_WRITE(st7701, 0xE1, 0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00,
		   0x00, 0x00, 0x44, 0x44);
	ST7701_WRITE(st7701, 0xE2, 0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00,
		   0x00, 0xEC, 0xA0, 0x00, 0x00);
	ST7701_WRITE(st7701, 0xE3, 0x00, 0x00, 0x11, 0x11);
	ST7701_WRITE(st7701, 0xE4, 0x44, 0x44);
	ST7701_WRITE(st7701, 0xE5, 0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8,
		   0xA0, 0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0);
	ST7701_WRITE(st7701, 0xE6, 0x00, 0x00, 0x11, 0x11);
	ST7701_WRITE(st7701, 0xE7, 0x44, 0x44);
	ST7701_WRITE(st7701, 0xE8, 0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8,
		   0xA0, 0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0);
	ST7701_WRITE(st7701, 0xEB, 0x00, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40);
	ST7701_WRITE(st7701, 0xEC, 0x3C, 0x00);
	ST7701_WRITE(st7701, 0xED, 0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF,
		   0xFF, 0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA);
	ST7701_WRITE(st7701, MIPI_DCS_SET_ADDRESS_MODE, 0);
}

static int st7701_prepare(struct drm_panel *panel)
{
	struct st7701 *st7701 = panel_to_st7701(panel);
	int ret;

	gpiod_set_value(st7701->reset, 0);

	ret = regulator_bulk_enable(ARRAY_SIZE(st7701->supplies),
				    st7701->supplies);
	if (ret < 0)
		return ret;
	msleep(20);

	gpiod_set_value(st7701->reset, 1);
	msleep(150);

	st7701_init_sequence(st7701);

	if (st7701->desc->gip_sequence)
		st7701->desc->gip_sequence(st7701);

	/* Disable Command2 */
	st7701_switch_cmd_bkx(st7701, false, 0);

	return 0;
}

static int st7701_enable(struct drm_panel *panel)
{
	struct st7701 *st7701 = panel_to_st7701(panel);

	ST7701_WRITE(st7701, MIPI_DCS_SET_DISPLAY_ON, 0x00);

	return 0;
}

static int st7701_disable(struct drm_panel *panel)
{
	struct st7701 *st7701 = panel_to_st7701(panel);

	ST7701_WRITE(st7701, MIPI_DCS_SET_DISPLAY_OFF, 0x00);

	return 0;
}

static int st7701_unprepare(struct drm_panel *panel)
{
	struct st7701 *st7701 = panel_to_st7701(panel);

	ST7701_WRITE(st7701, MIPI_DCS_ENTER_SLEEP_MODE, 0x00);

	msleep(st7701->sleep_delay);

	gpiod_set_value(st7701->reset, 0);

	/**
	 * During the Resetting period, the display will be blanked
	 * (The display is entering blanking sequence, which maximum
	 * time is 120 ms, when Reset Starts in Sleep Out –mode. The
	 * display remains the blank state in Sleep In –mode.) and
	 * then return to Default condition for Hardware Reset.
	 *
	 * So we need wait sleep_delay time to make sure reset completed.
	 */
	msleep(st7701->sleep_delay);

	regulator_bulk_disable(ARRAY_SIZE(st7701->supplies), st7701->supplies);

	return 0;
}

static int st7701_get_modes(struct drm_panel *panel,
			    struct drm_connector *connector)
{
	struct st7701 *st7701 = panel_to_st7701(panel);
	const struct drm_display_mode *desc_mode = st7701->desc->mode;
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, desc_mode);
	if (!mode) {
		dev_err(panel->dev, "failed to add mode %ux%u@%u\n",
			desc_mode->hdisplay, desc_mode->vdisplay,
			drm_mode_vrefresh(desc_mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = desc_mode->width_mm;
	connector->display_info.height_mm = desc_mode->height_mm;

	/*
	 * TODO: Remove once all drm drivers call
	 * drm_connector_set_orientation_from_panel()
	 */
	drm_connector_set_panel_orientation(connector, st7701->orientation);

	return 1;
}

static enum drm_panel_orientation st7701_get_orientation(struct drm_panel *panel)
{
	struct st7701 *st7701 = panel_to_st7701(panel);

	return st7701->orientation;
}

static const struct drm_panel_funcs st7701_funcs = {
	.disable	= st7701_disable,
	.unprepare	= st7701_unprepare,
	.prepare	= st7701_prepare,
	.enable		= st7701_enable,
	.get_modes	= st7701_get_modes,
	.get_orientation = st7701_get_orientation,
};

static const struct drm_display_mode ts8550b_mode = {
	.clock		= 27500,

	.hdisplay	= 480,
	.hsync_start	= 480 + 38,
	.hsync_end	= 480 + 38 + 12,
	.htotal		= 480 + 38 + 12 + 12,

	.vdisplay	= 854,
	.vsync_start	= 854 + 18,
	.vsync_end	= 854 + 18 + 8,
	.vtotal		= 854 + 18 + 8 + 4,

	.width_mm	= 69,
	.height_mm	= 139,

	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static const struct st7701_panel_desc ts8550b_desc = {
	.mode = &ts8550b_mode,
	.lanes = 2,
	.format = MIPI_DSI_FMT_RGB888,
	.panel_sleep_delay = 80, /* panel need extra 80ms for sleep out cmd */

	.pv_gamma = {
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC0_MASK, 0),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC4_MASK, 0xe),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC8_MASK, 0x15),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC16_MASK, 0xf),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC24_MASK, 0x11),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC52_MASK, 0x8),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC80_MASK, 0x8),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC108_MASK, 0x8),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC147_MASK, 0x8),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC175_MASK, 0x23),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC203_MASK, 0x4),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC231_MASK, 0x13),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC239_MASK, 0x12),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC247_MASK, 0x2b),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC251_MASK, 0x34),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC255_MASK, 0x1f)
	},
	.nv_gamma = {
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC0_MASK, 0),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC4_MASK, 0xe),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0x2) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC8_MASK, 0x15),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC16_MASK, 0xf),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC24_MASK, 0x13),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC52_MASK, 0x7),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC80_MASK, 0x9),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC108_MASK, 0x8),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC147_MASK, 0x8),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC175_MASK, 0x22),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC203_MASK, 0x4),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC231_MASK, 0x10),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC239_MASK, 0xe),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC247_MASK, 0x2c),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC251_MASK, 0x34),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC255_MASK, 0x1f)
	},
	.nlinv = 7,
	.vop_uv = 4400000,
	.vcom_uv = 337500,
	.vgh_mv = 15000,
	.vgl_mv = -9510,
	.avdd_mv = 6600,
	.avcl_mv = -4400,
	.gamma_op_bias = OP_BIAS_MAX,
	.input_op_bias = OP_BIAS_MIN,
	.output_op_bias = OP_BIAS_MIN,
	.t2d_ns = 1600,
	.t3d_ns = 10400,
	.eot_en = true,
	.gip_sequence = ts8550b_gip_sequence,
};

static const struct drm_display_mode dmt028vghmcmi_1a_mode = {
	.clock		= 22325,

	.hdisplay	= 480,
	.hsync_start	= 480 + 40,
	.hsync_end	= 480 + 40 + 4,
	.htotal		= 480 + 40 + 4 + 20,

	.vdisplay	= 640,
	.vsync_start	= 640 + 2,
	.vsync_end	= 640 + 2 + 40,
	.vtotal		= 640 + 2 + 40 + 16,

	.width_mm	= 56,
	.height_mm	= 78,

	.flags		= DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,

	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static const struct st7701_panel_desc dmt028vghmcmi_1a_desc = {
	.mode = &dmt028vghmcmi_1a_mode,
	.lanes = 2,
	.format = MIPI_DSI_FMT_RGB888,
	.panel_sleep_delay = 5, /* panel need extra 5ms for sleep out cmd */

	.pv_gamma = {
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC0_MASK, 0),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC4_MASK, 0x10),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC8_MASK, 0x17),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC16_MASK, 0xd),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC24_MASK, 0x11),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC52_MASK, 0x6),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC80_MASK, 0x5),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC108_MASK, 0x8),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC147_MASK, 0x7),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC175_MASK, 0x1f),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC203_MASK, 0x4),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC231_MASK, 0x11),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC239_MASK, 0xe),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC247_MASK, 0x29),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC251_MASK, 0x30),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC255_MASK, 0x1f)
	},
	.nv_gamma = {
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC0_MASK, 0),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC4_MASK, 0xd),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC8_MASK, 0x14),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC16_MASK, 0xe),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC24_MASK, 0x11),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC52_MASK, 0x6),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC80_MASK, 0x4),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC108_MASK, 0x8),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC147_MASK, 0x8),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC175_MASK, 0x20),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC203_MASK, 0x5),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC231_MASK, 0x13),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC239_MASK, 0x13),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC247_MASK, 0x26),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC251_MASK, 0x30),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC255_MASK, 0x1f)
	},
	.nlinv = 1,
	.vop_uv = 4800000,
	.vcom_uv = 1650000,
	.vgh_mv = 15000,
	.vgl_mv = -10170,
	.avdd_mv = 6600,
	.avcl_mv = -4400,
	.gamma_op_bias = OP_BIAS_MIDDLE,
	.input_op_bias = OP_BIAS_MIN,
	.output_op_bias = OP_BIAS_MIN,
	.t2d_ns = 1600,
	.t3d_ns = 10400,
	.eot_en = true,
	.gip_sequence = dmt028vghmcmi_1a_gip_sequence,
};

static const struct drm_display_mode kd50t048a_mode = {
	.clock          = 27500,

	.hdisplay       = 480,
	.hsync_start    = 480 + 2,
	.hsync_end      = 480 + 2 + 10,
	.htotal         = 480 + 2 + 10 + 2,

	.vdisplay       = 854,
	.vsync_start    = 854 + 2,
	.vsync_end      = 854 + 2 + 2,
	.vtotal         = 854 + 2 + 2 + 17,

	.width_mm       = 69,
	.height_mm      = 139,

	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static const struct st7701_panel_desc kd50t048a_desc = {
	.mode = &kd50t048a_mode,
	.lanes = 2,
	.format = MIPI_DSI_FMT_RGB888,
	.panel_sleep_delay = 0,

	.pv_gamma = {
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC0_MASK, 0),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC4_MASK, 0xd),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC8_MASK, 0x14),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC16_MASK, 0xd),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC24_MASK, 0x10),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC52_MASK, 0x5),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC80_MASK, 0x2),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC108_MASK, 0x8),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC147_MASK, 0x8),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC175_MASK, 0x1e),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC203_MASK, 0x5),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC231_MASK, 0x13),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC239_MASK, 0x11),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 2) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC247_MASK, 0x23),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC251_MASK, 0x29),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC255_MASK, 0x18)
	},
	.nv_gamma = {
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC0_MASK, 0),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC4_MASK, 0xc),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC8_MASK, 0x14),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC16_MASK, 0xc),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC24_MASK, 0x10),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC52_MASK, 0x5),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC80_MASK, 0x3),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC108_MASK, 0x8),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC147_MASK, 0x7),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC175_MASK, 0x20),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC203_MASK, 0x5),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC231_MASK, 0x13),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC239_MASK, 0x11),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 2) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC247_MASK, 0x24),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC251_MASK, 0x29),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC255_MASK, 0x18)
	},
	.nlinv = 1,
	.vop_uv = 4887500,
	.vcom_uv = 937500,
	.vgh_mv = 15000,
	.vgl_mv = -9510,
	.avdd_mv = 6600,
	.avcl_mv = -4400,
	.gamma_op_bias = OP_BIAS_MIDDLE,
	.input_op_bias = OP_BIAS_MIN,
	.output_op_bias = OP_BIAS_MIN,
	.t2d_ns = 1600,
	.t3d_ns = 10400,
	.eot_en = true,
	.gip_sequence = kd50t048a_gip_sequence,
};

static const struct drm_display_mode rg_arc_mode = {
	.clock          = 25600,

	.hdisplay	= 480,
	.hsync_start	= 480 + 60,
	.hsync_end	= 480 + 60 + 42,
	.htotal         = 480 + 60 + 42 + 60,

	.vdisplay	= 640,
	.vsync_start	= 640 + 10,
	.vsync_end	= 640 + 10 + 4,
	.vtotal         = 640 + 10 + 4 + 16,

	.width_mm	= 63,
	.height_mm	= 84,

	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static const struct st7701_panel_desc rg_arc_desc = {
	.mode = &rg_arc_mode,
	.lanes = 2,
	.format = MIPI_DSI_FMT_RGB888,
	.panel_sleep_delay = 80,

	.pv_gamma = {
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0x01) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC0_MASK, 0),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC4_MASK, 0x16),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC8_MASK, 0x1d),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC16_MASK, 0x0e),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC24_MASK, 0x12),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC52_MASK, 0x06),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC80_MASK, 0x0c),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC108_MASK, 0x0a),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC147_MASK, 0x09),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC175_MASK, 0x25),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC203_MASK, 0x00),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC231_MASK, 0x03),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC239_MASK, 0x00),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC247_MASK, 0x3f),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC251_MASK, 0x3f),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC255_MASK, 0x1c)
	},
	.nv_gamma = {
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0x01) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC0_MASK, 0),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC4_MASK, 0x16),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC8_MASK, 0x1e),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC16_MASK, 0x0e),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC24_MASK, 0x11),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC52_MASK, 0x06),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC80_MASK, 0x0c),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC108_MASK, 0x08),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC147_MASK, 0x09),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC175_MASK, 0x26),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC203_MASK, 0x00),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC231_MASK, 0x15),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC239_MASK, 0x00),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC247_MASK, 0x3f),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC251_MASK, 0x3f),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC255_MASK, 0x1c)
	},
	.nlinv = 0,
	.vop_uv = 4500000,
	.vcom_uv = 762500,
	.vgh_mv = 15000,
	.vgl_mv = -9510,
	.avdd_mv = 6600,
	.avcl_mv = -4400,
	.gamma_op_bias = OP_BIAS_MIDDLE,
	.input_op_bias = OP_BIAS_MIN,
	.output_op_bias = OP_BIAS_MIN,
	.t2d_ns = 1600,
	.t3d_ns = 10400,
	.eot_en = true,
	.gip_sequence = rg_arc_gip_sequence,
};

static const struct drm_display_mode rg28xx_mode = {
	.clock		= 22325,

	.hdisplay	= 480,
	.hsync_start	= 480 + 40,
	.hsync_end	= 480 + 40 + 4,
	.htotal		= 480 + 40 + 4 + 20,

	.vdisplay	= 640,
	.vsync_start	= 640 + 2,
	.vsync_end	= 640 + 2 + 40,
	.vtotal		= 640 + 2 + 40 + 16,

	.width_mm	= 44,
	.height_mm	= 58,

	.flags		= DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,

	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static const struct st7701_panel_desc rg28xx_desc = {
	.mode = &rg28xx_mode,

	.panel_sleep_delay = 80,

	.pv_gamma = {
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC0_MASK, 0),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC4_MASK, 0x10),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC8_MASK, 0x17),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC16_MASK, 0xd),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC24_MASK, 0x11),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC52_MASK, 0x6),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC80_MASK, 0x5),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC108_MASK, 0x8),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC147_MASK, 0x7),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC175_MASK, 0x1f),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC203_MASK, 0x4),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC231_MASK, 0x11),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC239_MASK, 0xe),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC247_MASK, 0x29),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC251_MASK, 0x30),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC255_MASK, 0x1f)
	},
	.nv_gamma = {
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC0_MASK, 0),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC4_MASK, 0xd),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC8_MASK, 0x14),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC16_MASK, 0xe),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC24_MASK, 0x11),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC52_MASK, 0x6),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC80_MASK, 0x4),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC108_MASK, 0x8),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC147_MASK, 0x8),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC175_MASK, 0x20),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC203_MASK, 0x5),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC231_MASK, 0x13),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC239_MASK, 0x13),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC247_MASK, 0x26),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC251_MASK, 0x30),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC255_MASK, 0x1f)
	},
	.nlinv = 7,
	.vop_uv = 4800000,
	.vcom_uv = 1512500,
	.vgh_mv = 15000,
	.vgl_mv = -11730,
	.avdd_mv = 6600,
	.avcl_mv = -4400,
	.gamma_op_bias = OP_BIAS_MIDDLE,
	.input_op_bias = OP_BIAS_MIN,
	.output_op_bias = OP_BIAS_MIN,
	.t2d_ns = 1600,
	.t3d_ns = 10400,
	.eot_en = true,
	.gip_sequence = rg28xx_gip_sequence,
};

static const struct drm_display_mode wf40eswaa6mnn0_mode = {
	.clock		= 18306,

	.hdisplay	= 480,
	.hsync_start	= 480 + 2,
	.hsync_end	= 480 + 2 + 45,
	.htotal		= 480 + 2 + 45  + 13,

	.vdisplay	= 480,
	.vsync_start	= 480 + 2,
	.vsync_end	= 480 + 2 + 70,
	.vtotal		= 480 + 2 + 70 + 13,

	.width_mm	= 72,
	.height_mm	= 70,

	.flags		= DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,

	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static const struct st7701_panel_desc wf40eswaa6mnn0_desc = {
	.mode = &wf40eswaa6mnn0_mode,
	.lanes = 2,
	.format = MIPI_DSI_FMT_RGB888,
	.panel_sleep_delay = 0,

	.pv_gamma = {
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC0_MASK, 0x1),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC4_MASK, 0x08),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC8_MASK, 0x10),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC16_MASK, 0x0c),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC24_MASK, 0x10),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC52_MASK, 0x08),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC80_MASK, 0x10),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC108_MASK, 0x0c),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC147_MASK, 0x08),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC175_MASK, 0x22),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC203_MASK, 0x04),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC231_MASK, 0x14),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC239_MASK, 0x12),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC247_MASK, 0xb3),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC251_MASK, 0x3a),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC255_MASK, 0x1f)
	},
	.nv_gamma = {
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC4_MASK, 0x13),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC4_MASK, 0x19),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC8_MASK, 0x1f),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC16_MASK, 0x0f),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC24_MASK, 0x14),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC52_MASK, 0x07),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC80_MASK, 0x07),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC108_MASK, 0x08),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC147_MASK, 0x07),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC175_MASK, 0x22),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC203_MASK, 0x02),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC231_MASK, 0xf),

		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC239_MASK, 0x0f),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC247_MASK, 0xa3),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC251_MASK, 0x29),
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_AJ_MASK, 0) |
		CFIELD_PREP(ST7701_CMD2_BK0_GAMCTRL_VC255_MASK, 0x0d)
	},
	.nlinv = 3,
	.vop_uv = 4737500,
	.vcom_uv = 662500,
	.vgh_mv = 15000,
	.vgl_mv = -10170,
	.avdd_mv = 6600,
	.avcl_mv = -4600,
	.gamma_op_bias = OP_BIAS_MIDDLE,
	.input_op_bias = OP_BIAS_MIDDLE,
	.output_op_bias = OP_BIAS_MIN,
	.t2d_ns = 1600,
	.t3d_ns = 10400,
	.eot_en = true,
	.gip_sequence = wf40eswaa6mnn0_gip_sequence,
};

static void st7701_cleanup(void *data)
{
	struct st7701 *st7701 = (struct st7701 *)data;

	drm_panel_remove(&st7701->panel);
	drm_panel_disable(&st7701->panel);
	drm_panel_unprepare(&st7701->panel);
}

static int st7701_probe(struct device *dev, int connector_type)
{
	const struct st7701_panel_desc *desc;
	struct st7701 *st7701;
	int ret;

	st7701 = devm_drm_panel_alloc(dev, struct st7701, panel, &st7701_funcs,
				      connector_type);
	if (IS_ERR(st7701))
		return PTR_ERR(st7701);

	desc = of_device_get_match_data(dev);
	if (!desc)
		return -ENODEV;

	st7701->supplies[0].supply = "VCC";
	st7701->supplies[1].supply = "IOVCC";

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(st7701->supplies),
				      st7701->supplies);
	if (ret < 0)
		return ret;

	st7701->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(st7701->reset)) {
		dev_err(dev, "Couldn't get our reset GPIO\n");
		return PTR_ERR(st7701->reset);
	}

	ret = of_drm_get_panel_orientation(dev->of_node, &st7701->orientation);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get orientation\n");

	st7701->panel.prepare_prev_first = true;

	/**
	 * Once sleep out has been issued, ST7701 IC required to wait 120ms
	 * before initiating new commands.
	 *
	 * On top of that some panels might need an extra delay to wait, so
	 * add panel specific delay for those cases. As now this panel specific
	 * delay information is referenced from those panel BSP driver, example
	 * ts8550b and there is no valid documentation for that.
	 */
	st7701->sleep_delay = 120 + desc->panel_sleep_delay;

	ret = drm_panel_of_backlight(&st7701->panel);
	if (ret)
		return ret;

	drm_panel_add(&st7701->panel);

	dev_set_drvdata(dev, st7701);
	st7701->desc = desc;

	return devm_add_action_or_reset(dev, st7701_cleanup, st7701);
}

static int st7701_dsi_probe(struct mipi_dsi_device *dsi)
{
	struct st7701 *st7701;
	int err;

	err = st7701_probe(&dsi->dev, DRM_MODE_CONNECTOR_DSI);
	if (err)
		return err;

	st7701 = dev_get_drvdata(&dsi->dev);
	st7701->dsi = dsi;
	st7701->write_command = st7701_dsi_write;

	if (!st7701->desc->lanes)
		return dev_err_probe(&dsi->dev, -EINVAL, "This panel is not for MIPI DSI\n");

	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_LPM | MIPI_DSI_CLOCK_NON_CONTINUOUS;
	dsi->format = st7701->desc->format;
	dsi->lanes = st7701->desc->lanes;

	err = mipi_dsi_attach(dsi);
	if (err)
		return dev_err_probe(&dsi->dev, err, "Failed to init MIPI DSI\n");

	return 0;
}

static int st7701_spi_probe(struct spi_device *spi)
{
	struct st7701 *st7701;
	struct gpio_desc *dc;
	int err;

	err = st7701_probe(&spi->dev, DRM_MODE_CONNECTOR_DPI);
	if (err)
		return err;

	st7701 = dev_get_drvdata(&spi->dev);
	st7701->write_command = st7701_dbi_write;

	dc = devm_gpiod_get_optional(&spi->dev, "dc", GPIOD_OUT_LOW);
	if (IS_ERR(dc))
		return dev_err_probe(&spi->dev, PTR_ERR(dc), "Failed to get GPIO for D/CX\n");

	err = mipi_dbi_spi_init(spi, &st7701->dbi, dc);
	if (err)
		return dev_err_probe(&spi->dev, err, "Failed to init MIPI DBI\n");
	st7701->dbi.read_commands = NULL;

	return 0;
}

static void st7701_dsi_remove(struct mipi_dsi_device *dsi)
{
	mipi_dsi_detach(dsi);
}

static const struct of_device_id st7701_dsi_of_match[] = {
	{ .compatible = "anbernic,rg-arc-panel", .data = &rg_arc_desc },
	{ .compatible = "densitron,dmt028vghmcmi-1a", .data = &dmt028vghmcmi_1a_desc },
	{ .compatible = "elida,kd50t048a", .data = &kd50t048a_desc },
	{ .compatible = "techstar,ts8550b", .data = &ts8550b_desc },
	{ .compatible = "winstar,wf40eswaa6mnn0", .data = &wf40eswaa6mnn0_desc },
	{ }
};
MODULE_DEVICE_TABLE(of, st7701_dsi_of_match);

static const struct of_device_id st7701_spi_of_match[] = {
	{ .compatible = "anbernic,rg28xx-panel", .data = &rg28xx_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, st7701_spi_of_match);

static const struct spi_device_id st7701_spi_ids[] = {
	{ "rg28xx-panel" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(spi, st7701_spi_ids);

static struct mipi_dsi_driver st7701_dsi_driver = {
	.probe		= st7701_dsi_probe,
	.remove		= st7701_dsi_remove,
	.driver = {
		.name		= "st7701",
		.of_match_table	= st7701_dsi_of_match,
	},
};

static struct spi_driver st7701_spi_driver = {
	.probe		= st7701_spi_probe,
	.id_table	= st7701_spi_ids,
	.driver = {
		.name		= "st7701",
		.of_match_table	= st7701_spi_of_match,
	},
};

static int __init st7701_driver_init(void)
{
	int err;

	if (IS_ENABLED(CONFIG_SPI)) {
		err = spi_register_driver(&st7701_spi_driver);
		if (err)
			return err;
	}

	if (IS_ENABLED(CONFIG_DRM_MIPI_DSI)) {
		err = mipi_dsi_driver_register(&st7701_dsi_driver);
		if (err) {
			if (IS_ENABLED(CONFIG_SPI))
				spi_unregister_driver(&st7701_spi_driver);
			return err;
		}
	}

	return 0;
}
module_init(st7701_driver_init);

static void __exit st7701_driver_exit(void)
{
	if (IS_ENABLED(CONFIG_DRM_MIPI_DSI))
		mipi_dsi_driver_unregister(&st7701_dsi_driver);

	if (IS_ENABLED(CONFIG_SPI))
		spi_unregister_driver(&st7701_spi_driver);
}
module_exit(st7701_driver_exit);

MODULE_AUTHOR("Jagan Teki <jagan@amarulasolutions.com>");
MODULE_AUTHOR("Hironori KIKUCHI <kikuchan98@gmail.com>");
MODULE_DESCRIPTION("Sitronix ST7701 LCD Panel Driver");
MODULE_LICENSE("GPL");
