// SPDX-License-Identifier: GPL-2.0-only
/*
 * ld9040 AMOLED LCD drm_panel driver.
 *
 * Copyright (c) 2014 Samsung Electronics Co., Ltd
 * Derived from drivers/video/backlight/ld9040.c
 *
 * Andrzej Hajda <a.hajda@samsung.com>
*/

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>

#include <video/mipi_display.h>
#include <video/of_videomode.h>
#include <video/videomode.h>

#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

/* Manufacturer Command Set */
#define MCS_MANPWR		0xb0
#define MCS_ELVSS_ON		0xb1
#define MCS_USER_SETTING	0xf0
#define MCS_DISPCTL		0xf2
#define MCS_POWER_CTRL		0xf4
#define MCS_GTCON		0xf7
#define MCS_PANEL_CONDITION	0xf8
#define MCS_GAMMA_SET1		0xf9
#define MCS_GAMMA_CTRL		0xfb

/* array of gamma tables for gamma value 2.2 */
static u8 const ld9040_gammas[25][22] = {
	{ 0xf9, 0x00, 0x13, 0xb2, 0xba, 0xd2, 0x00, 0x30, 0x00, 0xaf, 0xc0,
	  0xb8, 0xcd, 0x00, 0x3d, 0x00, 0xa8, 0xb8, 0xb7, 0xcd, 0x00, 0x44 },
	{ 0xf9, 0x00, 0x13, 0xb9, 0xb9, 0xd0, 0x00, 0x3c, 0x00, 0xaf, 0xbf,
	  0xb6, 0xcb, 0x00, 0x4b, 0x00, 0xa8, 0xb9, 0xb5, 0xcc, 0x00, 0x52 },
	{ 0xf9, 0x00, 0x13, 0xba, 0xb9, 0xcd, 0x00, 0x41, 0x00, 0xb0, 0xbe,
	  0xb5, 0xc9, 0x00, 0x51, 0x00, 0xa9, 0xb9, 0xb5, 0xca, 0x00, 0x57 },
	{ 0xf9, 0x00, 0x13, 0xb9, 0xb8, 0xcd, 0x00, 0x46, 0x00, 0xb1, 0xbc,
	  0xb5, 0xc8, 0x00, 0x56, 0x00, 0xaa, 0xb8, 0xb4, 0xc9, 0x00, 0x5d },
	{ 0xf9, 0x00, 0x13, 0xba, 0xb8, 0xcb, 0x00, 0x4b, 0x00, 0xb3, 0xbc,
	  0xb4, 0xc7, 0x00, 0x5c, 0x00, 0xac, 0xb8, 0xb4, 0xc8, 0x00, 0x62 },
	{ 0xf9, 0x00, 0x13, 0xbb, 0xb7, 0xca, 0x00, 0x4f, 0x00, 0xb4, 0xbb,
	  0xb3, 0xc7, 0x00, 0x60, 0x00, 0xad, 0xb8, 0xb4, 0xc7, 0x00, 0x67 },
	{ 0xf9, 0x00, 0x47, 0xba, 0xb6, 0xca, 0x00, 0x53, 0x00, 0xb5, 0xbb,
	  0xb3, 0xc6, 0x00, 0x65, 0x00, 0xae, 0xb8, 0xb3, 0xc7, 0x00, 0x6c },
	{ 0xf9, 0x00, 0x71, 0xbb, 0xb5, 0xc8, 0x00, 0x57, 0x00, 0xb5, 0xbb,
	  0xb0, 0xc5, 0x00, 0x6a, 0x00, 0xae, 0xb9, 0xb1, 0xc6, 0x00, 0x70 },
	{ 0xf9, 0x00, 0x7b, 0xbb, 0xb4, 0xc8, 0x00, 0x5b, 0x00, 0xb5, 0xba,
	  0xb1, 0xc4, 0x00, 0x6e, 0x00, 0xae, 0xb9, 0xb0, 0xc5, 0x00, 0x75 },
	{ 0xf9, 0x00, 0x82, 0xba, 0xb4, 0xc7, 0x00, 0x5f, 0x00, 0xb5, 0xba,
	  0xb0, 0xc3, 0x00, 0x72, 0x00, 0xae, 0xb8, 0xb0, 0xc3, 0x00, 0x7a },
	{ 0xf9, 0x00, 0x89, 0xba, 0xb3, 0xc8, 0x00, 0x62, 0x00, 0xb6, 0xba,
	  0xaf, 0xc3, 0x00, 0x76, 0x00, 0xaf, 0xb7, 0xae, 0xc4, 0x00, 0x7e },
	{ 0xf9, 0x00, 0x8b, 0xb9, 0xb3, 0xc7, 0x00, 0x65, 0x00, 0xb7, 0xb8,
	  0xaf, 0xc3, 0x00, 0x7a, 0x00, 0x80, 0xb6, 0xae, 0xc4, 0x00, 0x81 },
	{ 0xf9, 0x00, 0x93, 0xba, 0xb3, 0xc5, 0x00, 0x69, 0x00, 0xb8, 0xb9,
	  0xae, 0xc1, 0x00, 0x7f, 0x00, 0xb0, 0xb6, 0xae, 0xc3, 0x00, 0x85 },
	{ 0xf9, 0x00, 0x97, 0xba, 0xb2, 0xc5, 0x00, 0x6c, 0x00, 0xb8, 0xb8,
	  0xae, 0xc1, 0x00, 0x82, 0x00, 0xb0, 0xb6, 0xae, 0xc2, 0x00, 0x89 },
	{ 0xf9, 0x00, 0x9a, 0xba, 0xb1, 0xc4, 0x00, 0x6f, 0x00, 0xb8, 0xb8,
	  0xad, 0xc0, 0x00, 0x86, 0x00, 0xb0, 0xb7, 0xad, 0xc0, 0x00, 0x8d },
	{ 0xf9, 0x00, 0x9c, 0xb9, 0xb0, 0xc4, 0x00, 0x72, 0x00, 0xb8, 0xb8,
	  0xac, 0xbf, 0x00, 0x8a, 0x00, 0xb0, 0xb6, 0xac, 0xc0, 0x00, 0x91 },
	{ 0xf9, 0x00, 0x9e, 0xba, 0xb0, 0xc2, 0x00, 0x75, 0x00, 0xb9, 0xb8,
	  0xab, 0xbe, 0x00, 0x8e, 0x00, 0xb0, 0xb6, 0xac, 0xbf, 0x00, 0x94 },
	{ 0xf9, 0x00, 0xa0, 0xb9, 0xaf, 0xc3, 0x00, 0x77, 0x00, 0xb9, 0xb7,
	  0xab, 0xbe, 0x00, 0x90, 0x00, 0xb0, 0xb6, 0xab, 0xbf, 0x00, 0x97 },
	{ 0xf9, 0x00, 0xa2, 0xb9, 0xaf, 0xc2, 0x00, 0x7a, 0x00, 0xb9, 0xb7,
	  0xaa, 0xbd, 0x00, 0x94, 0x00, 0xb0, 0xb5, 0xab, 0xbf, 0x00, 0x9a },
	{ 0xf9, 0x00, 0xa4, 0xb9, 0xaf, 0xc1, 0x00, 0x7d, 0x00, 0xb9, 0xb6,
	  0xaa, 0xbb, 0x00, 0x97, 0x00, 0xb1, 0xb5, 0xaa, 0xbf, 0x00, 0x9d },
	{ 0xf9, 0x00, 0xa4, 0xb8, 0xb0, 0xbf, 0x00, 0x80, 0x00, 0xb8, 0xb6,
	  0xaa, 0xbc, 0x00, 0x9a, 0x00, 0xb0, 0xb5, 0xab, 0xbd, 0x00, 0xa0 },
	{ 0xf9, 0x00, 0xa8, 0xb8, 0xae, 0xbe, 0x00, 0x84, 0x00, 0xb9, 0xb7,
	  0xa8, 0xbc, 0x00, 0x9d, 0x00, 0xb2, 0xb5, 0xaa, 0xbc, 0x00, 0xa4 },
	{ 0xf9, 0x00, 0xa9, 0xb6, 0xad, 0xbf, 0x00, 0x86, 0x00, 0xb8, 0xb5,
	  0xa8, 0xbc, 0x00, 0xa0, 0x00, 0xb3, 0xb3, 0xa9, 0xbc, 0x00, 0xa7 },
	{ 0xf9, 0x00, 0xa9, 0xb7, 0xae, 0xbd, 0x00, 0x89, 0x00, 0xb7, 0xb6,
	  0xa8, 0xba, 0x00, 0xa4, 0x00, 0xb1, 0xb4, 0xaa, 0xbb, 0x00, 0xaa },
	{ 0xf9, 0x00, 0xa7, 0xb4, 0xae, 0xbf, 0x00, 0x91, 0x00, 0xb2, 0xb4,
	  0xaa, 0xbb, 0x00, 0xac, 0x00, 0xb3, 0xb1, 0xaa, 0xbc, 0x00, 0xb3 },
};

