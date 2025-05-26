// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2023 Aoba K
// Copyright (c) 2013, The Linux Foundation. All rights reserved.

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct novatek_nt71391 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator *supply;
	const struct nt71391_panel_desc *desc;
	bool prepared;
};


struct nt71391_panel_desc {
	const char *panel_name;
	const struct drm_display_mode drm_mode;
	unsigned long mode_flags;
	u32 bus_flags;
};

static inline
struct novatek_nt71391 *to_novatek_nt71391(struct drm_panel *panel)
{
	return container_of(panel, struct novatek_nt71391, panel);
}

static void novatek_nt71391_reset(struct novatek_nt71391 *ctx)
{
	return;
}

static int novatek_nt71391_on(struct novatek_nt71391 *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };
	int ret = 0;

	msleep(120);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf3, 0xa0); // UNLOCK_PAGE0
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xac, 0x18); // FREQ_SETTING
	mipi_dsi_turn_on_peripheral(dsi);

	usleep_range(10000, 11000);

	return ret;
}

static int novatek_nt71391_off(struct novatek_nt71391 *ctx)
{
	return 0;
}

static int novatek_nt71391_prepare(struct drm_panel *panel)
{
	struct novatek_nt71391 *ctx = to_novatek_nt71391(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	if (ctx->prepared)
		return 0;

	ret = regulator_enable(ctx->supply);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulator: %d\n", ret);
		return ret;
	}

	novatek_nt71391_reset(ctx);

	ret = novatek_nt71391_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		regulator_disable(ctx->supply);
		return ret;
	}

	ctx->prepared = true;
	return 0;
}

static int novatek_nt71391_unprepare(struct drm_panel *panel)
{
	struct novatek_nt71391 *ctx = to_novatek_nt71391(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	if (!ctx->prepared)
		return 0;

	ret = novatek_nt71391_off(ctx);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	regulator_disable(ctx->supply);

	ctx->prepared = false;
	return 0;
}

static const struct drm_display_mode novatek_nt71391_mode = {
	.clock = (1280 + 25 + 41 + 25) * (800 + 8 + 6 + 3) * 60 / 1000,
	.hdisplay = 1280,
	.hsync_start = 1280 + 25,
	.hsync_end = 1280 + 25 + 41,
	.htotal = 1280 + 25 + 41 + 25,
	.vdisplay = 800,
	.vsync_start = 800 + 8,
	.vsync_end = 800 + 8 + 6,
	.vtotal = 800 + 8 + 6 + 3,
	.width_mm = 172,
	.height_mm = 108,
};

static const struct nt71391_panel_desc nt71391_bp070wx1_desc = {
	.panel_name = "bp070wx1",
	.drm_mode = novatek_nt71391_mode,
	.mode_flags = MIPI_DSI_MODE_VIDEO_NO_HFP,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH
};

static int novatek_nt71391_get_modes(struct drm_panel *panel,
				     struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	struct novatek_nt71391 *ctx;

	ctx = container_of(panel, struct novatek_nt71391, panel);
	if (!ctx)
		return -EINVAL;

	mode = drm_mode_duplicate(connector->dev, &ctx->desc->drm_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	connector->display_info.bus_flags = ctx->desc->bus_flags;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs novatek_nt71391_panel_funcs = {
	.prepare = novatek_nt71391_prepare,
	.unprepare = novatek_nt71391_unprepare,
	.get_modes = novatek_nt71391_get_modes,
};

static int novatek_nt71391_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct novatek_nt71391 *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->desc = of_device_get_match_data(dev);
	if (!ctx->desc)
		return -ENODEV;

	ctx->supply = devm_regulator_get(dev, "power");
	if (IS_ERR(ctx->supply))
		return dev_err_probe(dev, PTR_ERR(ctx->supply),
				     "Failed to get power regulator\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  ctx->desc->mode_flags;

	drm_panel_init(&ctx->panel, dev, &novatek_nt71391_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);
	ctx->panel.prepare_prev_first = true;

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get backlight\n");

	drm_panel_add(&ctx->panel);

	ret = devm_mipi_dsi_attach(dev, dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret,
				     "Failed to attach to DSI host\n");
	}


	return 0;
}

static void novatek_nt71391_remove(struct mipi_dsi_device *dsi)
{
	struct novatek_nt71391 *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id novatek_nt71391_of_match[] = {
	{
		.compatible = "novatek,nt71391",
		.data = &nt71391_bp070wx1_desc
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, novatek_nt71391_of_match);

static struct mipi_dsi_driver novatek_nt71391_driver = {
	.probe = novatek_nt71391_probe,
	.remove = novatek_nt71391_remove,
	.driver = {
		.name = "panel-novatek-nt71391",
		.of_match_table = novatek_nt71391_of_match,
	},
};
module_mipi_dsi_driver(novatek_nt71391_driver);

MODULE_AUTHOR("eval Nya <nexp_0x17@outlook.com>");
MODULE_DESCRIPTION("DRM driver for NT71391_BP080WX7_WXGA panel");
MODULE_LICENSE("GPL");
