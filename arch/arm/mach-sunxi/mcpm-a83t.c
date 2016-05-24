/*
 * Copyright (c) 2015 Chen-Yu Tsai
 *
 * Chen-Yu Tsai <wens@csie.org>
 *
 * arch/arm/mach-sunxi/mcpm.c
 *
 * Based on arch/arm/mach-exynos/mcpm-exynos.c and Allwinner code
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/arm-cci.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of_address.h>
#include <linux/irqchip/arm-gic.h>

#include <asm/cputype.h>
#include <asm/cp15.h>
#include <asm/mcpm.h>
#define __init

/*
 * cpucfg
 */
#define PRIVATE_REG0                        0x01a4
#define PRIVATE_REG1                        0x01a8
#define SUNXI_CPUSCFG_C0CPUX_RESET          0x30
#define SUNXI_CPUSCFG_C1CPUX_RESET          0x34
#define SUNXI_CLUSTER_PWRON_RESET(cluster)  (0x30 + (cluster) * 0x4)
/*
 *  cpuxcfg
 */

#define SUNXI_CPUXCFG_C0CTRL_REG0         0x000
#define SUNXI_CPUXCFG_C0CTRL_REG1         0x004
#define SUNXI_CPUXCFG_C1CTRL_REG0         0x010
#define SUNXI_CPUXCFG_C1CTRL_REG1         0x014
#define SUNXI_CLUSTER_CTRL0(cluster)                (0x000 + (cluster) * 0x10)
#define SUNXI_CLUSTER_CTRL1(cluster)                (0x004 + (cluster) * 0x10)
#define SUNXI_CPUXCFG_DBGCTL0             0x020
#define C0_CPU_STATUS                             0x0030
#define C1_CPU_STATUS                             0x0034
#define DBG_REG1                                  0x0038
#define C0_RST_CTRL                               0x0080
#define C1_RST_CTRL                               0x0084
#define GIC_RST_CTRL                              0x0088
#define SUNXI_CLUSTER_CPU_STATUS(cluster)         (0x30 + (cluster)*0x4)
#define SUNXI_CPU_RST_CTRL(cluster)               (0x80 + (cluster)*0x4)
/*
 * prcm
 */
#define SUNXI_C0CPUX_PWROFF_REG           0x100
#define SUNXI_C1CPUX_PWROFF_REG           0x104
#define SUNXI_CLUSTER_PWROFF_GATING(cluster)      (0x100 + (cluster) * 0x4)
#define SUNXI_CPU_PWR_CLAMP(cluster, cpu)         (0x140 + (cluster*4 + cpu)*0x04)


//=========================================

#define SUNXI_CPUS_PER_CLUSTER		4
#define SUNXI_NR_CLUSTERS		2

#define SUN9I_A80_A15_CLUSTER		1

#define CPUCFG_CX_CTRL_REG0(c)		(0x10 * (c))
#define CPUCFG_CX_CTRL_REG0_L1_RST_DISABLE(n)	BIT(n)
#define CPUCFG_CX_CTRL_REG0_L1_RST_DISABLE_ALL	0xf
#define CPUCFG_CX_CTRL_REG0_L2_RST_DISABLE_A7	BIT(4)
#define CPUCFG_CX_CTRL_REG0_L2_RST_DISABLE_A15	BIT(0)
#define CPUCFG_CX_CTRL_REG1(c)		(0x10 * (c) + 0x4)
#define CPUCFG_CX_CTRL_REG1_ACINACTM	BIT(0)
#define CPUCFG_CX_STATUS(c)		(0x30 * (c) + 0x4)
#define CPUCFG_CX_STATUS_STANDBYWFI(n)	BIT(16 + (n))
#define CPUCFG_CX_STATUS_STANDBYWFIL2	BIT(0)
#define CPUCFG_CX_RST_CTRL(c)		(0x80 + 0x4 * (c))
#define CPUCFG_CX_RST_CTRL_DBG_SOC_RST	BIT(24)
#define CPUCFG_CX_RST_CTRL_ETM_RST(n)	BIT(20 + (n))
#define CPUCFG_CX_RST_CTRL_ETM_RST_ALL	(0xf << 20)
#define CPUCFG_CX_RST_CTRL_DBG_RST(n)	BIT(16 + (n))
#define CPUCFG_CX_RST_CTRL_DBG_RST_ALL	(0xf << 16)
#define CPUCFG_CX_RST_CTRL_H_RST	BIT(12)
#define CPUCFG_CX_RST_CTRL_L2_RST	BIT(8)
#define CPUCFG_CX_RST_CTRL_CX_RST(n)	BIT(4 + (n))
#define CPUCFG_CX_RST_CTRL_CORE_RST(n)	BIT(n)

