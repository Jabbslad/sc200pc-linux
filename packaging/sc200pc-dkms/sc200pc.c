// V4L2 sensor driver for the Samsung/Intel SC200PC camera, exposed as
// ACPI SSLC2000 on Panther Lake laptops (Samsung Galaxy Book6 Pro).
//
// Bring-up status:
//   - probe / chip-ID read: verified on target hardware
//   - stream-on: register table ported from the Espressif SC202CS driver
//     (https://github.com/espressif/esp-video-components) which is the
//     closest upstream-licensed SmartSens SC20x sibling. The two silicon
//     variants share the register map; chip ID and a few PHY-specific
//     values differ. Validation on SC200PC hardware is expected to reveal
//     tuning needed for 2-lane MIPI output.
//   - controls (exposure, gain, test pattern): not yet wired

#include <linux/acpi.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define SC200PC_DRV_NAME "sc200pc"

/* Register addresses (SmartSens SC20x family, verified against SC202CS). */
#define SC200PC_REG_SLEEP_MODE		0x0100
#define SC200PC_REG_SOFT_RESET		0x0103
#define SC200PC_REG_CHIP_ID_H		0x3107
#define SC200PC_REG_CHIP_ID_L		0x3108
#define SC200PC_REG_CHIP_REVISION	0x3109

/*
 * Sensor returns (0x0b << 8) | 0x71 on reg reads of 0x3107/0x3108.
 * Silicon revision byte at 0x3109 reads 0x01 on this sample.
 */
#define SC200PC_CHIP_ID			0x0b71

/*
 * MIPI CSI-2 link frequency (Hz, DDR) derived from the real sc200pc.sys
 * init table:
 *   HTS = 0x047e (1150), VTS = 0x08d4 (2260)
 *   pixel clock = HTS * VTS * 30 fps = 77.97 MHz
 *   bits per pixel = 10 (raw10)
 *   lanes = 2 (from 0x330e = 0x28, high nibble = 2)
 *   link_freq (DDR) = pixel_clock * bpp / (2 * lanes) = 194.9 MHz
 *
 * Rounded to 195 MHz; IPU7 PHY is forgiving of a few percent drift.
 */
#define SC200PC_LINK_FREQ_DEFAULT	195000000ULL

/* Native output resolution programmed by the init table. */
#define SC200PC_WIDTH			1928
#define SC200PC_HEIGHT			1088
#define SC200PC_FPS			30
#define SC200PC_HTS			0x047e	/* 1150 internal clocks/line */
#define SC200PC_VTS_DEF			0x08d4	/* 2260 lines/frame */
#define SC200PC_VTS_MIN			(SC200PC_HEIGHT + 8)
#define SC200PC_VTS_MAX			0x7fff

/*
 * The SC200PC uses internal column-parallel readout: multiple pixels are
 * sampled per internal clock cycle, so HTS (1150) < active width (1928).
 * V4L2's timing model requires line_length_pixels >= active_width, and
 * computes: llp = HBLANK + width, fll = VBLANK + height.
 *
 * We report pixel_rate at 2× the internal rate and treat line_length as
 * 2×HTS.  This keeps frame-duration arithmetic exact:
 *   llp = 2*1150 = 2300,  HBLANK = 2300 − 1928 = 372
 *   fll = 2260,           VBLANK = 2260 − 1088 = 1172
 *   pixel_rate = 2300 × 2260 × 30 = 155 940 000
 *   frame_dur  = 2300 × 2260 / 155940000 = 33.33 ms  (30 fps)  ✓
 *
 * Exposure and gain units are unaffected — they are in lines and codes,
 * not pixels.
 */
#define SC200PC_INTERNAL_PARALLELISM	2
#define SC200PC_LLP			(SC200PC_HTS * SC200PC_INTERNAL_PARALLELISM)
#define SC200PC_HBLANK_DEF		(SC200PC_LLP - SC200PC_WIDTH)
#define SC200PC_VBLANK_DEF		(SC200PC_VTS_DEF - SC200PC_HEIGHT)
#define SC200PC_PIXEL_RATE_DEFAULT	((u64)SC200PC_LLP * SC200PC_VTS_DEF * SC200PC_FPS)

