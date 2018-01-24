/*
 * linux/drivers/video/omap2/dss/dpi.c
 *
 * Copyright (C) 2009 Nokia Corporation
 * Author: Tomi Valkeinen <tomi.valkeinen@nokia.com>
 *
 * Some code and ideas taken from drivers/video/omap/ driver
 * by Imre Deak.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define DSS_SUBSYS_NAME "DPI"

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/export.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/string.h>
#include <linux/of.h>
#include <linux/clk.h>

#include "omapdss.h"
#include "dss.h"
#include "dss_features.h"

struct dpi_data {
	struct platform_device *pdev;

	struct regulator *vdds_dsi_reg;
	enum dss_clk_source clk_src;
	struct dss_pll *pll;

	struct mutex lock;

	struct videomode vm;
	struct dss_lcd_mgr_config mgr_config;
	int data_lines;

	struct omap_dss_device output;

	bool port_initialized;
};

static struct dpi_data *dpi_get_data_from_dssdev(struct omap_dss_device *dssdev)
{
	return container_of(dssdev, struct dpi_data, output);
}

static enum dss_clk_source dpi_get_clk_src_dra7xx(enum omap_channel channel)
{
	/*
	 * Possible clock sources:
	 * LCD1: FCK/PLL1_1/HDMI_PLL
	 * LCD2: FCK/PLL1_3/HDMI_PLL (DRA74x: PLL2_3)
	 * LCD3: FCK/PLL1_3/HDMI_PLL (DRA74x: PLL2_1)
	 */

	switch (channel) {
	case OMAP_DSS_CHANNEL_LCD:
	{
		if (dss_pll_find_by_src(DSS_CLK_SRC_PLL1_1))
			return DSS_CLK_SRC_PLL1_1;
		break;
	}
	case OMAP_DSS_CHANNEL_LCD2:
	{
		if (dss_pll_find_by_src(DSS_CLK_SRC_PLL1_3))
			return DSS_CLK_SRC_PLL1_3;
		if (dss_pll_find_by_src(DSS_CLK_SRC_PLL2_3))
			return DSS_CLK_SRC_PLL2_3;
		break;
	}
	case OMAP_DSS_CHANNEL_LCD3:
	{
		if (dss_pll_find_by_src(DSS_CLK_SRC_PLL2_1))
			return DSS_CLK_SRC_PLL2_1;
		if (dss_pll_find_by_src(DSS_CLK_SRC_PLL1_3))
			return DSS_CLK_SRC_PLL1_3;
		break;
	}
	default:
		break;
	}

	return DSS_CLK_SRC_FCK;
}

static enum dss_clk_source dpi_get_clk_src(enum omap_channel channel)
{
	/*
	 * XXX we can't currently use DSI PLL for DPI with OMAP3, as the DSI PLL
	 * would also be used for DISPC fclk. Meaning, when the DPI output is
	 * disabled, DISPC clock will be disabled, and TV out will stop.
	 */
	switch (omapdss_get_version()) {
	case OMAPDSS_VER_OMAP24xx:
	case OMAPDSS_VER_OMAP34xx_ES1:
	case OMAPDSS_VER_OMAP34xx_ES3:
	case OMAPDSS_VER_OMAP3630:
	case OMAPDSS_VER_AM35xx:
	case OMAPDSS_VER_AM43xx:
		return DSS_CLK_SRC_FCK;

	case OMAPDSS_VER_OMAP4430_ES1:
	case OMAPDSS_VER_OMAP4430_ES2:
	case OMAPDSS_VER_OMAP4:
		switch (channel) {
		case OMAP_DSS_CHANNEL_LCD:
			return DSS_CLK_SRC_PLL1_1;
		case OMAP_DSS_CHANNEL_LCD2:
			return DSS_CLK_SRC_PLL2_1;
		default:
			return DSS_CLK_SRC_FCK;
		}

	case OMAPDSS_VER_OMAP5:
		switch (channel) {
		case OMAP_DSS_CHANNEL_LCD:
			return DSS_CLK_SRC_PLL1_1;
		case OMAP_DSS_CHANNEL_LCD3:
			return DSS_CLK_SRC_PLL2_1;
		case OMAP_DSS_CHANNEL_LCD2:
		default:
			return DSS_CLK_SRC_FCK;
		}

	case OMAPDSS_VER_DRA7xx:
		return dpi_get_clk_src_dra7xx(channel);

	default:
		return DSS_CLK_SRC_FCK;
	}
}