#define PRCM_CPU_PO_RST_CTRL(c)		(0x4 + 0x4 * (c))
#define PRCM_CPU_PO_RST_CTRL_CORE(n)	BIT(n)
#define PRCM_CPU_PO_RST_CTRL_CORE_ALL	0xf
#define PRCM_PWROFF_GATING_REG(c)	(0x100 + 0x4 * (c))
#define PRCM_PWROFF_GATING_REG_CLUSTER	BIT(4)
#define PRCM_PWROFF_GATING_REG_CORE(n)	BIT(n)
#define PRCM_PWR_SWITCH_REG(c, cpu)	(0x140 + 0x10 * (c) + 0x4 * (cpu))
#define PRCM_CPU_SOFT_ENTRY_REG		0x164

#define CPU0_SUPPORT_HOTPLUG_MAGIC0	0xFA50392F
#define CPU0_SUPPORT_HOTPLUG_MAGIC1	0x790DCA3A

static void __iomem *cpuxcfg_base;
static void __iomem *cpuscfg_base;
static void __iomem *prcm_base;

static int sunxi_cpu_power_switch_set(unsigned int cpu, unsigned int cluster,
				      bool enable)
{
	u32 reg;

	/* control sequence from Allwinner A80 user manual v1.2 PRCM section */
	reg = readl(prcm_base + PRCM_PWR_SWITCH_REG(cluster, cpu));
	if (enable) {
		if (reg == 0x00) {
			pr_debug("power clamp for cluster %u cpu %u already open",
				 cluster, cpu);
			return 0;
		}

		writel(0xff, prcm_base + PRCM_PWR_SWITCH_REG(cluster, cpu));
		udelay(10);
		writel(0xfe, prcm_base + PRCM_PWR_SWITCH_REG(cluster, cpu));
		udelay(10);
		writel(0xf8, prcm_base + PRCM_PWR_SWITCH_REG(cluster, cpu));
		udelay(10);
		writel(0xf0, prcm_base + PRCM_PWR_SWITCH_REG(cluster, cpu));
		udelay(10);
		writel(0x00, prcm_base + PRCM_PWR_SWITCH_REG(cluster, cpu));
		udelay(10);
	} else {
		writel(0xff, prcm_base + PRCM_PWR_SWITCH_REG(cluster, cpu));
		udelay(10);
	}

	return 0;
}

static void sunxi_cpu0_hotplug_support_set(bool enable)
{
	if (enable) {
		writel(CPU0_SUPPORT_HOTPLUG_MAGIC0, sram_b_smp_base);
		writel(CPU0_SUPPORT_HOTPLUG_MAGIC1, sram_b_smp_base + 0x4);
	} else {
		writel(0x0, sram_b_smp_base);
		writel(0x0, sram_b_smp_base + 0x4);
	}
}