#define SC200PC_EXPOSURE_MIN		1
#define SC200PC_EXPOSURE_MAX_MARGIN	8
#define SC200PC_EXPOSURE_MAX		(SC200PC_VTS_DEF - SC200PC_EXPOSURE_MAX_MARGIN)
#define SC200PC_EXPOSURE_DEFAULT	0x08b0
#define SC200PC_ANALOGUE_GAIN_MIN	0x10
#define SC200PC_ANALOGUE_GAIN_MAX	0x20
#define SC200PC_ANALOGUE_GAIN_DEFAULT	0x10

/*
 * Digital gain: the HAL's 3A engine (AIQB) sends V4L2_CID_DIGITAL_GAIN
 * with 128 = 1.0×.  Accept the control so the exposure update path
 * doesn't abort.  Not wired to hardware registers yet.
 */
#define SC200PC_DIGITAL_GAIN_MIN	1
#define SC200PC_DIGITAL_GAIN_MAX	4096
#define SC200PC_DIGITAL_GAIN_DEFAULT	128

/* VTS register pair (big-endian 16-bit). */
#define SC200PC_REG_VTS_H		0x320e
#define SC200PC_REG_VTS_L		0x320f

/* Init table terminators. */
#define SC200PC_REG_END			0xffff
#define SC200PC_REG_DELAY		0xfffe

struct sc200pc_reg {
	u16 addr;
	u8  val;
};

/*
 * 1928x1088 RAW10 Bayer BGGR, 30 fps, 2-lane MIPI CSI-2.
 *
 * Extracted verbatim from the OEM Windows driver (sc200pc.sys,
 * version 71.26100.0.11, Microsoft Update Catalog package for
 * hardware ID ACPI\SSLC2000). The .sys stores init tables as an array
 * of {u32 op, u32 addr, u32 value, u32 reserved} entries in .rdata;
 * this is the 141-entry main streaming table. See comments in
 * camera-bringup-plan.md for the extraction method.
 *
 *   output size (active):    1920 x 1080
 *   output size (with blank):1928 x 1088
 *   HTS = 0x047e = 1150
 *   VTS = 0x08d4 = 2260
 *   lane count via 0x330e:   upper nibble 2 = 2 lanes
 *   exposure (0x3e00-0x3e02): 0x0008b0 = 2224 lines
 */