struct dpi_clk_calc_ctx {
	struct dss_pll *pll;
	unsigned clkout_idx;

	/* inputs */

	unsigned long pck_min, pck_max;

	/* outputs */

	struct dss_pll_clock_info pll_cinfo;
	unsigned long fck;
	struct dispc_clock_info dispc_cinfo;
};

static bool dpi_calc_dispc_cb(int lckd, int pckd, unsigned long lck,
		unsigned long pck, void *data)
{
	struct dpi_clk_calc_ctx *ctx = data;

	/*
	 * Odd dividers give us uneven duty cycle, causing problem when level
	 * shifted. So skip all odd dividers when the pixel clock is on the
	 * higher side.
	 */
	if (ctx->pck_min >= 100000000) {
		if (lckd > 1 && lckd % 2 != 0)
			return false;

		if (pckd > 1 && pckd % 2 != 0)
			return false;
	}

	ctx->dispc_cinfo.lck_div = lckd;
	ctx->dispc_cinfo.pck_div = pckd;
	ctx->dispc_cinfo.lck = lck;
	ctx->dispc_cinfo.pck = pck;

	return true;
}


static bool dpi_calc_hsdiv_cb(int m_dispc, unsigned long dispc,
		void *data)
{
	struct dpi_clk_calc_ctx *ctx = data;

	ctx->pll_cinfo.mX[ctx->clkout_idx] = m_dispc;
	ctx->pll_cinfo.clkout[ctx->clkout_idx] = dispc;

	return dispc_div_calc(dispc, ctx->pck_min, ctx->pck_max,
			dpi_calc_dispc_cb, ctx);
}


static bool dpi_calc_pll_cb(int n, int m, unsigned long fint,
		unsigned long clkdco,
		void *data)
{
	struct dpi_clk_calc_ctx *ctx = data;

	ctx->pll_cinfo.n = n;
	ctx->pll_cinfo.m = m;
	ctx->pll_cinfo.fint = fint;
	ctx->pll_cinfo.clkdco = clkdco;

	return dss_pll_hsdiv_calc_a(ctx->pll, clkdco,
		ctx->pck_min, dss_feat_get_param_max(FEAT_PARAM_DSS_FCK),
		dpi_calc_hsdiv_cb, ctx);
}

static bool dpi_calc_dss_cb(unsigned long fck, void *data)
{
	struct dpi_clk_calc_ctx *ctx = data;

	ctx->fck = fck;

	return dispc_div_calc(fck, ctx->pck_min, ctx->pck_max,
			dpi_calc_dispc_cb, ctx);
}

static bool dpi_pll_clk_calc(struct dpi_data *dpi, unsigned long pck,
		struct dpi_clk_calc_ctx *ctx)
{
	unsigned long clkin;

	memset(ctx, 0, sizeof(*ctx));
	ctx->pll = dpi->pll;
	ctx->clkout_idx = dss_pll_get_clkout_idx_for_src(dpi->clk_src);

	clkin = clk_get_rate(dpi->pll->clkin);

	if (dpi->pll->hw->type == DSS_PLL_TYPE_A) {
		unsigned long pll_min, pll_max;

		ctx->pck_min = pck - 1000;
		ctx->pck_max = pck + 1000;

		pll_min = 0;
		pll_max = 0;

		return dss_pll_calc_a(ctx->pll, clkin,
				pll_min, pll_max,
				dpi_calc_pll_cb, ctx);
	} else { /* DSS_PLL_TYPE_B */
		dss_pll_calc_b(dpi->pll, clkin, pck, &ctx->pll_cinfo);

		ctx->dispc_cinfo.lck_div = 1;
		ctx->dispc_cinfo.pck_div = 1;
		ctx->dispc_cinfo.lck = ctx->pll_cinfo.clkout[0];
		ctx->dispc_cinfo.pck = ctx->dispc_cinfo.lck;

		return true;
	}
}