static int sunxi_cpu_powerup(unsigned int cpu, unsigned int cluster)
{
	u32 reg;

	pr_debug("%s: cpu %u cluster %u\n", __func__, cpu, cluster);
	if (cpu >= SUNXI_CPUS_PER_CLUSTER || cluster >= SUNXI_NR_CLUSTERS)
		return -EINVAL;

	/* Set hotplug support magic flags for cpu0 */
	if (cluster == 0 && cpu == 0)
		sunxi_cpu0_hotplug_support_set(true);

	/* assert processor power-on reset */
	reg = readl(prcm_base + PRCM_CPU_PO_RST_CTRL(cluster));
	reg &= ~PRCM_CPU_PO_RST_CTRL_CORE(cpu);
	writel(reg, prcm_base + PRCM_CPU_PO_RST_CTRL(cluster));

	/* Cortex-A7: hold L1 reset disable signal low */
	if (!(of_machine_is_compatible("allwinner,sun9i-a80") &&
			cluster == SUN9I_A80_A15_CLUSTER)) {
		reg = readl(cpuxcfg_base + CPUCFG_CX_CTRL_REG0(cluster));
		reg &= ~CPUCFG_CX_CTRL_REG0_L1_RST_DISABLE(cpu);
		writel(reg, cpuxcfg_base + CPUCFG_CX_CTRL_REG0(cluster));
	}

	/* assert processor related resets */
	reg = readl(cpuxcfg_base + CPUCFG_CX_RST_CTRL(cluster));
	reg &= ~CPUCFG_CX_RST_CTRL_DBG_RST(cpu);

	/*
	 * Allwinner code also asserts resets for NEON on A15. According
	 * to ARM manuals, asserting power-on reset is sufficient.
	 */
	if (!(of_machine_is_compatible("allwinner,sun9i-a80") &&
			cluster == SUN9I_A80_A15_CLUSTER)) {
		reg &= ~CPUCFG_CX_RST_CTRL_ETM_RST(cpu);
	}
	writel(reg, cpuxcfg_base + CPUCFG_CX_RST_CTRL(cluster));

	/* open power switch */
	sunxi_cpu_power_switch_set(cpu, cluster, true);

	/* clear processor power gate */
	reg = readl(prcm_base + PRCM_PWROFF_GATING_REG(cluster));
	reg &= ~PRCM_PWROFF_GATING_REG_CORE(cpu);
	writel(reg, prcm_base + PRCM_PWROFF_GATING_REG(cluster));
	udelay(20);

	/* de-assert processor power-on reset */
	reg = readl(prcm_base + PRCM_CPU_PO_RST_CTRL(cluster));
	reg |= PRCM_CPU_PO_RST_CTRL_CORE(cpu);
	writel(reg, prcm_base + PRCM_CPU_PO_RST_CTRL(cluster));

	/* de-assert all processor resets */
	reg = readl(cpuxcfg_base + CPUCFG_CX_RST_CTRL(cluster));
	reg |= CPUCFG_CX_RST_CTRL_DBG_RST(cpu);
	reg |= CPUCFG_CX_RST_CTRL_CORE_RST(cpu);
	if (!(of_machine_is_compatible("allwinner,sun9i-a80") &&
			cluster == SUN9I_A80_A15_CLUSTER)) {
		reg |= CPUCFG_CX_RST_CTRL_ETM_RST(cpu);
	} else {
		reg |= CPUCFG_CX_RST_CTRL_CX_RST(cpu); /* NEON */
	}
	writel(reg, cpuxcfg_base + CPUCFG_CX_RST_CTRL(cluster));

	return 0;
}