static const struct sc200pc_reg sc200pc_1928x1088_raw10_30fps[] = {
	{ 0x0103, 0x01 },		/* software reset */
	{ 0x301f, 0x29 },
	{ 0x3200, 0x00 },
	{ 0x3201, 0x00 },
	{ 0x3202, 0x00 },
	{ 0x3203, 0x00 },
	{ 0x3204, 0x07 },
	{ 0x3205, 0x8f },
	{ 0x3206, 0x04 },
	{ 0x3207, 0x47 },
	{ 0x3208, 0x07 },		/* output_width high = 0x0788 */
	{ 0x3209, 0x88 },		/* output_width low             */
	{ 0x320a, 0x04 },		/* output_height high = 0x0440  */
	{ 0x320b, 0x40 },		/* output_height low            */
	{ 0x320c, 0x04 },		/* HTS high = 0x047e            */
	{ 0x320d, 0x7e },		/* HTS low                      */
	{ 0x320e, 0x08 },		/* VTS high = 0x08d4            */
	{ 0x320f, 0xd4 },		/* VTS low                      */
	{ 0x3210, 0x00 },
	{ 0x3211, 0x04 },
	{ 0x3212, 0x00 },
	{ 0x3213, 0x04 },
	{ 0x3250, 0xff },
	{ 0x3253, 0x60 },
	{ 0x325f, 0x80 },
	{ 0x327f, 0x3f },
	{ 0x3281, 0x01 },
	{ 0x32d1, 0x70 },
	{ 0x3301, 0x07 },
	{ 0x3302, 0x18 },
	{ 0x3306, 0x30 },
	{ 0x3308, 0x10 },
	{ 0x330b, 0x78 },
	{ 0x330e, 0x28 },		/* MIPI lane cfg: 2 lanes (upper) */
	{ 0x330f, 0x01 },
	{ 0x3310, 0x01 },
	{ 0x331e, 0x21 },
	{ 0x331f, 0x21 },
	{ 0x3333, 0x10 },
	{ 0x3334, 0x40 },
	{ 0x3347, 0x05 },
	{ 0x334c, 0x08 },
	{ 0x335d, 0x60 },
	{ 0x3364, 0x56 },
	{ 0x3390, 0x08 },
	{ 0x3391, 0x38 },
	{ 0x3393, 0x0e },
	{ 0x3394, 0x10 },
	{ 0x33ad, 0x1c },
	{ 0x33b0, 0x0f },
	{ 0x33b1, 0x80 },
	{ 0x33b2, 0x58 },
	{ 0x33b3, 0x08 },
	{ 0x349f, 0x02 },
	{ 0x34a6, 0x18 },
	{ 0x34a7, 0x38 },
	{ 0x34a8, 0x07 },
	{ 0x34a9, 0x06 },
	{ 0x3619, 0x20 },		/* PLL / analog frontend         */
	{ 0x361a, 0x91 },
	{ 0x3633, 0x48 },
	{ 0x3637, 0x49 },
	{ 0x3638, 0xa1 },
	{ 0x3660, 0x80 },
	{ 0x3661, 0x86 },
	{ 0x3662, 0x8e },
	{ 0x3667, 0x38 },
	{ 0x3668, 0x78 },
	{ 0x3670, 0x65 },
	{ 0x3671, 0x45 },
	{ 0x3672, 0x45 },
	{ 0x3680, 0x46 },
	{ 0x3681, 0x66 },
	{ 0x3682, 0x88 },
	{ 0x3683, 0x29 },
	{ 0x3684, 0x39 },
	{ 0x3685, 0x39 },
	{ 0x36c0, 0x08 },
	{ 0x36c1, 0x18 },
	{ 0x36c8, 0x18 },
	{ 0x36c9, 0x78 },
	{ 0x36ca, 0x08 },
	{ 0x36cb, 0x78 },
	{ 0x3718, 0x04 },
	{ 0x3723, 0x20 },
	{ 0x3724, 0xe1 },
	{ 0x3770, 0x03 },
	{ 0x3771, 0x03 },
	{ 0x3772, 0x03 },
	{ 0x37c0, 0x08 },
	{ 0x37c1, 0x78 },
	{ 0x37ea, 0x10 },
	{ 0x37ed, 0x89 },
	{ 0x37f9, 0x00 },
	{ 0x37fa, 0x0c },
	{ 0x37fb, 0xca },
	{ 0x3901, 0x08 },
	{ 0x3902, 0xc0 },
	{ 0x3903, 0x40 },
	{ 0x3908, 0x40 },
	{ 0x3909, 0x01 },
	{ 0x390a, 0x81 },
	{ 0x3929, 0x18 },
	{ 0x3933, 0x80 },
	{ 0x3934, 0x01 },
	{ 0x3937, 0x80 },
	{ 0x3939, 0x0f },
	{ 0x393a, 0xfe },
	{ 0x393d, 0x01 },
	{ 0x393e, 0xff },
	{ 0x39dd, 0x06 },
	{ 0x3c0f, 0x02 },
	{ 0x3e00, 0x00 },		/* exposure high                 */
	{ 0x3e01, 0x08 },		/* exposure mid                  */
	{ 0x3e02, 0xb0 },		/* exposure low = 0x0008b0       */
	{ 0x3e03, 0x0b },
	{ 0x3e04, 0x10 },
	{ 0x3e05, 0x70 },
	{ 0x3e09, 0x10 },		/* analog gain                   */
	{ 0x3e23, 0x00 },
	{ 0x3e24, 0x86 },
	{ 0x3f09, 0x0e },
	{ 0x4407, 0x0c },
	{ 0x4509, 0x1e },		/* MIPI tuning                   */
	{ 0x450d, 0x01 },
	{ 0x450f, 0x06 },
	{ 0x4820, 0x00 },
	{ 0x4821, 0xb2 },
	{ 0x482e, 0x34 },
	{ 0x4837, 0x13 },
	{ 0x5000, 0x06 },
	{ 0x5002, 0x06 },
	{ 0x5784, 0x0c },
	{ 0x5785, 0x04 },
	{ 0x578d, 0x40 },
	{ 0x57ac, 0x00 },
	{ 0x57ad, 0x00 },
	{ 0x58e0, 0xae },
	{ 0x3802, 0x01 },
	{ 0x3021, 0x67 },
	{ SC200PC_REG_SLEEP_MODE, 0x00 }, /* stay in sleep until s_stream */
	{ SC200PC_REG_END, 0x00 },
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

	/* Names come directly from Windows sc200pc.sys strings. */
	struct gpio_desc *reset_gpio;
	struct gpio_desc *power0_gpio;
	struct gpio_desc *power1_gpio;

	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *analogue_gain;
	struct v4l2_ctrl *digital_gain;
	struct v4l2_ctrl *test_pattern;

	u16  cur_vts;
	bool streaming;
	u32 xclk_freq;
	u32 mipi_lanes;
	u32 mipi_port;
	u32 mipi_mbps;
	u16 chip_id;
	u8  chip_rev;
};

