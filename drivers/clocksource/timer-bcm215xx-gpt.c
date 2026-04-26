// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for the GPT (general purpose timer) found in BCM215xx SoCs.
 *
 * Artur Weber <aweber.kernel@gmail.com>, 2026
 */

#include <linux/clk.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/clockchips.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/sched_clock.h>

/* Interrupt register */
#define BCM215XX_GPT_ISR	0x00
#define   GPT_ISR(n)		BIT(n)

/* Per-channel registers */
#define BCM215XX_GPT_CSR(n)	((0x10 * n) + 0x04)
#define  GPT_CSR_EN		BIT(31)
#define  GPT_CSR_CLKSEL		BIT(30)
#define  GPT_CSR_INT_EN		BIT(27)
#define  GPT_CSR_CLKSEL1	BIT(25)
#define  GPT_CSR_TIMER_PWRON	BIT(24)
#define  GPT_CSR_INT_FLAG	BIT(0)
#define BCM215XX_GPT_RELOAD(n)	((0x10 * n) + 0x08)
#define BCM215XX_GPT_VALUE(n)	((0x10 * n) + 0x0c)

#define NUM_COUNTERS	6

enum bcm215xx_gpt_mode {
	GPT_MODE_PERIODIC,
	GPT_MODE_ONESHOT,
};

struct bcm215xx_gpt_counter {
	int id;
	struct clock_event_device evt;
	enum bcm215xx_gpt_mode mode;
};

struct bcm215xx_gpt {
	void __iomem *base;
	int irq;
	u32 rate;
	struct bcm215xx_gpt_counter counters[6];
};

static struct bcm215xx_gpt *timer;

static inline struct bcm215xx_gpt_counter *to_bcm215xx_counter(struct clock_event_device *c)
{
	return container_of(c, struct bcm215xx_gpt_counter, evt);
}

static void _gpt_start(struct bcm215xx_gpt_counter *cntr, u32 value)
{
	void *csr_addr = timer->base + BCM215XX_GPT_CSR(cntr->id);
	u32 reg;

	reg = readl_relaxed(csr_addr);
	reg |= GPT_CSR_TIMER_PWRON;
	writel_relaxed(reg, csr_addr);

	if (value != 0)
		writel_relaxed(value, timer->base + BCM215XX_GPT_RELOAD(cntr->id));

	reg = readl_relaxed(csr_addr);
	reg |= GPT_CSR_EN | GPT_CSR_INT_EN | GPT_CSR_INT_FLAG;
	writel_relaxed(reg, csr_addr);
}

static void _gpt_stop(struct bcm215xx_gpt_counter *chan)
{
	void *csr_addr = timer->base + BCM215XX_GPT_CSR(chan->id);
	u32 reg;

	reg = readl_relaxed(csr_addr);
	reg &= ~(GPT_CSR_TIMER_PWRON | GPT_CSR_EN);
	writel_relaxed(reg, csr_addr);
}

static u32 _gpt_read(struct bcm215xx_gpt_counter *chan)
{
	return readl(timer->base + BCM215XX_GPT_VALUE(chan->id));
}

static void _gpt_config(struct bcm215xx_gpt_counter *cntr)
{
	u32 ctrl_reg;

	ctrl_reg = readl(timer->base + BCM215XX_GPT_CSR(cntr->id));

	if (timer->rate >= 26000000UL) {
		ctrl_reg |= GPT_CSR_CLKSEL;
		ctrl_reg &= ~GPT_CSR_CLKSEL1;
	} else if (timer->rate >= 1000000UL) {
		ctrl_reg &= ~GPT_CSR_CLKSEL;
		ctrl_reg |= GPT_CSR_CLKSEL1;
	} else {
		ctrl_reg &= ~(GPT_CSR_CLKSEL1 | GPT_CSR_CLKSEL);
	}

	writel(ctrl_reg, timer->base + BCM215XX_GPT_CSR(cntr->id));

	_gpt_stop(cntr);
}

static u64 notrace bcm215xx_gpt_sched_read(void)
{
	return (u64)(~_gpt_read(&timer->counters[NUM_COUNTERS-1])) & 0xffffffff;
}

static int bcm215xx_gpt_shutdown(struct clock_event_device *ce)
{
	struct bcm215xx_gpt_counter *counter = to_bcm215xx_counter(ce);

	_gpt_stop(counter);

	return 0;
}

