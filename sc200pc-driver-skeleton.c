// Minimal reference skeleton for a Linux V4L2 sensor driver targeting
// the Samsung/Intel SC200PC path exposed as ACPI SSLC2000.
//
// This is not intended to compile in this repo as-is. It is a bring-up
// template for porting into a Linux kernel tree under drivers/media/i2c/.

#include <linux/acpi.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define SC200PC_DRV_NAME "sc200pc"

/*
 * Unknown until deeper reversing / hardware probing:
 * - chip ID register addresses / expected values
 * - stream on/off registers
 * - lane count / link frequency tables
 *
 * Start with a probe-only driver that can power the device and attempt a
 * few candidate ID reads while logging enough context to iterate safely.
 */
#define SC200PC_REG16(_hi, _lo) (((_hi) << 8) | (_lo))

/* Common candidates to try during early bring-up. */
#define SC200PC_REG_CHIP_ID_0 SC200PC_REG16(0x31, 0x07)
#define SC200PC_REG_CHIP_ID_1 SC200PC_REG16(0x31, 0x08)
#define SC200PC_REG_CHIP_ID_2 SC200PC_REG16(0x31, 0x09)
#define SC200PC_REG_MODE_SELECT SC200PC_REG16(0x01, 0x00)

struct sc200pc_mode {
	u32 width;
	u32 height;
	u32 code;
};

struct sc200pc {
	struct device *dev;
	struct i2c_client *client;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_mbus_framefmt fmt;
	struct mutex lock;

	struct regulator *avdd;
	struct regulator *dvdd;
	struct regulator *dovdd;
	struct clk *xclk;

	/* These names come directly from the Windows driver strings. */
	struct gpio_desc *reset_gpio;
	struct gpio_desc *power0_gpio;
	struct gpio_desc *power1_gpio;

	bool streaming;
	u32 xclk_freq;
	u32 mipi_lanes;
	u32 mipi_port;
	u32 mipi_mbps;
};

static inline struct sc200pc *to_sc200pc(struct v4l2_subdev *sd)
{
	return container_of(sd, struct sc200pc, sd);
}

static int sc200pc_read_reg(struct sc200pc *sensor, u16 reg, u8 *val)
{
	struct i2c_client *client = sensor->client;
	struct i2c_msg msgs[2];
	u8 addr[2] = { reg >> 8, reg & 0xff };
	int ret;

	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = sizeof(addr);
	msgs[0].buf = addr;

	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = 1;
	msgs[1].buf = val;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	return 0;
}

static int sc200pc_write_reg(struct sc200pc *sensor, u16 reg, u8 val)
{
	struct i2c_client *client = sensor->client;
	u8 buf[3] = { reg >> 8, reg & 0xff, val };
	int ret;

	ret = i2c_master_send(client, buf, sizeof(buf));
	if (ret < 0)
		return ret;
	if (ret != sizeof(buf))
		return -EIO;

	return 0;
}

static int sc200pc_power_on(struct sc200pc *sensor)
{
	int ret;

	if (!IS_ERR(sensor->avdd)) {
		ret = regulator_enable(sensor->avdd);
		if (ret)
			return ret;
	}

	if (!IS_ERR(sensor->dvdd)) {
		ret = regulator_enable(sensor->dvdd);
		if (ret)
			goto err_disable_avdd;
	}

	if (!IS_ERR(sensor->dovdd)) {
		ret = regulator_enable(sensor->dovdd);
		if (ret)
			goto err_disable_dvdd;
	}

	if (!IS_ERR(sensor->xclk)) {
		ret = clk_prepare_enable(sensor->xclk);
		if (ret)
			goto err_disable_dovdd;
	}

	/*
	 * The Windows binary refers to GPIO names "Power0", "Power1", and
	 * "Reset". Exact polarity is not known yet, so keep the first pass
	 * conservative and easy to flip during hardware testing.
	 */
	if (!IS_ERR(sensor->power0_gpio))
		gpiod_set_value_cansleep(sensor->power0_gpio, 1);
	if (!IS_ERR(sensor->power1_gpio))
		gpiod_set_value_cansleep(sensor->power1_gpio, 1);

	usleep_range(2000, 4000);

	if (!IS_ERR(sensor->reset_gpio)) {
		gpiod_set_value_cansleep(sensor->reset_gpio, 1);
		usleep_range(2000, 4000);
		gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	}

	usleep_range(5000, 10000);
	return 0;

err_disable_dovdd:
	if (!IS_ERR(sensor->dovdd))
		regulator_disable(sensor->dovdd);
err_disable_dvdd:
	if (!IS_ERR(sensor->dvdd))
		regulator_disable(sensor->dvdd);
err_disable_avdd:
	if (!IS_ERR(sensor->avdd))
		regulator_disable(sensor->avdd);
	return ret;
}

