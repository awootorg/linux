// SPDX-License-Identifier: GPL-2.0+
// Copyright IBM Corp 2019
/*
 * The DPS310 is a barometric pressure and temperature sensor.
 * Currently only reading a single temperature is supported by
 * this driver.
 *
 * https://www.infineon.com/dgdl/?fileId=5546d462576f34750157750826c42242
 *
 * Temperature calculation:
 *   c0 * 0.5 + c1 * T_raw / kT °C
 *
 * TODO:
 *  - Optionally support the FIFO
 */

#include <linux/i2c.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/regmap.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#define DPS310_DEV_NAME		"dps310"

#define DPS310_PRS_B0		0x00
#define DPS310_PRS_B1		0x01
#define DPS310_PRS_B2		0x02
#define DPS310_TMP_B0		0x03
#define DPS310_TMP_B1		0x04
#define DPS310_TMP_B2		0x05
#define DPS310_PRS_CFG		0x06
#define  DPS310_PRS_RATE_BITS	GENMASK(6, 4)
#define  DPS310_PRS_PRC_BITS	GENMASK(3, 0)
#define DPS310_TMP_CFG		0x07
#define  DPS310_TMP_RATE_BITS	GENMASK(6, 4)
#define  DPS310_TMP_PRC_BITS	GENMASK(3, 0)
#define  DPS310_TMP_EXT		BIT(7)
#define DPS310_MEAS_CFG		0x08
#define  DPS310_MEAS_CTRL_BITS	GENMASK(2, 0)
#define   DPS310_PRS_EN		BIT(0)
#define   DPS310_TEMP_EN	BIT(1)
#define   DPS310_BACKGROUND	BIT(2)
#define  DPS310_PRS_RDY		BIT(4)
#define  DPS310_TMP_RDY		BIT(5)
#define  DPS310_SENSOR_RDY	BIT(6)
#define  DPS310_COEF_RDY	BIT(7)
#define DPS310_CFG_REG		0x09
#define  DPS310_INT_HL		BIT(7)
#define  DPS310_TMP_SHIFT_EN	BIT(3)
#define  DPS310_PRS_SHIFT_EN	BIT(4)
#define  DPS310_FIFO_EN		BIT(5)
#define  DPS310_SPI_EN		BIT(6)
#define DPS310_RESET		0x0c
#define  DPS310_RESET_MAGIC	0x09
#define DPS310_COEF_BASE	0x10

/* Make sure sleep time is <= 30ms for usleep_range */
#define DPS310_POLL_SLEEP_US(t)		min(30000, (t) / 8)
/* Silently handle error in rate value here */
#define DPS310_POLL_TIMEOUT_US(rc)	((rc) <= 0 ? 1000000 : 1000000 / (rc))

#define DPS310_PRS_BASE		DPS310_PRS_B0
#define DPS310_TMP_BASE		DPS310_TMP_B0

/*
 * These values (defined in the spec) indicate how to scale the raw register
 * values for each level of precision available.
 */
static const int scale_factors[] = {
	 524288,
	1572864,
	3670016,
	7864320,
	 253952,
	 516096,
	1040384,
	2088960,
};

struct dps310_data {
	struct i2c_client *client;
	struct regmap *regmap;
	struct mutex lock;	/* Lock for sequential HW access functions */

	s32 c0, c1;
	s32 c00, c10, c20, c30, c01, c11, c21;
	s32 pressure_raw;
	s32 temp_raw;
	bool timeout_recovery_failed;
};

static const struct iio_chan_spec dps310_channels[] = {
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_OVERSAMPLING_RATIO) |
			BIT(IIO_CHAN_INFO_SAMP_FREQ) |
			BIT(IIO_CHAN_INFO_PROCESSED),
	},
	{
		.type = IIO_PRESSURE,
		.info_mask_separate = BIT(IIO_CHAN_INFO_OVERSAMPLING_RATIO) |
			BIT(IIO_CHAN_INFO_SAMP_FREQ) |
			BIT(IIO_CHAN_INFO_PROCESSED),
	},
};