static bool dpi_dss_clk_calc(unsigned long pck, struct dpi_clk_calc_ctx *ctx)
{
	int i;

	/*
	 * DSS fck gives us very few possibilities, so finding a good pixel
	 * clock may not be possible. We try multiple times to find the clock,
	 * each time widening the pixel clock range we look for, up to
	 * +/- ~15MHz.
	 */

	for (i = 0; i < 25; ++i) {
		bool ok;

		memset(ctx, 0, sizeof(*ctx));
		if (pck > 1000 * i * i * i)
			ctx->pck_min = max(pck - 1000 * i * i * i, 0lu);
		else
			ctx->pck_min = 0;
		ctx->pck_max = pck + 1000 * i * i * i;

		ok = dss_div_calc(pck, ctx->pck_min, dpi_calc_dss_cb, ctx);
		if (ok)
			return ok;
	}

	return false;
}



static int dpi_set_pll_clk(struct dpi_data *dpi, enum omap_channel channel,
		unsigned long pck_req, unsigned long *fck, int *lck_div,
		int *pck_div)
{
	struct dpi_clk_calc_ctx ctx;
	int r;
	bool ok;

	ok = dpi_pll_clk_calc(dpi, pck_req, &ctx);
	if (!ok)
		return -EINVAL;

	r = dss_pll_set_config(dpi->pll, &ctx.pll_cinfo);
	if (r)
		return r;

	dss_select_lcd_clk_source(channel, dpi->clk_src);

	dpi->mgr_config.clock_info = ctx.dispc_cinfo;

	*fck = ctx.pll_cinfo.clkout[ctx.clkout_idx];
	*lck_div = ctx.dispc_cinfo.lck_div;
	*pck_div = ctx.dispc_cinfo.pck_div;

	return 0;
}

static int dpi_set_dispc_clk(struct dpi_data *dpi, unsigned long pck_req,
		unsigned long *fck, int *lck_div, int *pck_div)
{
	struct dpi_clk_calc_ctx ctx;
	int r;
	bool ok;

	ok = dpi_dss_clk_calc(pck_req, &ctx);
	if (!ok)
		return -EINVAL;

	r = dss_set_fck_rate(ctx.fck);
	if (r)
		return r;

	dpi->mgr_config.clock_info = ctx.dispc_cinfo;

	*fck = ctx.fck;
	*lck_div = ctx.dispc_cinfo.lck_div;
	*pck_div = ctx.dispc_cinfo.pck_div;

	return 0;
}

static int dpi_set_mode(struct dpi_data *dpi)
{
	struct omap_dss_device *out = &dpi->output;
	enum omap_channel channel = out->dispc_channel;
	struct videomode *vm = &dpi->vm;
	int lck_div = 0, pck_div = 0;
	unsigned long fck = 0;
	unsigned long pck;
	int r = 0;

	if (dpi->pll)
		r = dpi_set_pll_clk(dpi, channel, vm->pixelclock, &fck,
				&lck_div, &pck_div);
	else
		r = dpi_set_dispc_clk(dpi, vm->pixelclock, &fck,
				&lck_div, &pck_div);
	if (r)
		return r;

	pck = fck / lck_div / pck_div;

	if (pck != vm->pixelclock) {
		DSSWARN("Could not find exact pixel clock. Requested %lu Hz, got %lu Hz\n",
			vm->pixelclock, pck);

		vm->pixelclock = pck;
	}

	dss_mgr_set_timings(channel, vm);

	return 0;
}