static int sunxi_cluster_powerup(unsigned int cluster)
{
	u32 reg;

	pr_debug("%s: cluster %u\n", __func__, cluster);
	if (cluster >= SUNXI_NR_CLUSTERS)
		return -EINVAL;

	/* assert ACINACTM */
	reg = readl(cpuxcfg_base + CPUCFG_CX_CTRL_REG1(cluster));
	reg |= CPUCFG_CX_CTRL_REG1_ACINACTM;
	writel(reg, cpuxcfg_base + CPUCFG_CX_CTRL_REG1(cluster));

	/* assert cluster processor power-on resets */
	reg = readl(prcm_base + PRCM_CPU_PO_RST_CTRL(cluster));
	reg &= ~PRCM_CPU_PO_RST_CTRL_CORE_ALL;
	writel(reg, prcm_base + PRCM_CPU_PO_RST_CTRL(cluster));

	/* assert cluster resets */
	reg = readl(cpuxcfg_base + CPUCFG_CX_RST_CTRL(cluster));
	reg &= ~CPUCFG_CX_RST_CTRL_DBG_SOC_RST;
	reg &= ~CPUCFG_CX_RST_CTRL_DBG_RST_ALL;
	reg &= ~CPUCFG_CX_RST_CTRL_H_RST;
	reg &= ~CPUCFG_CX_RST_CTRL_L2_RST;

	/*
	 * Allwinner code also asserts resets for NEON on A15. According
	 * to ARM manuals, asserting power-on reset is sufficient.
	 */
	if (!(of_machine_is_compatible("allwinner,sun9i-a80") &&
			cluster == SUN9I_A80_A15_CLUSTER)) {
		reg &= ~CPUCFG_CX_RST_CTRL_ETM_RST_ALL;
	}
	writel(reg, cpuxcfg_base + CPUCFG_CX_RST_CTRL(cluster));

	/* hold L1/L2 reset disable signals low */
	reg = readl(cpuxcfg_base + CPUCFG_CX_CTRL_REG0(cluster));
	if (of_machine_is_compatible("allwinner,sun9i-a80") &&
			cluster == SUN9I_A80_A15_CLUSTER) {
		/* Cortex-A15: hold L2RSTDISABLE low */
		reg &= ~CPUCFG_CX_CTRL_REG0_L2_RST_DISABLE_A15;
	} else {
		/* Cortex-A7: hold L1RSTDISABLE and L2RSTDISABLE low */
		reg &= ~CPUCFG_CX_CTRL_REG0_L1_RST_DISABLE_ALL;
		reg &= ~CPUCFG_CX_CTRL_REG0_L2_RST_DISABLE_A7;
	}
	writel(reg, cpuxcfg_base + CPUCFG_CX_CTRL_REG0(cluster));

	/* clear cluster power gate */
	reg = readl(prcm_base + PRCM_PWROFF_GATING_REG(cluster));
	reg &= ~PRCM_PWROFF_GATING_REG_CLUSTER;
	writel(reg, prcm_base + PRCM_PWROFF_GATING_REG(cluster));
	udelay(20);

	/* de-assert cluster resets */
	reg = readl(cpuxcfg_base + CPUCFG_CX_RST_CTRL(cluster));
	reg |= CPUCFG_CX_RST_CTRL_DBG_SOC_RST;
	reg |= CPUCFG_CX_RST_CTRL_H_RST;
	reg |= CPUCFG_CX_RST_CTRL_L2_RST;
	writel(reg, cpuxcfg_base + CPUCFG_CX_RST_CTRL(cluster));

	/* de-assert ACINACTM */
	reg = readl(cpuxcfg_base + CPUCFG_CX_CTRL_REG1(cluster));
	reg &= ~CPUCFG_CX_CTRL_REG1_ACINACTM;
	writel(reg, cpuxcfg_base + CPUCFG_CX_CTRL_REG1(cluster));

	return 0;
}

static void sunxi_cpu_powerdown_prepare(unsigned int cpu, unsigned int cluster)
{
	gic_cpu_if_down();
}

static void sunxi_cluster_powerdown_prepare(unsigned int cluster)
{
}

static void sunxi_cpu_cache_disable(void)
{
	/* Disable and flush the local CPU cache. */
	v7_exit_coherency_flush(louis);
}