static void sc200pc_power_off(struct sc200pc *sensor)
{
	if (!IS_ERR(sensor->reset_gpio))
		gpiod_set_value_cansleep(sensor->reset_gpio, 1);
	if (!IS_ERR(sensor->power1_gpio))
		gpiod_set_value_cansleep(sensor->power1_gpio, 0);
	if (!IS_ERR(sensor->power0_gpio))
		gpiod_set_value_cansleep(sensor->power0_gpio, 0);

	if (!IS_ERR(sensor->xclk))
		clk_disable_unprepare(sensor->xclk);
	if (!IS_ERR(sensor->dovdd))
		regulator_disable(sensor->dovdd);
	if (!IS_ERR(sensor->dvdd))
		regulator_disable(sensor->dvdd);
	if (!IS_ERR(sensor->avdd))
		regulator_disable(sensor->avdd);
}

static int sc200pc_identify(struct sc200pc *sensor)
{
	static const u16 candidates[] = {
		SC200PC_REG_CHIP_ID_0,
		SC200PC_REG_CHIP_ID_1,
		SC200PC_REG_CHIP_ID_2,
		SC200PC_REG_MODE_SELECT,
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(candidates); i++) {
		u8 val = 0;
		int ret;

		ret = sc200pc_read_reg(sensor, candidates[i], &val);
		if (!ret)
			dev_info(sensor->dev, "probe read reg 0x%04x = 0x%02x\n",
				 candidates[i], val);
		else
			dev_dbg(sensor->dev, "probe read reg 0x%04x failed: %d\n",
				candidates[i], ret);
	}

	/*
	 * Early milestone: any stable register read after power-on is useful.
	 * Replace this with a real chip ID check once the correct registers are
	 * known from reversing or probing.
	 */
	return 0;
}

static int sc200pc_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct sc200pc *sensor = to_sc200pc(sd);
	int ret = 0;

	mutex_lock(&sensor->lock);

	if (sensor->streaming == !!enable)
		goto out_unlock;

	if (enable) {
		ret = sc200pc_power_on(sensor);
		if (ret)
			goto out_unlock;

		/* Placeholder until stream-on sequence is known. */
		sensor->streaming = true;
	} else {
		sc200pc_power_off(sensor);
		sensor->streaming = false;
	}

out_unlock:
	mutex_unlock(&sensor->lock);
	return ret;
}

static int sc200pc_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;

	/*
	 * Windows assets mention BG1B and NV12. BG1B likely corresponds to a
	 * raw Bayer path before ISP processing. Start with one raw media-bus
	 * code and revise after hardware validation.
	 */
	code->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	return 0;
}

static int sc200pc_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *state,
			   struct v4l2_subdev_format *fmt)
{
	struct sc200pc *sensor = to_sc200pc(sd);

	mutex_lock(&sensor->lock);
	fmt->format = sensor->fmt;
	mutex_unlock(&sensor->lock);

	return 0;
}

static int sc200pc_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *state,
			   struct v4l2_subdev_format *fmt)
{
	struct sc200pc *sensor = to_sc200pc(sd);

	mutex_lock(&sensor->lock);

	sensor->fmt.width = 1600;
	sensor->fmt.height = 1200;
	sensor->fmt.code = MEDIA_BUS_FMT_SBGGR10_1X10;
	sensor->fmt.field = V4L2_FIELD_NONE;
	sensor->fmt.colorspace = V4L2_COLORSPACE_RAW;

	fmt->format = sensor->fmt;