static void dpi_config_lcd_manager(struct dpi_data *dpi)
{
	struct omap_dss_device *out = &dpi->output;
	enum omap_channel channel = out->dispc_channel;

	dpi->mgr_config.io_pad_mode = DSS_IO_PAD_MODE_BYPASS;

	dpi->mgr_config.stallmode = false;
	dpi->mgr_config.fifohandcheck = false;

	dpi->mgr_config.video_port_width = dpi->data_lines;

	dpi->mgr_config.lcden_sig_polarity = 0;

	dss_mgr_set_lcd_config(channel, &dpi->mgr_config);
}

static int dpi_display_enable(struct omap_dss_device *dssdev)
{
	struct dpi_data *dpi = dpi_get_data_from_dssdev(dssdev);
	struct omap_dss_device *out = &dpi->output;
	enum omap_channel channel = out->dispc_channel;
	int r;

	mutex_lock(&dpi->lock);

	if (dss_has_feature(FEAT_DPI_USES_VDDS_DSI) && !dpi->vdds_dsi_reg) {
		DSSERR("no VDSS_DSI regulator\n");
		r = -ENODEV;
		goto err_no_reg;
	}

	if (!out->dispc_channel_connected) {
		DSSERR("failed to enable display: no output/manager\n");
		r = -ENODEV;
		goto err_no_out_mgr;
	}

	if (dss_has_feature(FEAT_DPI_USES_VDDS_DSI)) {
		r = regulator_enable(dpi->vdds_dsi_reg);
		if (r)
			goto err_reg_enable;
	}

	r = dispc_runtime_get();
	if (r)
		goto err_get_dispc;

	r = dss_dpi_select_source(out->port_num, channel);
	if (r)
		goto err_src_sel;

	if (dpi->pll) {
		r = dss_pll_enable(dpi->pll);
		if (r)
			goto err_pll_init;
	}

	r = dpi_set_mode(dpi);
	if (r)
		goto err_set_mode;

	dpi_config_lcd_manager(dpi);

	mdelay(2);

	r = dss_mgr_enable(channel);
	if (r)
		goto err_mgr_enable;

	mutex_unlock(&dpi->lock);

	return 0;

err_mgr_enable:
err_set_mode:
	if (dpi->pll)
		dss_pll_disable(dpi->pll);
err_pll_init:
err_src_sel:
	dispc_runtime_put();
err_get_dispc:
	if (dss_has_feature(FEAT_DPI_USES_VDDS_DSI))
		regulator_disable(dpi->vdds_dsi_reg);
err_reg_enable:
err_no_out_mgr:
err_no_reg:
	mutex_unlock(&dpi->lock);
	return r;
}

static void dpi_display_disable(struct omap_dss_device *dssdev)
{
	struct dpi_data *dpi = dpi_get_data_from_dssdev(dssdev);
	enum omap_channel channel = dpi->output.dispc_channel;

	mutex_lock(&dpi->lock);

	dss_mgr_disable(channel);

	if (dpi->pll) {
		dss_select_lcd_clk_source(channel, DSS_CLK_SRC_FCK);
		dss_pll_disable(dpi->pll);
	}

	dispc_runtime_put();

	if (dss_has_feature(FEAT_DPI_USES_VDDS_DSI))
		regulator_disable(dpi->vdds_dsi_reg);

	mutex_unlock(&dpi->lock);
}

static void dpi_set_timings(struct omap_dss_device *dssdev,
			    struct videomode *vm)
{
	struct dpi_data *dpi = dpi_get_data_from_dssdev(dssdev);

	DSSDBG("dpi_set_timings\n");

	mutex_lock(&dpi->lock);

	dpi->vm = *vm;

	mutex_unlock(&dpi->lock);
}

static void dpi_get_timings(struct omap_dss_device *dssdev,
			    struct videomode *vm)
{
	struct dpi_data *dpi = dpi_get_data_from_dssdev(dssdev);

	mutex_lock(&dpi->lock);

	*vm = dpi->vm;

	mutex_unlock(&dpi->lock);
}

