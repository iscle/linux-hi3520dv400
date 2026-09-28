// SPDX-License-Identifier: GPL-2.0-or-later
/* Hi3520DV400 bring-up, based on the HiSilicon 3.18.20 SDK. */
#include <linux/clk-provider.h>
#include <linux/clocksource.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <asm/mach/arch.h>
#include <asm/smp_plat.h>

static void __init hi3520dv400_timer_init(void)
{
	void __iomem *sys = ioremap(0x12050000, SZ_4K);

	/* SP804 timer 0/1/2 select the fixed 3 MHz oscillator. */
	if (WARN_ON(!sys))
		return;
	writel(readl(sys) & ~(BIT(16) | BIT(18) | BIT(20)), sys);
	iounmap(sys);
	of_clk_init(NULL);
	timer_probe();
}

#ifdef CONFIG_SMP
static int hi3520dv400_boot_secondary(unsigned int cpu,
				      struct task_struct *idle)
{
	void __iomem *boot, *crg;

	if (cpu_logical_map(cpu) != 1)
		return -EINVAL;
	boot = ioremap(0, SZ_4K);
	crg = ioremap(0x12040000, SZ_4K);
	if (!boot || !crg) {
		if (boot)
			iounmap(boot);
		if (crg)
			iounmap(crg);
		return -ENOMEM;
	}
	/* Boot alias at zero, same reset-vector mechanism as the SDK. */
	writel(0xe51ff004, boot); /* ldr pc, [pc, #-4] */
	writel(__pa_symbol(secondary_startup), boot + 4);
	dsb(sy);
	writel(readl(crg + 0x80) & ~BIT(2), crg + 0x80);
	readl(crg + 0x80);
	iounmap(crg);
	iounmap(boot);
	return 0;
}

static const struct smp_operations hi3520dv400_smp_ops __initconst = {
	.smp_boot_secondary = hi3520dv400_boot_secondary,
};

CPU_METHOD_OF_DECLARE(hi3520dv400_smp, "hisilicon,hi3520dv400-smp",
		      &hi3520dv400_smp_ops);
#endif

static const char *const hi3520dv400_compat[] __initconst = {
	"hisilicon,hi3520dv400", NULL,
};

DT_MACHINE_START(HI3520DV400, "HiSilicon Hi3520DV400")
	.dt_compat = hi3520dv400_compat,
	.init_time = hi3520dv400_timer_init,
MACHINE_END