static irqreturn_t bcm215xx_gpt_interrupt(int irq, void *dev_id)
{
	struct bcm215xx_gpt_counter *cntr;
	unsigned int i;
	u32 isr, reg;

	/*
	 * All counters are serviced by one interrupt; we need to figure out
	 * which event handlers to call based on the value of the ISR register.
	 */

	isr = readl(timer->base + BCM215XX_GPT_ISR);
	for (i = 0; i < NUM_COUNTERS; i++) {
		if (!(isr & GPT_ISR(i)))
			continue;

		cntr = &timer->counters[i];

		reg = readl(timer->base + BCM215XX_GPT_CSR(i));
		reg |= GPT_CSR_INT_FLAG;
		if (cntr->mode == GPT_MODE_ONESHOT)
			reg &= ~GPT_CSR_EN;
		writel(reg, timer->base + BCM215XX_GPT_CSR(i));

		cntr->evt.event_handler(&cntr->evt);
	}

	return IRQ_HANDLED;
}

static int bcm215xx_gpt_set_state_oneshot(struct clock_event_device *ce)
{
	struct bcm215xx_gpt_counter *cntr = to_bcm215xx_counter(ce);

	cntr->mode = GPT_MODE_ONESHOT;
	_gpt_config(cntr);

	return 0;
};

static int bcm215xx_gpt_set_state_periodic(struct clock_event_device *ce)
{
	struct bcm215xx_gpt_counter *cntr = to_bcm215xx_counter(ce);

	cntr->mode = GPT_MODE_PERIODIC;
	_gpt_config(cntr);
	_gpt_start(cntr, DIV_ROUND_CLOSEST(timer->rate, HZ));

	return 0;
};

static int bcm215xx_gpt_set_next_event(unsigned long value,
				       struct clock_event_device *ce)
{
	struct bcm215xx_gpt_counter *cntr = to_bcm215xx_counter(ce);

	_gpt_start(cntr, (u32)value);

	return 0;
}

static int __init bcm215xx_gpt_clockevent_init(struct bcm215xx_gpt_counter *chan)
{
	struct clock_event_device *evt = &chan->evt;

	evt->name = "bcm215xx_gpt_evt";
	evt->features = CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT;
	evt->set_state_shutdown = bcm215xx_gpt_shutdown;
	evt->set_state_oneshot = bcm215xx_gpt_set_state_oneshot;
	evt->set_state_periodic = bcm215xx_gpt_set_state_periodic;
	evt->tick_resume = bcm215xx_gpt_shutdown;
	evt->set_next_event = bcm215xx_gpt_set_next_event;
	evt->rating = 200;
	evt->cpumask = cpumask_of(0);
	evt->irq = timer->irq;
	clockevents_config_and_register(evt, timer->rate,
					0x0e, 0xfffffffe);

	return 0;
}

static int __init bcm215xx_gpt_init(struct device_node *np)
{
	int ret;

	pr_err("bcm215xx-gpt: initializing\n");

	timer = kzalloc_obj(*timer);
	if (!timer)
		return -ENOMEM;

	timer->base = of_iomap(np, 0);
	if (!timer->base) {
		ret = -ENXIO;
		goto err_kfree;
	}

	timer->irq = irq_of_parse_and_map(np, 0);
	if (timer->irq <= 0) {
		pr_err("bcm215xx-gpt: failed to get IRQ\n");
		ret = -EINVAL;
		goto err_iounmap;
	}

	ret = of_property_read_u32(np, "clock-frequency", &timer->rate);
	if (ret) {
		pr_err("bcm215xx-gpt: failed to get frequency\n");
		ret = -EINVAL;
		goto err_iounmap;
	}

	pr_info("bcm215xx-gpt: timer rate = %u Hz\n", timer->rate);

	for (int i = 0; i < NUM_COUNTERS; i++)
		timer->counters[i].id = i;

	/*
	 * The last counter is used for the clocksource; the remaining counters
	 * are used for clockevents
	 */

	_gpt_config(&timer->counters[NUM_COUNTERS - 1]);
	_gpt_start(&timer->counters[NUM_COUNTERS - 1], 0xffffffff);

	sched_clock_register(bcm215xx_gpt_sched_read, 32, timer->rate);
	ret = clocksource_mmio_init(timer->base + BCM215XX_GPT_VALUE((NUM_COUNTERS - 1)),
				    "bcm215xx_gpt_clocksource", timer->rate, 200, 32,
				    clocksource_mmio_readl_down);
	if (ret) {
		pr_err("bcm215xx-gpt: failed to register clocksource\n");
		goto err_iounmap;
	}

	/* Create clock events for the remaining timers */
	for (int i = 0; i < (NUM_COUNTERS - 1); i++)
		bcm215xx_gpt_clockevent_init(&timer->counters[i]);

	ret = request_irq(timer->irq, bcm215xx_gpt_interrupt,
			  IRQF_TIMER, "BCM215xx Timer Tick",
			  NULL);
	if (ret)
		goto err_iounmap;

	return 0;

err_iounmap:
	iounmap(timer->base);

err_kfree:
	kfree(timer);
	return ret;
}

TIMER_OF_DECLARE(bcm215xx_gptimer, "brcm,bcm215xx-gptimer", bcm215xx_gpt_init);