static int dpi_check_timings(struct omap_dss_device *dssdev,
			     struct videomode *vm)
{
	struct dpi_data *dpi = dpi_get_data_from_dssdev(dssdev);
	enum omap_channel channel = dpi->output.dispc_channel;
	int lck_div, pck_div;
	unsigned long fck;
	unsigned long pck;
	struct dpi_clk_calc_ctx ctx;
	bool ok;

	if (vm->hactive % 8 != 0)
		return -EINVAL;

	if (!dispc_mgr_timings_ok(channel, vm))
		return -EINVAL;

	if (vm->pixelclock == 0)
		return -EINVAL;

	if (dpi->pll) {
		ok = dpi_pll_clk_calc(dpi, vm->pixelclock, &ctx);
		if (!ok)
			return -EINVAL;

		fck = ctx.pll_cinfo.clkout[ctx.clkout_idx];
	} else {
		ok = dpi_dss_clk_calc(vm->pixelclock, &ctx);
		if (!ok)
			return -EINVAL;

		fck = ctx.fck;
	}

	lck_div = ctx.dispc_cinfo.lck_div;
	pck_div = ctx.dispc_cinfo.pck_div;

	pck = fck / lck_div / pck_div;

	vm->pixelclock = pck;

	return 0;
}

static int dpi_verify_pll(struct dss_pll *pll)
{
	int r;

	/* do initial setup with the PLL to see if it is operational */

	r = dss_pll_enable(pll);
	if (r)
		return r;

	dss_pll_disable(pll);

	return 0;
}

static int dpi_init_regulator(struct dpi_data *dpi)
{
	struct regulator *vdds_dsi;

	if (!dss_has_feature(FEAT_DPI_USES_VDDS_DSI))
		return 0;

	if (dpi->vdds_dsi_reg)
		return 0;

	vdds_dsi = devm_regulator_get(&dpi->pdev->dev, "vdds_dsi");
	if (IS_ERR(vdds_dsi)) {
		if (PTR_ERR(vdds_dsi) != -EPROBE_DEFER)
			DSSERR("can't get VDDS_DSI regulator\n");
		return PTR_ERR(vdds_dsi);
	}

	dpi->vdds_dsi_reg = vdds_dsi;

	return 0;
}

static void dpi_init_pll(struct dpi_data *dpi)
{
	struct dss_pll *pll;

	if (dpi->pll)
		return;

	dpi->clk_src = dpi_get_clk_src(dpi->output.dispc_channel);

	pll = dss_pll_find_by_src(dpi->clk_src);
	if (!pll)
		return;

	if (dpi_verify_pll(pll)) {
		DSSWARN("PLL not operational\n");
		return;
	}

	dpi->pll = pll;
}

/*
 * Return a hardcoded channel for the DPI output. This should work for
 * current use cases, but this can be later expanded to either resolve
 * the channel in some more dynamic manner, or get the channel as a user
 * parameter.
 */
static enum omap_channel dpi_get_channel(int port_num)
{
	switch (omapdss_get_version()) {
	case OMAPDSS_VER_OMAP24xx:
	case OMAPDSS_VER_OMAP34xx_ES1:
	case OMAPDSS_VER_OMAP34xx_ES3:
	case OMAPDSS_VER_OMAP3630:
	case OMAPDSS_VER_AM35xx:
	case OMAPDSS_VER_AM43xx:
		return OMAP_DSS_CHANNEL_LCD;

	case OMAPDSS_VER_DRA7xx:
		switch (port_num) {
		case 2:
			return OMAP_DSS_CHANNEL_LCD3;
		case 1:
			return OMAP_DSS_CHANNEL_LCD2;
		case 0:
		default:
			return OMAP_DSS_CHANNEL_LCD;
		}

	case OMAPDSS_VER_OMAP4430_ES1:
	case OMAPDSS_VER_OMAP4430_ES2:
	case OMAPDSS_VER_OMAP4:
		return OMAP_DSS_CHANNEL_LCD2;

	case OMAPDSS_VER_OMAP5:
		return OMAP_DSS_CHANNEL_LCD3;

	default:
		DSSWARN("unsupported DSS version\n");
		return OMAP_DSS_CHANNEL_LCD;
	}
}

