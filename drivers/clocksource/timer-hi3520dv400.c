// SPDX-License-Identifier: GPL-2.0-or-later
/* Hi3520DV400 SP804 channels: one clocksource and two CPU clock events. */
#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/cpuhotplug.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/sched_clock.h>
#include <asm/delay.h>
#include <asm/smp_plat.h>

#include "timer-sp.h"

#define HI3520DV400_TIMER_CPUS	2
#define TIMER_EVENT_CTRL		(TIMER_CTRL_32BIT | TIMER_CTRL_IE)

struct hi3520dv400_timer_event {
	struct clock_event_device event;
	void __iomem *base;
};

static void __iomem *hi3520dv400_source_base;
static unsigned long hi3520dv400_timer_rate;
static struct hi3520dv400_timer_event hi3520dv400_events[HI3520DV400_TIMER_CPUS];

static struct hi3520dv400_timer_event *to_hi3520dv400_event(struct clock_event_device *event)
{
	return container_of(event, struct hi3520dv400_timer_event, event);
}

static int hi3520dv400_event_stop(struct clock_event_device *event)
{
	void __iomem *base = to_hi3520dv400_event(event)->base;

	writel(0, base + TIMER_CTRL);
	writel(1, base + TIMER_INTCLR);
	return 0;
}

static int hi3520dv400_event_periodic(struct clock_event_device *event)
{
	void __iomem *base = to_hi3520dv400_event(event)->base;

	hi3520dv400_event_stop(event);
	writel(DIV_ROUND_CLOSEST(hi3520dv400_timer_rate, HZ), base + TIMER_LOAD);
	writel(TIMER_EVENT_CTRL | TIMER_CTRL_PERIODIC | TIMER_CTRL_ENABLE,
	       base + TIMER_CTRL);
	return 0;
}

static int hi3520dv400_event_next(unsigned long cycles, struct clock_event_device *event)
{
	void __iomem *base = to_hi3520dv400_event(event)->base;

	writel(cycles, base + TIMER_LOAD);
	writel(TIMER_EVENT_CTRL | TIMER_CTRL_ONESHOT | TIMER_CTRL_ENABLE,
	       base + TIMER_CTRL);
	return 0;
}

static irqreturn_t hi3520dv400_timer_interrupt(int irq, void *data)
{
	struct hi3520dv400_timer_event *timer = data;

	if (!(readl(timer->base + TIMER_MIS) & 1))
		return IRQ_NONE;
	writel(1, timer->base + TIMER_INTCLR);
	timer->event.event_handler(&timer->event);
	return IRQ_HANDLED;
}

static int hi3520dv400_timer_start_cpu(unsigned int cpu)
{
	unsigned int index = cpu_logical_map(cpu);
	struct clock_event_device *event;
	int ret;

	if (index >= HI3520DV400_TIMER_CPUS)
		return -EINVAL;
	event = &hi3520dv400_events[index].event;
	event->cpumask = cpumask_of(cpu);
	ret = irq_force_affinity(event->irq, event->cpumask);
	if (ret)
		return ret;
	clockevents_config_and_register(event, hi3520dv400_timer_rate, 0xf, U32_MAX);
	enable_irq(event->irq);
	return 0;
}

static int hi3520dv400_timer_stop_cpu(unsigned int cpu)
{
	struct clock_event_device *event = &hi3520dv400_events[cpu_logical_map(cpu)].event;

	disable_irq_nosync(event->irq);
	hi3520dv400_event_stop(event);
	return 0;
}

static u64 hi3520dv400_clocksource_read(struct clocksource *source)
{
	return ~readl_relaxed(hi3520dv400_source_base + TIMER_VALUE);
}

static struct clocksource hi3520dv400_clocksource = {
	.name = "hi3520dv400",
	.rating = 300,
	.read = hi3520dv400_clocksource_read,
	.mask = CLOCKSOURCE_MASK(32),
	.flags = CLOCK_SOURCE_IS_CONTINUOUS,
};

static u64 notrace hi3520dv400_sched_clock_read(void)
{
	return ~readl_relaxed(hi3520dv400_source_base + TIMER_VALUE);
}