struct ld9040 {
	struct device *dev;
	struct drm_panel panel;

	struct regulator_bulk_data supplies[2];
	struct gpio_desc *reset_gpio;
	u32 power_on_delay;
	u32 reset_delay;
	struct videomode vm;
	u32 width_mm;
	u32 height_mm;

	int brightness;

	/* This field is tested by functions directly accessing bus before
	 * transfer, transfer is skipped if it is set. In case of transfer
	 * failure or unexpected response the field is set to error value.
	 * Such construct allows to eliminate many checks in higher level
	 * functions.
	 */
	int error;
};

static inline struct ld9040 *panel_to_ld9040(struct drm_panel *panel)
{
	return container_of(panel, struct ld9040, panel);
}

static int ld9040_clear_error(struct ld9040 *ctx)
{
	int ret = ctx->error;

	ctx->error = 0;
	return ret;
}

static int ld9040_spi_write_word(struct ld9040 *ctx, u16 data)
{
	struct spi_device *spi = to_spi_device(ctx->dev);
	struct spi_transfer xfer = {
		.len		= 2,
		.tx_buf		= &data,
	};
	struct spi_message msg;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	return spi_sync(spi, &msg);
}

static void ld9040_dcs_write(struct ld9040 *ctx, const u8 *data, size_t len)
{
	int ret = 0;

	if (ctx->error < 0 || len == 0)
		return;

	dev_dbg(ctx->dev, "writing dcs seq: %*ph\n", (int)len, data);
	ret = ld9040_spi_write_word(ctx, *data);

	while (!ret && --len) {
		++data;
		ret = ld9040_spi_write_word(ctx, *data | 0x100);
	}

	if (ret) {
		dev_err(ctx->dev, "error %d writing dcs seq: %*ph\n", ret,
			(int)len, data);
		ctx->error = ret;
	}

	usleep_range(300, 310);
}