static void sunxi_cluster_cache_disable(void)
{
	if (read_cpuid_part() == ARM_CPU_PART_CORTEX_A15) {
		/*
		 * On the Cortex-A15 we need to disable
		 * L2 prefetching before flushing the cache.
		 */
		asm volatile(
		"mcr	p15, 1, %0, c15, c0, 3\n"
		"isb\n"
		"dsb"
		: : "r" (0x400));
	}

	/* Flush all cache levels for this cluster. */
	v7_exit_coherency_flush(all);

	/* Are we supposed to wait for STANDBYWFIL2? */

	/*
	 * Disable cluster-level coherency by masking
	 * incoming snoops and DVM messages:
	 */
	cci_disable_port_by_cpu(read_cpuid_mpidr());
}

static int sunxi_cpu_powerdown(unsigned int cpu, unsigned int cluster)
{
	u32 reg;

	pr_debug("%s: cpu %u cluster %u\n", __func__, cpu, cluster);
	if (cpu >= SUNXI_CPUS_PER_CLUSTER || cluster >= SUNXI_NR_CLUSTERS)
		return -EINVAL;

	/* gate processor power */
	reg = readl(prcm_base + PRCM_PWROFF_GATING_REG(cluster));
	reg |= PRCM_PWROFF_GATING_REG_CORE(cpu);
	writel(reg, prcm_base + PRCM_PWROFF_GATING_REG(cluster));
	udelay(20);

	/* close power switch */
	sunxi_cpu_power_switch_set(cpu, cluster, false);

	return 0;
}

static int sunxi_cluster_powerdown(unsigned int cluster)
{
	u32 reg;

	pr_debug("%s: cluster %u\n", __func__, cluster);
	if (cluster >= SUNXI_NR_CLUSTERS)
		return -EINVAL;

	/* clear cluster power gate */
	reg = readl(prcm_base + PRCM_PWROFF_GATING_REG(cluster));
	reg &= ~PRCM_PWROFF_GATING_REG_CLUSTER;
	writel(reg, prcm_base + PRCM_PWROFF_GATING_REG(cluster));
	udelay(20);

	return 0;
}

static int sunxi_wait_for_powerdown(unsigned int cpu, unsigned int cluster)
{
	int ret;
	u32 reg;

	pr_debug("%s: cpu %u cluster %u\n", __func__, cpu, cluster);

	/* wait for CPU core to enter WFI */
	ret = readl_poll_timeout(cpuxcfg_base + CPUCFG_CX_STATUS(cluster), reg,
				 reg & CPUCFG_CX_STATUS_STANDBYWFI(cpu),
				 1000, 100000);

	if (ret)
		return ret;

	/* power down CPU core */
	sunxi_cpu_powerdown(cpu, cluster);

	if (__mcpm_cluster_state(cluster) != CLUSTER_DOWN)
		return 0;

	/* last man standing, assert ACINACTM */
	reg = readl(cpuxcfg_base + CPUCFG_CX_CTRL_REG1(cluster));
	reg |= CPUCFG_CX_CTRL_REG1_ACINACTM;
	writel(reg, cpuxcfg_base + CPUCFG_CX_CTRL_REG1(cluster));

	/* wait for cluster L2 WFI */
	ret = readl_poll_timeout(cpuxcfg_base + CPUCFG_CX_STATUS(cluster), reg,
				 reg & CPUCFG_CX_STATUS_STANDBYWFIL2,
				 1000, 100000);
	if (ret) {
		pr_warn("%s: cluster %u time out waiting for STANDBYWFIL2\n",
				__func__, cluster);
		return ret;
	}

	sunxi_cluster_powerdown(cluster);

	return 0;
}

static const struct mcpm_platform_ops sunxi_power_ops = {
	.cpu_powerup		   = sunxi_cpu_powerup,
	.cpu_powerdown_prepare	   = sunxi_cpu_powerdown_prepare,
	.cluster_powerup	   = sunxi_cluster_powerup,
	.cluster_powerdown_prepare = sunxi_cluster_powerdown_prepare,
	.cpu_cache_disable	   = sunxi_cpu_cache_disable,
	.cluster_cache_disable	   = sunxi_cluster_cache_disable,
	.wait_for_powerdown	   = sunxi_wait_for_powerdown,
};