static unsigned long hi3520dv400_delay_read(void)
{
	return ~readl_relaxed(hi3520dv400_source_base + TIMER_VALUE);
}

static struct delay_timer hi3520dv400_delay_timer = {
	.read_current_timer = hi3520dv400_delay_read,
};

static int __init hi3520dv400_timer_init(struct device_node *node)
{
	struct hi3520dv400_timer_event *timer;
	struct clk *clk;
	unsigned int i, allocated = 0;
	int ret;

	clk = of_clk_get(node, 0);
	if (IS_ERR(clk))
		return PTR_ERR(clk);
	ret = clk_prepare_enable(clk);
	if (ret)
		goto put_clk;
	hi3520dv400_timer_rate = clk_get_rate(clk);
	if (!hi3520dv400_timer_rate) {
		ret = -EINVAL;
		goto disable_clk;
	}
	hi3520dv400_source_base = of_iomap(node, 0);
	if (!hi3520dv400_source_base) {
		ret = -ENOMEM;
		goto disable_clk;
	}
	writel(0, hi3520dv400_source_base + TIMER_CTRL);
	for (i = 0; i < HI3520DV400_TIMER_CPUS; i++) {
		timer = &hi3520dv400_events[i];
		timer->base = of_iomap(node, i + 1);
		if (!timer->base) {
			ret = -ENOMEM;
			goto free_events;
		}
		allocated++;
		timer->event.name = "hi3520dv400-clockevent";
		timer->event.rating = 350;
		timer->event.features = CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT |
					CLOCK_EVT_FEAT_PERCPU;
		timer->event.set_next_event = hi3520dv400_event_next;
		timer->event.set_state_periodic = hi3520dv400_event_periodic;
		timer->event.set_state_oneshot = hi3520dv400_event_stop;
		timer->event.set_state_shutdown = hi3520dv400_event_stop;
		timer->event.tick_resume = hi3520dv400_event_stop;
		hi3520dv400_event_stop(&timer->event);
		ret = irq_of_parse_and_map(node, i);
		if (!ret) {
			ret = -EINVAL;
			goto free_events;
		}
		timer->event.irq = ret;
		ret = request_irq(timer->event.irq, hi3520dv400_timer_interrupt,
				  IRQF_TIMER | IRQF_NOBALANCING | IRQF_NO_AUTOEN,
				  timer->event.name, timer);
		if (ret) {
			irq_dispose_mapping(timer->event.irq);
			timer->event.irq = 0;
			goto free_events;
		}
	}
	writel(U32_MAX, hi3520dv400_source_base + TIMER_LOAD);
	writel(TIMER_CTRL_32BIT | TIMER_CTRL_PERIODIC | TIMER_CTRL_ENABLE,
	       hi3520dv400_source_base + TIMER_CTRL);
	ret = clocksource_register_hz(&hi3520dv400_clocksource, hi3520dv400_timer_rate);
	if (ret)
		goto free_events;
	ret = cpuhp_setup_state(CPUHP_AP_HI3520DV400_TIMER_STARTING,
				"clockevents/hi3520dv400:starting",
				hi3520dv400_timer_start_cpu, hi3520dv400_timer_stop_cpu);
	if (ret) {
		clocksource_unregister(&hi3520dv400_clocksource);
		goto free_events;
	}
	sched_clock_register(hi3520dv400_sched_clock_read, 32, hi3520dv400_timer_rate);
	hi3520dv400_delay_timer.freq = hi3520dv400_timer_rate;
	register_current_timer_delay(&hi3520dv400_delay_timer);
	return 0;

free_events:
	while (allocated--) {
		timer = &hi3520dv400_events[allocated];
		if (timer->event.irq) {
			free_irq(timer->event.irq, timer);
			irq_dispose_mapping(timer->event.irq);
		}
		iounmap(timer->base);
	}
	writel(0, hi3520dv400_source_base + TIMER_CTRL);
	iounmap(hi3520dv400_source_base);
disable_clk:
	clk_disable_unprepare(clk);
put_clk:
	clk_put(clk);
	return ret;
}

TIMER_OF_DECLARE(hi3520dv400, "hisilicon,hi3520dv400-timer", hi3520dv400_timer_init);
