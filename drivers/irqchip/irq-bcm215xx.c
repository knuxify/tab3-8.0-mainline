// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  INTC (Interrupt controller) driver for BCM215xx SoCs
 *
 *  Copyright (C) 2026 Artur Weber <aweber.kernel@gmail.com>
 */

#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

#define INTC_IMR		0x00
#define INTC_ISR		0x04
#define INTC_ICR		0x08
#define INTC_IMSR		0x0c
#define INTC_ISTCR(x)		(0x10 + (x >> 3) * 4)
#define INTC_ICPR		0x20
#define INTC_ICCR		0x24
#define INTC_ISELR		0x28
#define INTC_SWIR(x)		(x < 64 ? 0x30 : 0x24)
#define INTC_SICR(x)		(x < 64 ? 0x34 : 0x28)
#define INTC_ARM9_SLEEP		0x38

/*
 * The 96 interrupts handled by the interrupt controller are split into
 * three blocks starting at 0x00, 0x100 and 0x180 respectively.
 */
#define NUM_IRQ_BLOCKS			3
#define NUM_IRQS_IN_BLOCK		32
#define INTC_HWIRQ_TO_BLOCK(irq)	(irq / NUM_IRQS_IN_BLOCK)

struct bcm215xx_intc {
	void __iomem *base;
	struct irq_domain *domain;
};

static struct bcm215xx_intc intc;

static inline u32 _intc_hwirq_to_offset(int irq)
{
	switch (INTC_HWIRQ_TO_BLOCK(irq)) {
	case 0:
		return 0x00;
	case 1:
		return 0x100;
	case 2:
		return 0x180;
	default:
		WARN_ON(1);
	}

	return 0;
}

static void bcm215xx_intc_handle_irq(struct pt_regs *regs)
{
	u32 isr;
	int i, hwirq;

	for (i = 0; i < NUM_IRQ_BLOCKS; i++) {
		isr = readl(intc.base + (i * 0x100) + INTC_IMSR);
		while (isr) {
			hwirq = ffs(isr) - 1;
			generic_handle_domain_irq(intc.domain, hwirq + i * 32);
			isr &= ~(1 << hwirq);
		}
	}
}

static void bcm215xx_intc_mask_irq(struct irq_data *d)
{
	void __iomem *base = intc.base + _intc_hwirq_to_offset(d->hwirq);
	u32 val;

	val = readl(base + INTC_IMR);
	val &= ~(1 << (d->hwirq & (NUM_IRQS_IN_BLOCK - 1)));

	writel(val, base + INTC_IMR);
}

static void bcm215xx_intc_unmask_irq(struct irq_data *d)
{
	void __iomem *base = intc.base + _intc_hwirq_to_offset(d->hwirq);
	u32 val;

	val = readl(base + INTC_IMR);
	val |= (1 << (d->hwirq & (NUM_IRQS_IN_BLOCK - 1)));

	writel(val, base + INTC_IMR);
}

static void bcm215xx_intc_ack_irq(struct irq_data *d)
{
	void __iomem *base = intc.base + _intc_hwirq_to_offset(d->hwirq);
	unsigned int irq = d->hwirq;

	writel(1 << (irq & (NUM_IRQS_IN_BLOCK - 1)), base + INTC_ICR);

	/* Clear soft-triggered */
	writel(1 << (irq & (NUM_IRQS_IN_BLOCK - 1)), base + INTC_SICR(irq));
}

static int bcm215xx_intc_set_type(struct irq_data *d, unsigned int flow_type)
{
	void __iomem *base = intc.base + _intc_hwirq_to_offset(d->hwirq);
	unsigned int type;
	u32 val;

	switch (flow_type) {
	case IRQ_TYPE_LEVEL_HIGH:
		type = 0x00;
		break;
	case IRQ_TYPE_LEVEL_LOW:
		type = 0x01;
		break;
	case IRQ_TYPE_EDGE_RISING:
		type = 0x04;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		type = 0x05;
		break;
	case IRQ_TYPE_EDGE_BOTH:
		type = 0x06;
		break;
	default:
		return -EINVAL;
	}

	val = readl(base + INTC_ISTCR(d->hwirq));
	type &= 0x7;
	val &= ~(0x7 << ((d->hwirq & 0x7) << 2));
	val |= type << ((d->hwirq & 0x7) << 2);
	writel(val, base + INTC_ISTCR(d->hwirq));

	switch (flow_type) {
	case IRQ_TYPE_LEVEL_HIGH:
	case IRQ_TYPE_LEVEL_LOW:
		irq_set_handler_locked(d, handle_level_irq);
		break;
	case IRQ_TYPE_EDGE_RISING:
	case IRQ_TYPE_EDGE_FALLING:
	case IRQ_TYPE_EDGE_BOTH:
		irq_set_handler_locked(d, handle_edge_irq);
		break;
	}

	return 0;
}

static struct irq_chip bcm215xx_intc_chip = {
	.name = "BCM215xx INTC",
	.irq_mask = bcm215xx_intc_mask_irq,
	.irq_unmask = bcm215xx_intc_unmask_irq,
	.irq_ack = bcm215xx_intc_ack_irq,
	.irq_set_type = bcm215xx_intc_set_type,
	//.irq_set_wake = bcm215xx_intc_set_wake,
};

static int bcm215xx_intc_domain_map(struct irq_domain *d, unsigned int irq,
				 irq_hw_number_t hwirq)
{
	u32 val;

	/* Default to level-high (0x00) in ISTCR */
	val = readl(intc.base + INTC_ISTCR(hwirq & 31));
	val &= ~(0x7 << ((hwirq & 0x7) << 2));
	writel(val, intc.base + INTC_ISTCR(hwirq & 31));

	irq_domain_set_info(d, irq, hwirq, &bcm215xx_intc_chip, NULL,
			    handle_level_irq, NULL, NULL);

	return 0;
}

static const struct irq_domain_ops bcm215xx_intc_domain_ops = {
	.map	= bcm215xx_intc_domain_map,
	.xlate	= irq_domain_xlate_twocell,
};

#ifdef CONFIG_OF
static int __init bcm215xx_intc_of_init(struct device_node *node,
			      struct device_node *parent)
{
	void *block_base;
	int i;

	intc.base = of_iomap(node, 0);
	if (WARN_ON(!intc.base))
		return -ENOMEM;

	/* Clear and disable all interrupts by default */
	for (i = 0; i < NUM_IRQ_BLOCKS * NUM_IRQS_IN_BLOCK; i += NUM_IRQS_IN_BLOCK) {
		block_base = intc.base + _intc_hwirq_to_offset(i);
		writel(0, block_base + INTC_IMR);
		writel(~0, block_base + INTC_ICR);
		writel(0, block_base + INTC_ISR);
		writel(0, block_base + INTC_IMSR);
		writel(0, block_base + INTC_ISELR);
	}

	intc.domain = irq_domain_create_linear(of_fwnode_handle(node), 96,
				 &bcm215xx_intc_domain_ops, NULL);
	if (!intc.domain)
		goto out_unmap;

	irq_domain_update_bus_token(intc.domain, DOMAIN_BUS_WIRED);

	set_handle_irq(bcm215xx_intc_handle_irq);

	return 0;

out_unmap:
	iounmap(intc.base);
	return -ENOMEM;
}
IRQCHIP_DECLARE(bcm215xx_intc, "brcm,bcm215xx-intc", bcm215xx_intc_of_init);
#endif /* CONFIG OF */
