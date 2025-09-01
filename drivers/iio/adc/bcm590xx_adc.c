// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for the ADC found in Broadcom BCM590xx PMUs.
 *
 * Copyright (C) 2025 Artur Weber <aweber.kernel@gmail.com>
 */

#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/mfd/bcm590xx.h>
#include <linux/iio/iio.h>

#define ADCCTRL1_RTM_START		BIT(2)
#define ADCCTRL1_RTM_ENABLE		BIT(3)
#define ADCCTRL1_RTM_CHANNEL_SHIFT	4
#define ADCCTRL1_RTM_CHANNEL_MASK	(0xf << ADCCTRL1_RTM_CHANNEL_SHIFT)

/*
 * All ADC channels have 2 registers directly following each other.
 * - In the first register:
 *   DATA_MSB is upper 2 bits of ADC data,
 *   READ_INVALID indicates whether the read failed or succeeded.
 * - The second register contains the lower 8 bits of ADC data.
 */
#define BCM590XX_ADC_DATA_MSB		GENMASK(1, 0)
#define BCM590XX_ADC_READ_INVALID	BIT(2)

/*
 * Get base register for ADC channel.
 *
 * @adc_info: instance of the bcm590xx_adc_info struct;
 * @index: index of the channel starting from 0.
 */
#define BCM590XX_ADC_CHANNEL_REG(adc_info, index)	\
	(adc_info->adcctrl_base + 2 + (index * 2))

/*
 * Model-specific information.
 *
 * @adcctrl_base: Address of ADCCTRL1 register
 * @adcctrl_regmap: I2C subdev/regmap where the ADCCTRL registers are located
 *
 * @channels: IIO channel definitions for the ADC channels
 * @num_channels: Amount of ADC channels
 *
 * @rtm_channel: Channel ID of the RTM read channel
 * @rtm_done_irq: ID of the RTM read done/data ready IRQ
 */
struct bcm590xx_adc_info {
	u8 adcctrl_base;
	enum bcm590xx_regmap_type adcctrl_regmap;

	const struct iio_chan_spec channels;
	unsigned int num_channels;

	int rtm_channel;
	int rtm_done_irq;
};

/*
 * Device driver state.
 *
 * @dev: Pointer to the device struct
 * @info: Pointer to the model-specific information struct
 * @regmap: Regmap to use for ADCCTRLx registers
 *
 * @rtm_lock: Mutex for RTM read operations
 * @rtm_done_irq: IRQ ID for the RTM read done interrupt
 * @rtm_done_completion: Completion for the RTM read done interrupt
 */
struct bcm590xx_adc {
	struct device *dev;
	const struct bcm590xx_adc_info *info;
	struct regmap *regmap;

	struct mutex rtm_lock;
	int rtm_done_irq;
	struct completion rtm_done_completion;
};

#define BCM590XX_ADC_CHANNEL(_index) {		\
	.type = IIO_VOLTAGE,			\
	.indexed = 1,				\
	.channel = _index,			\
}

/** Model-specific data **/

/* BCM59054 data */

static const struct iio_chan_spec bcm59054_iio_chan[] = {
	BCM590XX_ADC_CHANNEL(BCM59054_ADC_VMBATT),
	BCM590XX_ADC_CHANNEL(BCM59054_ADC_VBBATT),
	BCM590XX_ADC_CHANNEL(BCM59054_ADC_RESERVED),
	BCM590XX_ADC_CHANNEL(BCM59054_ADC_VBUS),
	BCM590XX_ADC_CHANNEL(BCM59054_ADC_IDIN),
	BCM590XX_ADC_CHANNEL(BCM59054_ADC_NTC),
	BCM590XX_ADC_CHANNEL(BCM59054_ADC_BSI),
	BCM590XX_ADC_CHANNEL(BCM59054_ADC_BOM),
	BCM590XX_ADC_CHANNEL(BCM59054_ADC_32KTEMP),
	BCM590XX_ADC_CHANNEL(BCM59054_ADC_PATEMP),
	BCM590XX_ADC_CHANNEL(BCM59054_ADC_ALS),
	BCM590XX_ADC_CHANNEL(BCM59054_ADC_DIE_TEMP),
};

static const struct bcm590xx_adc_info bcm59054_adc_info = {
	.adcctrl_base = 0x20,
	.adcctrl_regmap = BCM590XX_REGMAP_SEC,

	.rtm_channel = BCM59054_ADC_RTM,
	.rtm_done_irq = BCM59054_IRQ_RTM_DATA_RDY,
};

/** Functions **/

/* Read the raw data for the given ADC channel. */
static int _bcm590xx_adc_raw_read(struct bcm590xx_adc *adc, int channel) {
	u8 val[2];
	int ret;

	ret = regmap_bulk_read(adc->regmap,
			       BCM590XX_ADC_CHANNEL_REG(adc->info, channel),
			       &val, 2);
	if (ret) {
		dev_err(adc->dev, "Failed to read ADC registers: %d\n", ret);
		return ret;
	}

	if (val[0] & BCM590XX_ADC_READ_INVALID) {
		dev_warn(adc->dev, "ADC reports invalid read for channel %d\n", channel);
		return -EINVAL;
	}

	/* Combine 10-bit ADC value from both registers */
	return ((val[0] & BCM590XX_ADC_DATA_MSB) << 8) | val[1];
}