static const s64 sc200pc_link_freqs[] = {
	SC200PC_LINK_FREQ_DEFAULT,
};

static const char * const sc200pc_test_pattern_menu[] = {
	"Off",
	"Color Bars",
};

static const struct v4l2_rect sc200pc_pixel_array = {
	.left = 0,
	.top = 0,
	.width = SC200PC_WIDTH,
	.height = SC200PC_HEIGHT,
};

static inline struct sc200pc *to_sc200pc(struct v4l2_subdev *sd)
{
	return container_of(sd, struct sc200pc, sd);
}

static inline struct sc200pc *ctrl_to_sc200pc(struct v4l2_ctrl *ctrl)
{
	return container_of(ctrl->handler, struct sc200pc, ctrls);
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

static int sc200pc_write_array(struct sc200pc *sensor,
			       const struct sc200pc_reg *regs)
{
	int ret;

	for (; regs->addr != SC200PC_REG_END; regs++) {
		if (regs->addr == SC200PC_REG_DELAY) {
			msleep(regs->val);
			continue;
		}

		ret = sc200pc_write_reg(sensor, regs->addr, regs->val);
		if (ret) {
			dev_err(sensor->dev,
				"failed write reg 0x%04x = 0x%02x: %d\n",
				regs->addr, regs->val, ret);
			return ret;
		}
	}

	return 0;
}

static int sc200pc_set_exposure(struct sc200pc *sensor, u32 exposure)
{
	int ret;

	/*
	 * SmartSens SC20x-family sensors encode coarse integration time as a
	 * 12.4 fixed-point value across 0x3e00..0x3e02.
	 */
	exposure = clamp_t(u32, exposure,
			   SC200PC_EXPOSURE_MIN, SC200PC_EXPOSURE_MAX);

	ret = sc200pc_write_reg(sensor, 0x3e00, (exposure >> 12) & 0x0f);
	if (ret)
		return ret;

	ret = sc200pc_write_reg(sensor, 0x3e01, (exposure >> 4) & 0xff);
	if (ret)
		return ret;

	return sc200pc_write_reg(sensor, 0x3e02, (exposure & 0x0f) << 4);
}

static int sc200pc_set_analogue_gain(struct sc200pc *sensor, u32 gain)
{
	gain = clamp_t(u32, gain,
		       SC200PC_ANALOGUE_GAIN_MIN, SC200PC_ANALOGUE_GAIN_MAX);
	return sc200pc_write_reg(sensor, 0x3e09, gain & 0xff);
}

static int sc200pc_apply_controls(struct sc200pc *sensor)
{
	int ret;

	ret = sc200pc_set_exposure(sensor, sensor->exposure->val);
	if (ret)
		return ret;

	return sc200pc_set_analogue_gain(sensor, sensor->analogue_gain->val);
}

static int sc200pc_set_vts(struct sc200pc *sensor, u16 vts)
{
	int ret;

	ret = sc200pc_write_reg(sensor, SC200PC_REG_VTS_H, vts >> 8);
	if (ret)
		return ret;

	ret = sc200pc_write_reg(sensor, SC200PC_REG_VTS_L, vts & 0xff);
	if (ret)
		return ret;

	sensor->cur_vts = vts;
	return 0;
}

static int sc200pc_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sc200pc *sensor = ctrl_to_sc200pc(ctrl);
	int ret = 0;

	mutex_lock(&sensor->lock);

	/*
	 * VBLANK / HBLANK are cached even while not streaming so that
	 * the HAL can read back the value it wrote before stream-on.
	 */
	switch (ctrl->id) {
	case V4L2_CID_VBLANK: {
		u16 vts = ctrl->val + SC200PC_HEIGHT;

		if (sensor->streaming) {
			ret = sc200pc_set_vts(sensor, vts);
		} else {
			sensor->cur_vts = vts;
		}
		break;
	}
	case V4L2_CID_HBLANK:
		/* Accept the write; HTS is fixed by the init table. */
		ret = 0;
		break;
	default:
		break;
	}

	if (!sensor->streaming)
		goto out_unlock;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = sc200pc_set_exposure(sensor, ctrl->val);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = sc200pc_set_analogue_gain(sensor, ctrl->val);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		/* Accepted but not wired to hardware yet. */
		ret = 0;
		break;
	case V4L2_CID_TEST_PATTERN:
		/*
		 * The HAL always sends TEST_PATTERN=Off during startup.
		 * Accept that request even though test-pattern programming
		 * is not wired yet, so the sensor can be configured.
		 */
		ret = 0;
		break;
	default:
		break;
	}