#define ld9040_dcs_write_seq_static(ctx, seq...) \
({\
	static const u8 d[] = { seq };\
	ld9040_dcs_write(ctx, d, ARRAY_SIZE(d));\
})

static void ld9040_brightness_set(struct ld9040 *ctx)
{
	ld9040_dcs_write(ctx, ld9040_gammas[ctx->brightness],
			 ARRAY_SIZE(ld9040_gammas[ctx->brightness]));

	ld9040_dcs_write_seq_static(ctx, MCS_GAMMA_CTRL, 0x02, 0x5a);
}

static void ld9040_init(struct ld9040 *ctx)
{
	ld9040_dcs_write_seq_static(ctx, MCS_USER_SETTING, 0x5a, 0x5a);
	ld9040_dcs_write_seq_static(ctx, MCS_PANEL_CONDITION,
		0x05, 0x5e, 0x96, 0x6b, 0x7d, 0x0d, 0x3f, 0x00,
		0x00, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x07, 0x05, 0x1f, 0x1f, 0x1f, 0x00, 0x00);
	ld9040_dcs_write_seq_static(ctx, MCS_DISPCTL,
		0x02, 0x06, 0x0a, 0x10, 0x10);
	ld9040_dcs_write_seq_static(ctx, MCS_MANPWR, 0x04);
	ld9040_dcs_write_seq_static(ctx, MCS_POWER_CTRL,
		0x0a, 0x87, 0x25, 0x6a, 0x44, 0x02, 0x88);
	ld9040_dcs_write_seq_static(ctx, MCS_ELVSS_ON, 0x0f, 0x00, 0x16);
	ld9040_dcs_write_seq_static(ctx, MCS_GTCON, 0x09, 0x00, 0x00);
	ld9040_brightness_set(ctx);
	ld9040_dcs_write_seq_static(ctx, MIPI_DCS_EXIT_SLEEP_MODE);
	ld9040_dcs_write_seq_static(ctx, MIPI_DCS_SET_DISPLAY_ON);
}

static int ld9040_power_on(struct ld9040 *ctx)
{
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0)
		return ret;

	msleep(ctx->power_on_delay);
	gpiod_set_value(ctx->reset_gpio, 0);
	msleep(ctx->reset_delay);
	gpiod_set_value(ctx->reset_gpio, 1);
	msleep(ctx->reset_delay);

	return 0;
}

static int ld9040_power_off(struct ld9040 *ctx)
{
	return regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
}

static int ld9040_disable(struct drm_panel *panel)
{
	return 0;
}

static int ld9040_unprepare(struct drm_panel *panel)
{
	struct ld9040 *ctx = panel_to_ld9040(panel);

	msleep(120);
	ld9040_dcs_write_seq_static(ctx, MIPI_DCS_SET_DISPLAY_OFF);
	ld9040_dcs_write_seq_static(ctx, MIPI_DCS_ENTER_SLEEP_MODE);
	msleep(40);

	ld9040_clear_error(ctx);

	return ld9040_power_off(ctx);
}

