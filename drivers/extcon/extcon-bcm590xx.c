// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * extcon-bcm590xx.c - Broadcom BCM590xx PMU USB extcon driver
 *
 * Copyright (c) 2025 Artur Weber <aweber.kernel@gmail.com>
 */

#include <linux/extcon-provider.h>
#include <linux/regmap.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/mfd/bcm590xx.h>

#define BCM590XX_REG_MBCCTRL5		0x44
#define BCM590XX_MBCCTRL5_CHP_TYP_MASK	0x30
#define BCM590XX_MBCCTRL5_CHP_TYP_SHIFT	4

/*
 * Model-specific data struct.
 */
struct bcm590xx_extcon_data {
	int usbins_irq;
	int usbrm_irq;
};

static const struct bcm590xx_extcon_data bcm59054_extcon_data = {
	.usbins_irq = BCM59054_IRQ_USBINS,
	.usbrm_irq = BCM59054_IRQ_USBRM,
};

static const struct bcm590xx_extcon_data bcm59056_extcon_data = {
	.usbins_irq = BCM59056_IRQ_USBINS,
	.usbrm_irq = BCM59056_IRQ_USBRM,
};

struct bcm590xx_extcon {
	struct extcon_dev *edev;
	struct bcm590xx *mfd;
	const struct bcm590xx_extcon_data *data;

	int usbins_irq;
	int usbrm_irq;
};

static const unsigned int bcm590xx_extcon_cable[] = {
	EXTCON_USB,
	EXTCON_USB_HOST,
	EXTCON_CHG_USB_SDP,
	EXTCON_CHG_USB_DCP,
	EXTCON_CHG_USB_FAST,
	EXTCON_CHG_USB_SLOW,
	EXTCON_CHG_USB_CDP,
	EXTCON_NONE,
};

enum bcm590xx_id_code {
	BCM590XX_USB_ID_GROUND = 0,
	BCM590XX_USB_ID_DEVICE,
	BCM590XX_USB_ID_RESERVED1,
	BCM590XX_USB_ID_RID_A,
	BCM590XX_USB_ID_RID_B,
	BCM590XX_USB_ID_RID_C,
	BCM590XX_USB_ID_RESERVED2,
	BCM590XX_USB_ID_FLOAT,
};

static irqreturn_t bcm590xx_extcon_usbins_irq_handler(int irq, void *data)
{
	struct bcm590xx_extcon *extcon = data;
	unsigned int env4, id_code;
	int ret;

	ret = regmap_read(extcon->mfd->regmap_pri, BCM590XX_REG_ENV4, &env4);
	id_code = (env4 & BCM590XX_ENV4_ID_CODE_MASK)
			>> BCM590XX_ENV4_ID_CODE_SHIFT;

	// see bcmpmu_accy_detect_func in downstream

	case (id_code) {
	switch
	}

	return IRQ_HANDLED;
}

static irqreturn_t bcm590xx_extcon_usbrm_irq_handler(int irq, void *data)
{
	// TODO

	return IRQ_HANDLED;
}

static int bcm590xx_extcon_probe(struct platform_device *pdev)
{
	struct bcm590xx *bcm590xx = dev_get_drvdata(pdev->dev.parent);
	struct bcm590xx_extcon *extcon;
	int ret;

	extcon = devm_kzalloc(&pdev->dev, sizeof(struct bcm590xx_extcon),
			   GFP_KERNEL);
	if (!extcon) {
		dev_err(&pdev->dev, "Failed to allocate memory\n");
		return -ENOMEM;
	}

	dev_set_drvdata(&pdev->dev, extcon);

	extcon->mfd = bcm590xx;
	switch (bcm590xx->pmu_id) {
	case BCM590XX_PMUID_BCM59054:
		extcon->data = &bcm59054_extcon_data;
		break;
	case BCM590XX_PMUID_BCM59056:
		extcon->data = &bcm59056_extcon_data;
		break;
	default:
		return -ENODEV;
	}

	extcon->usbins_irq = bcm590xx_devm_request_irq(&pdev->dev, bcm590xx,
						 extcon->data->usbins_irq,
						 bcm590xx_extcon_usbins_irq_handler,
						 0, "extcon-sec", extcon);
	if (extcon->usbins_irq < 0) {
		dev_err(&pdev->dev, "Failed to request USB insert IRQ: %d\n",
			extcon->usbins_irq);
		return extcon->usbins_irq;
	}

	extcon->usbrm_irq = bcm590xx_devm_request_irq(&pdev->dev, bcm590xx,
						 extcon->data->usbrm_irq,
						 bcm590xx_extcon_usbrm_irq_handler,
						 0, "extcon-sec", extcon);
	if (extcon->usbrm_irq < 0) {
		dev_err(&pdev->dev, "Failed to request USB remove IRQ: %d\n",
			extcon->usbrm_irq);
		return extcon->usbrm_irq;
	}

	extcon->edev = devm_extcon_dev_allocate(&pdev->dev, bcm590xx_extcon_cable);
	if (IS_ERR(extcon->edev)) {
		dev_err(&pdev->dev, "Failed to allocate memory for extcon\n");
		return PTR_ERR(extcon->edev);
	}

	ret = devm_extcon_dev_register(&pdev->dev, extcon->edev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register extcon device\n");
		return ret;
	}

	return 0;
};

static struct platform_driver bcm590xx_extcon_driver = {
	.driver		= {
		.name	= "bcm590xx-extcon",
	},
	.probe		= bcm590xx_extcon_probe,
};

module_platform_driver(bcm590xx_extcon_driver);

MODULE_DESCRIPTION("Broadcom BCM590XX PMU RTC driver");
MODULE_AUTHOR("Artur Weber <aweber.kernel@gmail.com>");
MODULE_LICENSE("GPL");