/* To be called after checking the COEF_RDY bit in MEAS_CFG */
static int dps310_get_coefs(struct dps310_data *data)
{
	int rc;
	u8 coef[18];
	u32 c0, c1;
	u32 c00, c10, c20, c30, c01, c11, c21;

	/* Read all sensor calibration coefficients from the COEF registers. */
	rc = regmap_bulk_read(data->regmap, DPS310_COEF_BASE, coef,
			      sizeof(coef));
	if (rc < 0)
		return rc;

	/*
	 * Calculate temperature calibration coefficients c0 and c1. The
	 * numbers are 12-bit 2's complement numbers.
	 */
	c0 = (coef[0] << 4) | (coef[1] >> 4);
	data->c0 = sign_extend32(c0, 11);

	c1 = ((coef[1] & GENMASK(3, 0)) << 8) | coef[2];
	data->c1 = sign_extend32(c1, 11);

	/*
	 * Calculate pressure calibration coefficients. c00 and c10 are 20 bit
	 * 2's complement numbers, while the rest are 16 bit 2's complement
	 * numbers.
	 */
	c00 = (coef[3] << 12) | (coef[4] << 4) | (coef[5] >> 4);
	data->c00 = sign_extend32(c00, 19);

	c10 = ((coef[5] & GENMASK(3, 0)) << 16) | (coef[6] << 8) | coef[7];
	data->c10 = sign_extend32(c10, 19);

	c01 = (coef[8] << 8) | coef[9];
	data->c01 = sign_extend32(c01, 15);

	c11 = (coef[10] << 8) | coef[11];
	data->c11 = sign_extend32(c11, 15);

	c20 = (coef[12] << 8) | coef[13];
	data->c20 = sign_extend32(c20, 15);

	c21 = (coef[14] << 8) | coef[15];
	data->c21 = sign_extend32(c21, 15);

	c30 = (coef[16] << 8) | coef[17];
	data->c30 = sign_extend32(c30, 15);

	return 0;
}

/*
 * Some versions of the chip will read temperatures in the ~60C range when
 * it's actually ~20C. This is the manufacturer recommended workaround
 * to correct the issue. The registers used below are undocumented.
 */
static int dps310_temp_workaround(struct dps310_data *data)
{
	int rc;
	int reg;

	rc = regmap_read(data->regmap, 0x32, &reg);
	if (rc < 0)
		return rc;

	/*
	 * If bit 1 is set then the device is okay, and the workaround does not
	 * need to be applied
	 */
	if (reg & BIT(1))
		return 0;

	rc = regmap_write(data->regmap, 0x0e, 0xA5);
	if (rc)
		return rc;

	rc = regmap_write(data->regmap, 0x0f, 0x96);
	if (rc)
		return rc;

	rc = regmap_write(data->regmap, 0x62, 0x02);
	if (rc)
		return rc;

	rc = regmap_write(data->regmap, 0x0e, 0x00);
	if (rc)
		return rc;

	return regmap_write(data->regmap, 0x0f, 0x00);
}

static int dps310_startup(struct dps310_data *data)
{
	int rc;
	int ready;

	/*
	 * Set up pressure sensor in single sample, one measurement per second
	 * mode
	 */
	rc = regmap_write(data->regmap, DPS310_PRS_CFG, 0);
	if (rc)
		return rc;

	/*
	 * Set up external (MEMS) temperature sensor in single sample, one
	 * measurement per second mode
	 */
	rc = regmap_write(data->regmap, DPS310_TMP_CFG, DPS310_TMP_EXT);
	if (rc)
		return rc;

	/* Temp and pressure shifts are disabled when PRC <= 8 */
	rc = regmap_write_bits(data->regmap, DPS310_CFG_REG,
			       DPS310_PRS_SHIFT_EN | DPS310_TMP_SHIFT_EN, 0);
	if (rc)
		return rc;

	/* MEAS_CFG doesn't update correctly unless first written with 0 */
	rc = regmap_write_bits(data->regmap, DPS310_MEAS_CFG,
			       DPS310_MEAS_CTRL_BITS, 0);
	if (rc)
		return rc;

	/* Turn on temperature and pressure measurement in the background */
	rc = regmap_write_bits(data->regmap, DPS310_MEAS_CFG,
			       DPS310_MEAS_CTRL_BITS, DPS310_PRS_EN |
			       DPS310_TEMP_EN | DPS310_BACKGROUND);
	if (rc)
		return rc;

	/*
	 * Calibration coefficients required for reporting temperature.
	 * They are available 40ms after the device has started
	 */
	rc = regmap_read_poll_timeout(data->regmap, DPS310_MEAS_CFG, ready,
				      ready & DPS310_COEF_RDY, 10000, 40000);
	if (rc)
		return rc;

	rc = dps310_get_coefs(data);
	if (rc)
		return rc;

	return dps310_temp_workaround(data);
}