static int dpi_connect(struct omap_dss_device *dssdev,
		struct omap_dss_device *dst)
{
	struct dpi_data *dpi = dpi_get_data_from_dssdev(dssdev);
	enum omap_channel channel = dpi->output.dispc_channel;
	int r;

	r = dpi_init_regulator(dpi);
	if (r)
		return r;

	dpi_init_pll(dpi);

	r = dss_mgr_connect(channel, dssdev);
	if (r)
		return r;

	r = omapdss_output_set_device(dssdev, dst);
	if (r) {
		DSSERR("failed to connect output to new device: %s\n",
				dst->name);
		dss_mgr_disconnect(channel, dssdev);
		return r;
	}

	return 0;
}

static void dpi_disconnect(struct omap_dss_device *dssdev,
		struct omap_dss_device *dst)
{
	struct dpi_data *dpi = dpi_get_data_from_dssdev(dssdev);
	enum omap_channel channel = dpi->output.dispc_channel;

	WARN_ON(dst != dssdev->dst);

	if (dst != dssdev->dst)
		return;

	omapdss_output_unset_device(dssdev);

	dss_mgr_disconnect(channel, dssdev);
}

static const struct omapdss_dpi_ops dpi_ops = {
	.connect = dpi_connect,
	.disconnect = dpi_disconnect,

	.enable = dpi_display_enable,
	.disable = dpi_display_disable,

	.check_timings = dpi_check_timings,
	.set_timings = dpi_set_timings,
	.get_timings = dpi_get_timings,
};

static void dpi_init_output_port(struct platform_device *pdev,
	struct device_node *port)
{
	struct dpi_data *dpi = port->data;
	struct omap_dss_device *out = &dpi->output;
	int r;
	u32 port_num;

	r = of_property_read_u32(port, "reg", &port_num);
	if (r)
		port_num = 0;

	switch (port_num) {
	case 2:
		out->name = "dpi.2";
		break;
	case 1:
		out->name = "dpi.1";
		break;
	case 0:
	default:
		out->name = "dpi.0";
		break;
	}

	out->dev = &pdev->dev;
	out->id = OMAP_DSS_OUTPUT_DPI;
	out->output_type = OMAP_DISPLAY_TYPE_DPI;
	out->dispc_channel = dpi_get_channel(port_num);
	out->port_num = port_num;
	out->ops.dpi = &dpi_ops;
	out->owner = THIS_MODULE;

	omapdss_register_output(out);
}

static void dpi_uninit_output_port(struct device_node *port)
{
	struct dpi_data *dpi = port->data;
	struct omap_dss_device *out = &dpi->output;

	omapdss_unregister_output(out);
}

int dpi_init_port(struct platform_device *pdev, struct device_node *port)
{
	struct dpi_data *dpi;
	struct device_node *ep;
	u32 datalines;
	int r;

	dpi = devm_kzalloc(&pdev->dev, sizeof(*dpi), GFP_KERNEL);
	if (!dpi)
		return -ENOMEM;

	ep = omapdss_of_get_next_endpoint(port, NULL);
	if (!ep)
		return 0;

	r = of_property_read_u32(ep, "data-lines", &datalines);
	if (r) {
		DSSERR("failed to parse datalines\n");
		goto err_datalines;
	}

	dpi->data_lines = datalines;

	of_node_put(ep);

	dpi->pdev = pdev;
	port->data = dpi;

	mutex_init(&dpi->lock);

	dpi_init_output_port(pdev, port);

	dpi->port_initialized = true;

	return 0;

err_datalines:
	of_node_put(ep);

	return r;
}

void dpi_uninit_port(struct device_node *port)
{
	struct dpi_data *dpi = port->data;

	if (!dpi->port_initialized)
		return;

	dpi_uninit_output_port(port);
}