	mutex_unlock(&sensor->lock);
	return 0;
}

static const struct v4l2_subdev_video_ops sc200pc_video_ops = {
	.s_stream = sc200pc_s_stream,
};

static const struct v4l2_subdev_pad_ops sc200pc_pad_ops = {
	.enum_mbus_code = sc200pc_enum_mbus_code,
	.get_fmt = sc200pc_get_fmt,
	.set_fmt = sc200pc_set_fmt,
};

static const struct v4l2_subdev_ops sc200pc_subdev_ops = {
	.video = &sc200pc_video_ops,
	.pad = &sc200pc_pad_ops,
};

static int sc200pc_parse_firmware(struct sc200pc *sensor)
{
	/*
	 * Placeholder. The Windows driver clearly consumes more than a plain
	 * ACPI ID:
	 * - _CRS for I2C resources
	 * - SSDB for static sensor metadata
	 * - likely parts of _DSM for richer device descriptions
	 *
	 * The first Linux pass can often get by with the instantiated I2C
	 * client plus hardcoded defaults. Move this to a real parser once probe
	 * works and you need lane count / clock / port values to be exact.
	 */
	sensor->mipi_lanes = 2;
	sensor->mipi_port = 0;
	sensor->mipi_mbps = 0;
	return 0;
}

static int sc200pc_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct sc200pc *sensor;
	int ret;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->dev = dev;
	sensor->client = client;
	mutex_init(&sensor->lock);

	sensor->avdd = devm_regulator_get_optional(dev, "avdd");
	sensor->dvdd = devm_regulator_get_optional(dev, "dvdd");
	sensor->dovdd = devm_regulator_get_optional(dev, "dovdd");
	sensor->xclk = devm_clk_get_optional(dev, NULL);

	sensor->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_HIGH);
	sensor->power0_gpio = devm_gpiod_get_optional(dev, "power0",
						      GPIOD_OUT_LOW);
	sensor->power1_gpio = devm_gpiod_get_optional(dev, "power1",
						      GPIOD_OUT_LOW);

	ret = sc200pc_parse_firmware(sensor);
	if (ret)
		return ret;

	v4l2_i2c_subdev_init(&sensor->sd, client, &sc200pc_subdev_ops);
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret)
		return ret;

	sensor->fmt.width = 1600;
	sensor->fmt.height = 1200;
	sensor->fmt.code = MEDIA_BUS_FMT_SBGGR10_1X10;
	sensor->fmt.field = V4L2_FIELD_NONE;
	sensor->fmt.colorspace = V4L2_COLORSPACE_RAW;

	ret = sc200pc_power_on(sensor);
	if (ret)
		goto err_entity_cleanup;

	ret = sc200pc_identify(sensor);
	sc200pc_power_off(sensor);
	if (ret)
		goto err_entity_cleanup;

	ret = v4l2_async_register_subdev_sensor(&sensor->sd);
	if (ret)
		goto err_entity_cleanup;

	i2c_set_clientdata(client, sensor);
	dev_info(dev, "SC200PC skeleton bound on addr 0x%02x\n", client->addr);
	return 0;

err_entity_cleanup:
	media_entity_cleanup(&sensor->sd.entity);
	return ret;
}

static void sc200pc_remove(struct i2c_client *client)
{
	struct sc200pc *sensor = i2c_get_clientdata(client);

	v4l2_async_unregister_subdev(&sensor->sd);
	media_entity_cleanup(&sensor->sd.entity);
}

static const struct acpi_device_id sc200pc_acpi_ids[] = {
	{ "SSLC2000" },
	{ }
};
MODULE_DEVICE_TABLE(acpi, sc200pc_acpi_ids);

static struct i2c_driver sc200pc_i2c_driver = {
	.driver = {
		.name = SC200PC_DRV_NAME,
		.acpi_match_table = sc200pc_acpi_ids,
	},
	.probe_new = sc200pc_probe,
	.remove = sc200pc_remove,
};

module_i2c_driver(sc200pc_i2c_driver);

MODULE_DESCRIPTION("Reference skeleton for Samsung/Intel SC200PC camera sensor");
MODULE_LICENSE("GPL");