static int dps310_get_pres_precision(struct dps310_data *data, int *val)
{
	int reg_val, rc;

	rc = regmap_read(data->regmap, DPS310_PRS_CFG, &reg_val);
	if (rc < 0)
		return rc;

	*val = BIT(reg_val & GENMASK(2, 0));

	return 0;
}

static int dps310_get_temp_precision(struct dps310_data *data, int *val)
{
	int reg_val, rc;

	rc = regmap_read(data->regmap, DPS310_TMP_CFG, &reg_val);
	if (rc < 0)
		return rc;

	/*
	 * Scale factor is bottom 4 bits of the register, but 1111 is
	 * reserved so just grab bottom three
	 */
	*val = BIT(reg_val & GENMASK(2, 0));

	return 0;
}

/* Called with lock held */
static int dps310_set_pres_precision(struct dps310_data *data, int val)
{
	int rc;
	u8 shift_en;

	if (val < 0 || val > 128)
		return -EINVAL;

	shift_en = val >= 16 ? DPS310_PRS_SHIFT_EN : 0;
	rc = regmap_write_bits(data->regmap, DPS310_CFG_REG,
			       DPS310_PRS_SHIFT_EN, shift_en);
	if (rc)
		return rc;

	return regmap_update_bits(data->regmap, DPS310_PRS_CFG,
				  DPS310_PRS_PRC_BITS, ilog2(val));
}

/* Called with lock held */
static int dps310_set_temp_precision(struct dps310_data *data, int val)
{
	int rc;
	u8 shift_en;

	if (val < 0 || val > 128)
		return -EINVAL;

	shift_en = val >= 16 ? DPS310_TMP_SHIFT_EN : 0;
	rc = regmap_write_bits(data->regmap, DPS310_CFG_REG,
			       DPS310_TMP_SHIFT_EN, shift_en);
	if (rc)
		return rc;

	return regmap_update_bits(data->regmap, DPS310_TMP_CFG,
				  DPS310_TMP_PRC_BITS, ilog2(val));
}

/* Called with lock held */
static int dps310_set_pres_samp_freq(struct dps310_data *data, int freq)
{
	u8 val;

	if (freq < 0 || freq > 128)
		return -EINVAL;

	val = ilog2(freq) << 4;

	return regmap_update_bits(data->regmap, DPS310_PRS_CFG,
				  DPS310_PRS_RATE_BITS, val);
}

/* Called with lock held */
static int dps310_set_temp_samp_freq(struct dps310_data *data, int freq)
{
	u8 val;

	if (freq < 0 || freq > 128)
		return -EINVAL;

	val = ilog2(freq) << 4;

	return regmap_update_bits(data->regmap, DPS310_TMP_CFG,
				  DPS310_TMP_RATE_BITS, val);
}

static int dps310_get_pres_samp_freq(struct dps310_data *data, int *val)
{
	int reg_val, rc;

	rc = regmap_read(data->regmap, DPS310_PRS_CFG, &reg_val);
	if (rc < 0)
		return rc;

	*val = BIT((reg_val & DPS310_PRS_RATE_BITS) >> 4);

	return 0;
}

static int dps310_get_temp_samp_freq(struct dps310_data *data, int *val)
{
	int reg_val, rc;

	rc = regmap_read(data->regmap, DPS310_TMP_CFG, &reg_val);
	if (rc < 0)
		return rc;

	*val = BIT((reg_val & DPS310_TMP_RATE_BITS) >> 4);

	return 0;
}

static int dps310_get_pres_k(struct dps310_data *data, int *val)
{
	int reg_val, rc;

	rc = regmap_read(data->regmap, DPS310_PRS_CFG, &reg_val);
	if (rc < 0)
		return rc;

	*val = scale_factors[reg_val & GENMASK(2, 0)];

	return 0;
}

static int dps310_get_temp_k(struct dps310_data *data, int *val)
{
	int reg_val, rc;

	rc = regmap_read(data->regmap, DPS310_TMP_CFG, &reg_val);
	if (rc < 0)
		return rc;

	*val = scale_factors[reg_val & GENMASK(2, 0)];

	return 0;
}