out_unlock:
	mutex_unlock(&sensor->lock);
	return ret;
}

static const struct v4l2_ctrl_ops sc200pc_ctrl_ops = {
	.s_ctrl = sc200pc_s_ctrl,
};

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
	u8 id_h = 0, id_l = 0, rev = 0;
	u16 chip_id;
	int ret;

	ret = sc200pc_read_reg(sensor, SC200PC_REG_CHIP_ID_H, &id_h);
	if (ret)
		return dev_err_probe(sensor->dev, ret,
				     "failed to read chip ID high byte\n");

	ret = sc200pc_read_reg(sensor, SC200PC_REG_CHIP_ID_L, &id_l);
	if (ret)
		return dev_err_probe(sensor->dev, ret,
				     "failed to read chip ID low byte\n");

	ret = sc200pc_read_reg(sensor, SC200PC_REG_CHIP_REVISION, &rev);
	if (ret)
		dev_warn(sensor->dev, "failed to read chip revision: %d\n", ret);

	chip_id = ((u16)id_h << 8) | id_l;
	sensor->chip_id = chip_id;
	sensor->chip_rev = rev;

	if (chip_id != SC200PC_CHIP_ID)
		return dev_err_probe(sensor->dev, -ENODEV,
				     "unexpected chip ID 0x%04x, expected 0x%04x\n",
				     chip_id, SC200PC_CHIP_ID);

	dev_info(sensor->dev, "detected SC200PC (chip ID 0x%04x, rev 0x%02x)\n",
		 chip_id, rev);
	return 0;
}