static void sunxi_mcpm_setup_entry_point(void)
{
//	__raw_writel(virt_to_phys(mcpm_entry_point),
//		     prcm_base + PRCM_CPU_SOFT_ENTRY_REG);
	__raw_writel(virt_to_phys(mcpm_entry_point), cpuscfg_base + PRIVATE_REG0);
}

static void __init sun8i_mcpm_boot_cpu_init(void)
{
	unsigned int mpidr, cpu, cluster;

	mpidr = read_cpuid_mpidr();
	cpu = MPIDR_AFFINITY_LEVEL(mpidr, 0);
	cluster = MPIDR_AFFINITY_LEVEL(mpidr, 1);

	pr_debug("%s: cpu %u cluster %u\n", __func__, cpu, cluster);
	BUG_ON(cpu >= MAX_CPUS_PER_CLUSTER || cluster >= MAX_NR_CLUSTERS);
	sun8i_cpu_use_count[cluster][cpu] = 1;
	sun8i_cluster_use_count[cluster]++;

	cpumask_clear(&cpu_power_up_state_mask);
	cpumask_set_cpu((cluster*4 + cpu),&cpu_power_up_state_mask);
}

extern void sun8i_power_up_setup(unsigned int affinity_level);

static int __init sunxi_mcpm_init(void)
{
	struct device_node *node;
	int ret;

	if (!of_machine_is_compatible("allwinner,sun8i-a83t"))
		return -ENODEV;

	if (!cci_probed())
		return -ENODEV;

	node = of_find_compatible_node(NULL, NULL,
			"allwinner,sun8i-a83t-cpuxcfg");
	if (!node)
		return -ENODEV;

	cpuxcfg_base = of_iomap(node, 0);
	of_node_put(node);
	if (!cpuxcfg_base) {
		pr_err("%s: failed to map CPUXCFG registers\n", __func__);
		return -ENOMEM;
	}

	node = of_find_compatible_node(NULL, NULL,
			"allwinner,sun8i-a83t-cpuscfg");
	if (!node)
		return -ENODEV;

	cpuscfg_base = of_iomap(node, 0);
	of_node_put(node);
	if (!cpuscfg_base) {
		pr_err("%s: failed to map CPUSCFG registers\n", __func__);
		return -ENOMEM;
	}

	node = of_find_compatible_node(NULL, NULL,
			"allwinner,sun8i-a83t-prcm");
	if (!node)
		return -ENODEV;

	prcm_base = of_iomap(node, 0);
	of_node_put(node);
	if (!prcm_base) {
		pr_err("%s: failed to map PRCM registers\n", __func__);
		iounmap(prcm_base);
		return -ENOMEM;
	}

	node = of_find_compatible_node(NULL, NULL,
			"allwinner,sun9i-smp-sram");
	if (!node)
		return -ENODEV;

	sun8i_mcpm_boot_cpu_init();

//	hrtimer_init(&cluster_power_down_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
//	cluster_power_down_timer.function = sun8i_cluster_power_down;
//	hrtimer_start(&cluster_power_down_timer, ktime_set(0, 10000000), HRTIMER_MODE_REL);

	ret = mcpm_platform_register(&sunxi_power_ops);
	if (!ret)
		ret = mcpm_sync_init(sun8i_power_up_setup);
	/* turn on the CCI */
	if (!ret)
		ret = mcpm_loopback(sunxi_cluster_cache_disable);
	if (ret) {
		iounmap(cpuxcfg_base);
		iounmap(cpuscfg_base);
		iounmap(prcm_base);
		return ret;
	}

#ifdef MCPM_WITH_ARISC_DVFS_SUPPORT
	/* register arisc ready notifier */
	arisc_register_notifier(&sun8i_arisc_notifier);
#endif

	mcpm_smp_set_ops();

	pr_info("sunxi MCPM support installed\n");

	sunxi_mcpm_setup_entry_point();

	return ret;
}

early_initcall(sunxi_mcpm_init);