static int dps310_reset_wait(struct dps310_data *data)
{
	int rc;

	rc = regmap_write(data->regmap, DPS310_RESET, DPS310_RESET_MAGIC);
	if (rc)
		return rc;

	/* Wait for device chip access: 15ms in specification */
	usleep_range(15000, 55000);
	return 0;
}

static int dps310_reset_reinit(struct dps310_data *data)
{
	int rc;

	rc = dps310_reset_wait(data);
	if (rc)
		return rc;

	return dps310_startup(data);
}

static int dps310_ready_status(struct dps310_data *data, int ready_bit, int timeout)
{
	int sleep = DPS310_POLL_SLEEP_US(timeout);
	int ready;

	return regmap_read_poll_timeout(data->regmap, DPS310_MEAS_CFG, ready, ready & ready_bit,
					sleep, timeout);
}

static int dps310_ready(struct dps310_data *data, int ready_bit, int timeout)
{
	int rc;

	rc = dps310_ready_status(data, ready_bit, timeout);
	if (rc) {
		if (rc == -ETIMEDOUT && !data->timeout_recovery_failed) {
			/* Reset and reinitialize the chip. */
			if (dps310_reset_reinit(data)) {
				data->timeout_recovery_failed = true;
			} else {
				/* Try again to get sensor ready status. */
				if (dps310_ready_status(data, ready_bit, timeout))
					data->timeout_recovery_failed = true;
				else
					return 0;
			}
		}

		return rc;
	}

	data->timeout_recovery_failed = false;
	return 0;
}

static int dps310_read_pres_raw(struct dps310_data *data)
{
	int rc;
	int rate;
	int timeout;
	s32 raw;
	u8 val[3];

	if (mutex_lock_interruptible(&data->lock))
		return -EINTR;

	rc = dps310_get_pres_samp_freq(data, &rate);
	if (rc)
		goto done;

	timeout = DPS310_POLL_TIMEOUT_US(rate);

	/* Poll for sensor readiness; base the timeout upon the sample rate. */
	rc = dps310_ready(data, DPS310_PRS_RDY, timeout);
	if (rc)
		goto done;

	rc = regmap_bulk_read(data->regmap, DPS310_PRS_BASE, val, sizeof(val));
	if (rc < 0)
		goto done;

	raw = (val[0] << 16) | (val[1] << 8) | val[2];
	data->pressure_raw = sign_extend32(raw, 23);

done:
	mutex_unlock(&data->lock);
	return rc;
}

/* Called with lock held */
static int dps310_read_temp_ready(struct dps310_data *data)
{
	int rc;
	u8 val[3];
	s32 raw;

	rc = regmap_bulk_read(data->regmap, DPS310_TMP_BASE, val, sizeof(val));
	if (rc < 0)
		return rc;

	raw = (val[0] << 16) | (val[1] << 8) | val[2];
	data->temp_raw = sign_extend32(raw, 23);

	return 0;
}

static int dps310_read_temp_raw(struct dps310_data *data)
{
	int rc;
	int rate;
	int timeout;

	if (mutex_lock_interruptible(&data->lock))
		return -EINTR;

	rc = dps310_get_temp_samp_freq(data, &rate);
	if (rc)
		goto done;

	timeout = DPS310_POLL_TIMEOUT_US(rate);

	/* Poll for sensor readiness; base the timeout upon the sample rate. */
	rc = dps310_ready(data, DPS310_TMP_RDY, timeout);
	if (rc)
		goto done;

	rc = dps310_read_temp_ready(data);

done:
	mutex_unlock(&data->lock);
	return rc;
}

static bool dps310_is_writeable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case DPS310_PRS_CFG:
	case DPS310_TMP_CFG:
	case DPS310_MEAS_CFG:
	case DPS310_CFG_REG:
	case DPS310_RESET:
	/* No documentation available on the registers below */
	case 0x0e:
	case 0x0f:
	case 0x62:
		return true;
	default:
		return false;
	}
}

static bool dps310_is_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case DPS310_PRS_B0:
	case DPS310_PRS_B1:
	case DPS310_PRS_B2:
	case DPS310_TMP_B0:
	case DPS310_TMP_B1:
	case DPS310_TMP_B2:
	case DPS310_MEAS_CFG:
	case 0x32:	/* No documentation available on this register */
		return true;
	default:
		return false;
	}
}