static int sc200pc_start_streaming(struct sc200pc *sensor)
{
	int ret;

	dev_info(sensor->dev, "writing init table (%zu entries)...\n",
		 ARRAY_SIZE(sc200pc_1928x1088_raw10_30fps) - 1);

	ret = sc200pc_write_array(sensor, sc200pc_1928x1088_raw10_30fps);
	if (ret)
		return ret;

	/*
	 * Apply VTS if the HAL changed VBLANK before stream-on.
	 * The init table sets VTS to SC200PC_VTS_DEF; only re-write
	 * if the cached value differs.
	 */
	if (sensor->cur_vts != SC200PC_VTS_DEF) {
		ret = sc200pc_set_vts(sensor, sensor->cur_vts);
		if (ret)
			return ret;
	}

	/* Releasing sleep starts the MIPI output. */
	ret = sc200pc_write_reg(sensor, SC200PC_REG_SLEEP_MODE, 0x01);
	if (ret)
		return ret;

	ret = sc200pc_apply_controls(sensor);
	if (ret)
		return ret;

	dev_info(sensor->dev, "streaming started\n");
	return 0;
}

static int sc200pc_stop_streaming(struct sc200pc *sensor)
{
	int ret;

	ret = sc200pc_write_reg(sensor, SC200PC_REG_SLEEP_MODE, 0x00);
	if (ret)
		dev_warn(sensor->dev, "failed to clear stream enable: %d\n", ret);

	dev_dbg(sensor->dev, "streaming stopped\n");
	return ret;
}

static int sc200pc_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct sc200pc *sensor = to_sc200pc(sd);
	int ret = 0;

	dev_info(sensor->dev, "s_stream(enable=%d) called\n", enable);

	mutex_lock(&sensor->lock);

	if (sensor->streaming == !!enable)
		goto out_unlock;

	if (enable) {
		ret = sc200pc_power_on(sensor);
		if (ret)
			goto out_unlock;

		ret = sc200pc_start_streaming(sensor);
		if (ret) {
			sc200pc_power_off(sensor);
			goto out_unlock;
		}

		sensor->streaming = true;
	} else {
		sc200pc_stop_streaming(sensor);
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

	code->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	return 0;
}

static int sc200pc_enum_frame_size(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	struct sc200pc *sensor = to_sc200pc(sd);

	dev_info(sensor->dev, "enum_frame_size idx=%u code=0x%x\n",
		 fse->index, fse->code);

	if (fse->index)
		return -EINVAL;

	if (fse->code != MEDIA_BUS_FMT_SBGGR10_1X10)
		return -EINVAL;

	fse->min_width = SC200PC_WIDTH;
	fse->max_width = SC200PC_WIDTH;
	fse->min_height = SC200PC_HEIGHT;
	fse->max_height = SC200PC_HEIGHT;

	return 0;
}

static int sc200pc_enum_frame_interval(struct v4l2_subdev *sd,
				       struct v4l2_subdev_state *state,
				       struct v4l2_subdev_frame_interval_enum *fie)
{
	struct sc200pc *sensor = to_sc200pc(sd);

	dev_info(sensor->dev,
		 "enum_frame_interval idx=%u code=0x%x %ux%u\n",
		 fie->index, fie->code, fie->width, fie->height);

	if (fie->index)
		return -EINVAL;

	if (fie->code != MEDIA_BUS_FMT_SBGGR10_1X10 ||
	    fie->width != SC200PC_WIDTH ||
	    fie->height != SC200PC_HEIGHT)
		return -EINVAL;

	fie->interval.numerator = 1;
	fie->interval.denominator = SC200PC_FPS;

	return 0;
}

static int sc200pc_get_selection(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_selection *sel)
{
	struct sc200pc *sensor = to_sc200pc(sd);

	if (sel->pad != 0)
		return -EINVAL;

	dev_info(sensor->dev, "get_selection target=%u\n", sel->target);

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r = sc200pc_pixel_array;
		return 0;
	default:
		return -EINVAL;
	}
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

	sensor->fmt.width = SC200PC_WIDTH;
	sensor->fmt.height = SC200PC_HEIGHT;
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
	.enum_frame_size = sc200pc_enum_frame_size,
	.enum_frame_interval = sc200pc_enum_frame_interval,
	.get_fmt = sc200pc_get_fmt,
	.set_fmt = sc200pc_set_fmt,
	.get_selection = sc200pc_get_selection,
};