static int ld9040_prepare(struct drm_panel *panel)
{
	struct ld9040 *ctx = panel_to_ld9040(panel);
	int ret;

	ret = ld9040_power_on(ctx);
	if (ret < 0)
		return ret;

	ld9040_init(ctx);

	ret = ld9040_clear_error(ctx);

	if (ret < 0)
		ld9040_unprepare(panel);

	return ret;
}

static int ld9040_enable(struct drm_panel *panel)
{
	return 0;
}

static int ld9040_get_modes(struct drm_panel *panel,
			    struct drm_connector *connector)
{
	struct ld9040 *ctx = panel_to_ld9040(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_create(connector->dev);
	if (!mode) {
		dev_err(panel->dev, "failed to create a new display mode\n");
		return 0;
	}

	drm_display_mode_from_videomode(&ctx->vm, mode);
	mode->width_mm = ctx->width_mm;
	mode->height_mm = ctx->height_mm;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs ld9040_drm_funcs = {
	.disable = ld9040_disable,
	.unprepare = ld9040_unprepare,
	.prepare = ld9040_prepare,
	.enable = ld9040_enable,
	.get_modes = ld9040_get_modes,
};

static int ld9040_parse_dt(struct ld9040 *ctx)
{
	struct device *dev = ctx->dev;
	struct device_node *np = dev->of_node;
	int ret;

	ret = of_get_videomode(np, &ctx->vm, 0);
	if (ret < 0)
		return ret;

	of_property_read_u32(np, "power-on-delay", &ctx->power_on_delay);
	of_property_read_u32(np, "reset-delay", &ctx->reset_delay);
	of_property_read_u32(np, "panel-width-mm", &ctx->width_mm);
	of_property_read_u32(np, "panel-height-mm", &ctx->height_mm);

	return 0;
}

static int ld9040_bl_update_status(struct backlight_device *dev)
{
	struct ld9040 *ctx = bl_get_data(dev);

	ctx->brightness = backlight_get_brightness(dev);
	ld9040_brightness_set(ctx);

	return 0;
}

static const struct backlight_ops ld9040_bl_ops = {
	.update_status  = ld9040_bl_update_status,
};

static const struct backlight_properties ld9040_bl_props = {
	.type = BACKLIGHT_RAW,
	.scale = BACKLIGHT_SCALE_NON_LINEAR,
	.max_brightness = ARRAY_SIZE(ld9040_gammas) - 1,
	.brightness = ARRAY_SIZE(ld9040_gammas) - 1,
};

static int ld9040_probe(struct spi_device *spi)
{
	struct backlight_device *bldev;
	struct device *dev = &spi->dev;
	struct ld9040 *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct ld9040, panel,
				   &ld9040_drm_funcs,
				   DRM_MODE_CONNECTOR_DPI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	spi_set_drvdata(spi, ctx);

	ctx->dev = dev;
	ctx->brightness = ld9040_bl_props.brightness;

	ret = ld9040_parse_dt(ctx);
	if (ret < 0)
		return ret;

	ctx->supplies[0].supply = "vdd3";
	ctx->supplies[1].supply = "vci";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies),
				      ctx->supplies);
	if (ret < 0)
		return ret;

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(dev, "cannot get reset-gpios %ld\n",
			PTR_ERR(ctx->reset_gpio));
		return PTR_ERR(ctx->reset_gpio);
	}

	spi->bits_per_word = 9;
	ret = spi_setup(spi);
	if (ret < 0) {
		dev_err(dev, "spi setup failed.\n");
		return ret;
	}

	bldev = devm_backlight_device_register(dev, dev_name(dev), dev,
					       ctx, &ld9040_bl_ops,
					       &ld9040_bl_props);
	if (IS_ERR(bldev))
		return PTR_ERR(bldev);

	drm_panel_add(&ctx->panel);

	return 0;
}

static void ld9040_remove(struct spi_device *spi)
{
	struct ld9040 *ctx = spi_get_drvdata(spi);

	ld9040_power_off(ctx);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id ld9040_of_match[] = {
	{ .compatible = "samsung,ld9040" },
	{ }
};
MODULE_DEVICE_TABLE(of, ld9040_of_match);

static const struct spi_device_id ld9040_ids[] = {
	{ "ld9040", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(spi, ld9040_ids);

static struct spi_driver ld9040_driver = {
	.probe = ld9040_probe,
	.remove = ld9040_remove,
	.id_table = ld9040_ids,
	.driver = {
		.name = "panel-samsung-ld9040",
		.of_match_table = ld9040_of_match,
	},
};
module_spi_driver(ld9040_driver);

MODULE_AUTHOR("Andrzej Hajda <a.hajda@samsung.com>");
MODULE_DESCRIPTION("ld9040 LCD Driver");
MODULE_LICENSE("GPL v2");