static int dps310_write_raw(struct iio_dev *iio,
			    struct iio_chan_spec const *chan, int val,
			    int val2, long mask)
{
	int rc;
	struct dps310_data *data = iio_priv(iio);

	if (mutex_lock_interruptible(&data->lock))
		return -EINTR;

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		switch (chan->type) {
		case IIO_PRESSURE:
			rc = dps310_set_pres_samp_freq(data, val);
			break;

		case IIO_TEMP:
			rc = dps310_set_temp_samp_freq(data, val);
			break;

		default:
			rc = -EINVAL;
			break;
		}
		break;

	case IIO_CHAN_INFO_OVERSAMPLING_RATIO:
		switch (chan->type) {
		case IIO_PRESSURE:
			rc = dps310_set_pres_precision(data, val);
			break;

		case IIO_TEMP:
			rc = dps310_set_temp_precision(data, val);
			break;

		default:
			rc = -EINVAL;
			break;
		}
		break;

	default:
		rc = -EINVAL;
		break;
	}

	mutex_unlock(&data->lock);
	return rc;
}

static int dps310_calculate_pressure(struct dps310_data *data, int *val)
{
	int i;
	int rc;
	int t_ready;
	int kpi;
	int kti;
	s64 rem = 0ULL;
	s64 pressure = 0ULL;
	s64 p;
	s64 t;
	s64 denoms[7];
	s64 nums[7];
	s64 rems[7];
	s64 kp;
	s64 kt;

	rc = dps310_get_pres_k(data, &kpi);
	if (rc)
		return rc;

	rc = dps310_get_temp_k(data, &kti);
	if (rc)
		return rc;

	kp = (s64)kpi;
	kt = (s64)kti;

	/* Refresh temp if it's ready, otherwise just use the latest value */
	if (mutex_trylock(&data->lock)) {
		rc = regmap_read(data->regmap, DPS310_MEAS_CFG, &t_ready);
		if (rc >= 0 && t_ready & DPS310_TMP_RDY)
			dps310_read_temp_ready(data);

		mutex_unlock(&data->lock);
	}

	p = (s64)data->pressure_raw;
	t = (s64)data->temp_raw;

	/* Section 4.9.1 of the DPS310 spec; algebra'd to avoid underflow */
	nums[0] = (s64)data->c00;
	denoms[0] = 1LL;
	nums[1] = p * (s64)data->c10;
	denoms[1] = kp;
	nums[2] = p * p * (s64)data->c20;
	denoms[2] = kp * kp;
	nums[3] = p * p * p * (s64)data->c30;
	denoms[3] = kp * kp * kp;
	nums[4] = t * (s64)data->c01;
	denoms[4] = kt;
	nums[5] = t * p * (s64)data->c11;
	denoms[5] = kp * kt;
	nums[6] = t * p * p * (s64)data->c21;
	denoms[6] = kp * kp * kt;

	/* Kernel lacks a div64_s64_rem function; denoms are all positive */
	for (i = 0; i < 7; ++i) {
		u64 irem;

		if (nums[i] < 0LL) {
			pressure -= div64_u64_rem(-nums[i], denoms[i], &irem);
			rems[i] = -irem;
		} else {
			pressure += div64_u64_rem(nums[i], denoms[i], &irem);
			rems[i] = (s64)irem;
		}
	}

	/* Increase precision and calculate the remainder sum */
	for (i = 0; i < 7; ++i)
		rem += div64_s64((s64)rems[i] * 1000000000LL, denoms[i]);

	pressure += div_s64(rem, 1000000000LL);
	if (pressure < 0LL)
		return -ERANGE;

	*val = (int)min_t(s64, pressure, INT_MAX);

	return 0;
}

static int dps310_read_pressure(struct dps310_data *data, int *val, int *val2,
				long mask)
{
	int rc;

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		rc = dps310_get_pres_samp_freq(data, val);
		if (rc)
			return rc;

		return IIO_VAL_INT;

	case IIO_CHAN_INFO_PROCESSED:
		rc = dps310_read_pres_raw(data);
		if (rc)
			return rc;

		rc = dps310_calculate_pressure(data, val);
		if (rc)
			return rc;

		*val2 = 1000; /* Convert Pa to KPa per IIO ABI */
		return IIO_VAL_FRACTIONAL;

	case IIO_CHAN_INFO_OVERSAMPLING_RATIO:
		rc = dps310_get_pres_precision(data, val);
		if (rc)
			return rc;
		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}
}