static const struct v4l2_subdev_ops sc200pc_subdev_ops = {
	.video = &sc200pc_video_ops,
	.pad = &sc200pc_pad_ops,
};

static int sc200pc_parse_firmware(struct sc200pc *sensor)
{
	struct acpi_device *adev = ACPI_COMPANION(sensor->dev);
	struct fwnode_handle *fwnode = dev_fwnode(sensor->dev);
	struct fwnode_handle *ep;
	struct v4l2_fwnode_endpoint vep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	int ret;

	sensor->mipi_lanes = 2;
	sensor->mipi_port = 0;
	sensor->mipi_mbps = 0;

	dev_info(sensor->dev,
		 "fwnode=%p acpi=%d software=%d acpi-companion=%s hid=%s\n",
		 fwnode,
		 fwnode ? is_acpi_node(fwnode) : 0,
		 fwnode ? is_software_node(fwnode) : 0,
		 adev ? acpi_dev_name(adev) : "<none>",
		 adev ? acpi_device_hid(adev) : "<none>");

	ep = fwnode_graph_get_next_endpoint(dev_fwnode(sensor->dev), NULL);
	if (!ep) {
		dev_warn(sensor->dev,
			 "no firmware endpoint on sensor device; graph integration likely incomplete\n");
		return 0;
	}

	ret = v4l2_fwnode_endpoint_parse(ep, &vep);
	if (ret) {
		dev_warn(sensor->dev,
			 "failed to parse firmware endpoint: %d\n", ret);
		fwnode_handle_put(ep);
		return 0;
	}

	if (vep.bus_type == V4L2_MBUS_CSI2_DPHY ||
	    vep.bus_type == V4L2_MBUS_CSI2_CPHY)
		sensor->mipi_lanes = vep.bus.mipi_csi2.num_data_lanes;

	dev_info(sensor->dev,
		 "firmware endpoint: bus_type=%u lanes=%u clock=%u port=%u\n",
		 vep.bus_type, sensor->mipi_lanes, sensor->xclk_freq,
		 sensor->mipi_port);

	fwnode_handle_put(ep);
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

	sensor->fmt.width = SC200PC_WIDTH;
	sensor->fmt.height = SC200PC_HEIGHT;
	sensor->fmt.code = MEDIA_BUS_FMT_SBGGR10_1X10;
	sensor->fmt.field = V4L2_FIELD_NONE;
	sensor->fmt.colorspace = V4L2_COLORSPACE_RAW;

	sensor->cur_vts = SC200PC_VTS_DEF;

	ret = v4l2_ctrl_handler_init(&sensor->ctrls, 9);
	if (ret)
		goto err_entity_cleanup;

	sensor->link_freq = v4l2_ctrl_new_int_menu(&sensor->ctrls, NULL,
						   V4L2_CID_LINK_FREQ,
						   ARRAY_SIZE(sc200pc_link_freqs) - 1,
						   0, sc200pc_link_freqs);
	if (sensor->link_freq)
		sensor->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	sensor->pixel_rate = v4l2_ctrl_new_std(&sensor->ctrls, &sc200pc_ctrl_ops,
					       V4L2_CID_PIXEL_RATE,
					       SC200PC_PIXEL_RATE_DEFAULT,
					       SC200PC_PIXEL_RATE_DEFAULT,
					       1, SC200PC_PIXEL_RATE_DEFAULT);
	if (sensor->pixel_rate)
		sensor->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	/*
	 * VBLANK — writable so the HAL can adjust frame duration.
	 * The HAL computes fll = VBLANK + height, then writes VTS.
	 */
	sensor->vblank = v4l2_ctrl_new_std(&sensor->ctrls, &sc200pc_ctrl_ops,
					   V4L2_CID_VBLANK,
					   SC200PC_VTS_MIN - SC200PC_HEIGHT,
					   SC200PC_VTS_MAX - SC200PC_HEIGHT,
					   1, SC200PC_VBLANK_DEF);

	/*
	 * HBLANK — writable so SetControl() from the HAL succeeds, but
	 * the hardware HTS is fixed by the init table (the sensor uses
	 * internal column-parallel readout, so the real line length in
	 * internal clocks cannot be changed without re-programming the
	 * PLL). See the SC200PC_INTERNAL_PARALLELISM comment above.
	 */
	sensor->hblank = v4l2_ctrl_new_std(&sensor->ctrls, &sc200pc_ctrl_ops,
					   V4L2_CID_HBLANK,
					   SC200PC_HBLANK_DEF,
					   SC200PC_HBLANK_DEF,
					   1, SC200PC_HBLANK_DEF);

	sensor->exposure = v4l2_ctrl_new_std(&sensor->ctrls, &sc200pc_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     SC200PC_EXPOSURE_MIN,
					     SC200PC_EXPOSURE_MAX,
					     1, SC200PC_EXPOSURE_DEFAULT);

	sensor->analogue_gain = v4l2_ctrl_new_std(&sensor->ctrls, &sc200pc_ctrl_ops,
						  V4L2_CID_ANALOGUE_GAIN,
						  SC200PC_ANALOGUE_GAIN_MIN,
						  SC200PC_ANALOGUE_GAIN_MAX,
						  1, SC200PC_ANALOGUE_GAIN_DEFAULT);

	sensor->digital_gain = v4l2_ctrl_new_std(&sensor->ctrls, &sc200pc_ctrl_ops,
						 V4L2_CID_DIGITAL_GAIN,
						 SC200PC_DIGITAL_GAIN_MIN,
						 SC200PC_DIGITAL_GAIN_MAX,
						 1, SC200PC_DIGITAL_GAIN_DEFAULT);

	sensor->test_pattern =
		v4l2_ctrl_new_std_menu_items(&sensor->ctrls, &sc200pc_ctrl_ops,
					     V4L2_CID_TEST_PATTERN,
					     ARRAY_SIZE(sc200pc_test_pattern_menu) - 1,
					     0, 0, sc200pc_test_pattern_menu);

	if (sensor->ctrls.error) {
		ret = sensor->ctrls.error;
		dev_err(dev, "failed to create v4l2 controls: %d\n", ret);
		goto err_ctrls_free;
	}

	sensor->sd.ctrl_handler = &sensor->ctrls;

	ret = sc200pc_power_on(sensor);
	if (ret)
		goto err_ctrls_free;

	ret = sc200pc_identify(sensor);
	sc200pc_power_off(sensor);
	if (ret)
		goto err_ctrls_free;

	ret = v4l2_async_register_subdev_sensor(&sensor->sd);
	if (ret) {
		dev_err(dev, "failed to register async sensor subdev: %d\n", ret);
		goto err_ctrls_free;
	}

	i2c_set_clientdata(client, sensor);
	dev_info(dev,
		 "SC200PC bound at 0x%02x, chip 0x%04x rev 0x%02x, lanes=%u, link_freq=%llu\n",
		 client->addr, sensor->chip_id, sensor->chip_rev,
		 sensor->mipi_lanes, sc200pc_link_freqs[0]);
	return 0;

err_ctrls_free:
	v4l2_ctrl_handler_free(&sensor->ctrls);
err_entity_cleanup:
	media_entity_cleanup(&sensor->sd.entity);
	return ret;
}

static void sc200pc_remove(struct i2c_client *client)
{
	struct sc200pc *sensor = i2c_get_clientdata(client);

	v4l2_async_unregister_subdev(&sensor->sd);
	v4l2_ctrl_handler_free(&sensor->ctrls);
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
	.probe = sc200pc_probe,
	.remove = sc200pc_remove,
};

module_i2c_driver(sc200pc_i2c_driver);

MODULE_DESCRIPTION("V4L2 driver for Samsung/Intel SC200PC (ACPI SSLC2000)");
MODULE_LICENSE("GPL");