/* Read the data for the given channel in RTM (Real-Time Measurement) mode. */
static int _bcm590xx_adc_rtm_read(struct bcm590xx_adc *adc, int channel) {
	int ret, result;
	unsigned int val;

	mutex_lock(&adc->rtm_lock);

	/* Set channel to read and enable/start RTM read */
	val = (channel << ADCCTRL1_RTM_CHANNEL_SHIFT) & ADCCTRL1_RTM_CHANNEL_MASK;
	val |= (ADCCTRL1_RTM_ENABLE | ADCCTRL1_RTM_START);

	ret = regmap_update_bits(adc->regmap, adc->info->adcctrl_base,
			ADCCTRL1_RTM_CHANNEL_MASK | ADCCTRL1_RTM_ENABLE | ADCCTRL1_RTM_START,
			val);

	if (ret) {
		dev_err(adc->dev, "Failed to enable RTM read mode: %d\n", ret);
		mutex_unlock(&adc->rtm_lock);
		return ret;
	}

	/* Wait for the RTM read done interrupt to fire */
	wait_for_completion(&adc->rtm_done_completion);

	/* Get the result */
	result = _bcm590xx_adc_raw_read(adc, channel);
	if (result < 0) {
		mutex_unlock(&adc->rtm_lock);
		return result;
	}

	/* Disable RTM read mode */
	ret = regmap_update_bits(adc->regmap, adc->info->adcctrl_base,
				 ADCCTRL1_RTM_ENABLE | ADCCTRL1_RTM_START, 0);

	if (ret) {
		dev_err(adc->dev, "Failed to disable RTM read mode: %d\n", ret);
		mutex_unlock(&adc->rtm_lock);
		return ret;
	}

	mutex_unlock(&adc->rtm_lock);
	return result;
}

static int bcm590xx_adc_read_raw(struct iio_dev *iio_dev,
			    struct iio_chan_spec const *chan,
			    int *val,
			    int *val2,
			    long mask)
{
	struct bcm590xx_adc *adc = iio_dev->priv;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		// TODO: Add some way to specify RTM mode.
		// For now, we just do raw reads.
		ret = _bcm590xx_adc_raw_read(adc, chan->channel);
		if (ret < 0)
			return ret;

		*val = ret;
		return IIO_VAL_INT;
	// TODO: Calibrated reads
	}

	return -EINVAL;
};

static struct iio_info bcm590xx_adc_iio_info = {
	.read_raw = bcm590xx_adc_read_raw,
};

static irqreturn_t bcm590xx_adc_rtm_done_isr(int irq, void *dev_id) {
	struct bcm590xx_adc *adc = dev_id;

	complete(&adc->rtm_done_completion);

	return IRQ_HANDLED;
}

static int bcm590xx_adc_probe(struct platform_device *pdev) {
	struct bcm590xx *bcm590xx = dev_get_drvdata(pdev->dev.parent);
	struct bcm590xx_adc *adc;
	struct iio_dev *iodev;
	int ret;

	iodev = devm_iio_device_alloc(&pdev->dev, sizeof(*adc));
	if (!iodev)
		return -ENOMEM;

	adc = iio_priv(iodev);
	adc->dev = &pdev->dev;

	switch (bcm590xx->pmu_id) {
	case BCM590XX_PMUID_BCM59054:
		adc->info = &bcm59054_adc_info;
		break;
	// TODO: case BCM590XX_PMUID_BCM59056
	default:
		return -ENODEV;
	}

	switch (adc->info->adcctrl_regmap) {
	case BCM590XX_REGMAP_PRI:
		adc->regmap = bcm590xx->regmap_pri;
		break;
	case BCM590XX_REGMAP_SEC:
		adc->regmap = bcm590xx->regmap_sec;
		break;
	default:
		return -EINVAL;
	}

	adc->rtm_done_irq = bcm590xx_devm_request_irq(&iodev->dev, bcm590xx,
			adc->info->rtm_done_irq, bcm590xx_adc_rtm_done_isr,
			0, "bcm590xx-adc", adc);
	if (adc->rtm_done_irq < 0) {
		dev_err(&iodev->dev, "Failed to request RTM read done IRQ: %d\n", adc->rtm_done_irq);
		return adc->rtm_done_irq;
	}

	platform_set_drvdata(pdev, iodev);

	init_completion(&adc->rtm_done_completion);
	mutex_init(&adc->rtm_lock);

	iodev->name = "bcm590xx-adc";
	iodev->info = &bcm590xx_adc_iio_info;
	iodev->modes = INDIO_DIRECT_MODE;
	iodev->num_channels = adc->info->num_channels;

	ret = devm_iio_device_register(&pdev->dev, iodev);
	if (ret)
		return ret;

	return 0;
}

static const struct platform_device_id bcm590xx_adc_id_table[] = {
	{ .name = "bcm590xx_adc" },
	{},
};
MODULE_DEVICE_TABLE(platform, bcm590xx_adc_id_table);

static struct platform_driver bcm590xx_adc_driver = {
	.probe = bcm590xx_adc_probe,
	.id_table = bcm590xx_adc_id_table,
	.driver = {
		.name = "bcm590xx_adc",
	},
};
module_platform_driver(bcm590xx_adc_driver);

MODULE_AUTHOR("Artur Weber <aweber.kernel@gmail.com>");
MODULE_DESCRIPTION("Broadcom BCM590xx ADC driver");
MODULE_LICENSE("GPL v2");