static int dps310_calculate_temp(struct dps310_data *data, int *val)
{
	s64 c0;
	s64 t;
	int kt, rc;

	rc = dps310_get_temp_k(data, &kt);
	if (rc)
		return rc;

	/* Obtain inverse-scaled offset */
	c0 = div_s64((s64)kt * (s64)data->c0, 2);

	/* Add the offset to the unscaled temperature */
	t = c0 + ((s64)data->temp_raw * (s64)data->c1);

	/* Convert to milliCelsius and scale the temperature */
	*val = (int)div_s64(t * 1000LL, kt);

	return 0;
}

static int dps310_read_temp(struct dps310_data *data, int *val, int *val2,
			    long mask)
{
	int rc;

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		rc = dps310_get_temp_samp_freq(data, val);
		if (rc)
			return rc;

		return IIO_VAL_INT;

	case IIO_CHAN_INFO_PROCESSED:
		rc = dps310_read_temp_raw(data);
		if (rc)
			return rc;

		rc = dps310_calculate_temp(data, val);
		if (rc)
			return rc;

		return IIO_VAL_INT;

	case IIO_CHAN_INFO_OVERSAMPLING_RATIO:
		rc = dps310_get_temp_precision(data, val);
		if (rc)
			return rc;

		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}
}

static int dps310_read_raw(struct iio_dev *iio,
			   struct iio_chan_spec const *chan,
			   int *val, int *val2, long mask)
{
	struct dps310_data *data = iio_priv(iio);

	switch (chan->type) {
	case IIO_PRESSURE:
		return dps310_read_pressure(data, val, val2, mask);

	case IIO_TEMP:
		return dps310_read_temp(data, val, val2, mask);

	default:
		return -EINVAL;
	}
}

static void dps310_reset(void *action_data)
{
	struct dps310_data *data = action_data;

	dps310_reset_wait(data);
}

static const struct regmap_config dps310_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.writeable_reg = dps310_is_writeable_reg,
	.volatile_reg = dps310_is_volatile_reg,
	.cache_type = REGCACHE_RBTREE,
	.max_register = 0x62, /* No documentation available on this register */
};

static const struct iio_info dps310_info = {
	.read_raw = dps310_read_raw,
	.write_raw = dps310_write_raw,
};

static int dps310_probe(struct i2c_client *client)
{
	const struct i2c_device_id *id = i2c_client_get_device_id(client);
	struct dps310_data *data;
	struct iio_dev *iio;
	int rc;

	iio = devm_iio_device_alloc(&client->dev,  sizeof(*data));
	if (!iio)
		return -ENOMEM;

	data = iio_priv(iio);
	data->client = client;
	mutex_init(&data->lock);

	iio->name = id->name;
	iio->channels = dps310_channels;
	iio->num_channels = ARRAY_SIZE(dps310_channels);
	iio->info = &dps310_info;
	iio->modes = INDIO_DIRECT_MODE;

	data->regmap = devm_regmap_init_i2c(client, &dps310_regmap_config);
	if (IS_ERR(data->regmap))
		return PTR_ERR(data->regmap);

	/* Register to run the device reset when the device is removed */
	rc = devm_add_action_or_reset(&client->dev, dps310_reset, data);
	if (rc)
		return rc;

	rc = dps310_startup(data);
	if (rc)
		return rc;

	rc = devm_iio_device_register(&client->dev, iio);
	if (rc)
		return rc;

	i2c_set_clientdata(client, iio);

	return 0;
}

static const struct i2c_device_id dps310_id[] = {
	{ DPS310_DEV_NAME },
	{ }
};
MODULE_DEVICE_TABLE(i2c, dps310_id);

static const struct acpi_device_id dps310_acpi_match[] = {
	{ "IFX3100" },
	{ }
};
MODULE_DEVICE_TABLE(acpi, dps310_acpi_match);

static struct i2c_driver dps310_driver = {
	.driver = {
		.name = DPS310_DEV_NAME,
		.acpi_match_table = dps310_acpi_match,
	},
	.probe = dps310_probe,
	.id_table = dps310_id,
};
module_i2c_driver(dps310_driver);

MODULE_AUTHOR("Joel Stanley <joel@jms.id.au>");
MODULE_DESCRIPTION("Infineon DPS310 pressure and temperature sensor");
MODULE_LICENSE("GPL v2");
