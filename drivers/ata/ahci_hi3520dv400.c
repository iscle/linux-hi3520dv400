// SPDX-License-Identifier: GPL-2.0-or-later
/* Hi3520DV400 AHCI glue. Register sequence derived from HiSilicon's
 * phy-hi3520dv400-sata.c (Copyright 2016-2017 HiSilicon Technologies).
 */
#include <linux/ahci_platform.h>
#include <linux/delay.h>
#include <linux/libata.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include "ahci.h"

#define SATA_CRG_PHY		0x120
#define SATA_CRG_CTRL		0x128
#define SATA_PHY0_RESET		BIT(2)
#define SATA_PHY1_RESET		BIT(3)
#define SATA_PHY_REF_MASK	GENMASK(7, 4)
#define SATA_CTRL_RESETS		(BIT(8) | BIT(9) | BIT(10) | BIT(11))
#define SATA_PORT1_RESETS	(BIT(14) | BIT(15))
#define SATA_CTRL_CLOCKS		(BIT(0) | BIT(1) | BIT(2) | BIT(3))
#define SATA_PORT1_CLOCKS	(BIT(12) | BIT(13))
#define SATA_MISC_PHY0		0x58
#define SATA_MISC_PHY1		0x64
#define SATA_PHY_WRITE		BIT(6)
#define SATA_PHY_CTL1		0xa4
#define SATA_PHY_CTL2		0xb0
#define SATA_PHY_RESET_MASK	0xac
#define SATA_PHY_ENDIAN		BIT(3)
#define SATA_PHY_PORT1_BACK	BIT(5)
#define SATA_PORT_FIFO		0x44
#define SATA_PORT_SIGNAL	0x48
#define SATA_PORT_PHY		0x74
#define SATA_FIFO_THRESHOLD	0xfeed9f24
#define SATA_PHY_RATE_6G		0x0e5a0000
#define SATA_PHY_SIGNAL_DEFAULT	0x40438

struct hi3520dv400_sata {
	struct device *dev;
	struct regmap *crg, *misc;
	u32 ports;
};

static int hi3520dv400_sata_phy_write(struct hi3520dv400_sata *priv,
				      unsigned int port, u32 value)
{
	u32 offset = port ? SATA_MISC_PHY1 : SATA_MISC_PHY0;
	const struct reg_sequence sequence[] = {
		{ offset, value },
		{ offset, value | SATA_PHY_WRITE },
		{ offset, value },
		{ offset, 0 },
	};

	return regmap_multi_reg_write(priv->misc, sequence, ARRAY_SIZE(sequence));
}

static int hi3520dv400_sata_eq(struct hi3520dv400_sata *priv, unsigned int port,
			       bool online_6g)
{
	int ret;

	ret = hi3520dv400_sata_phy_write(priv, port, online_6g ? 0xf09 : 0x009);
	if (ret)
		return ret;
	return hi3520dv400_sata_phy_write(priv, port, online_6g ? 0x308 : 0x508);
}

static int hi3520dv400_sata_power_off(struct ahci_host_priv *hpriv)
{
	struct hi3520dv400_sata *priv = hpriv->plat_data;
	u32 resets = SATA_CTRL_RESETS;
	u32 clocks = SATA_CTRL_CLOCKS;
	u32 phy_resets = SATA_PHY0_RESET;
	u32 phy_clocks = BIT(0);
	int ret;

	if (priv->ports == 2) {
		resets |= SATA_PORT1_RESETS;
		clocks |= SATA_PORT1_CLOCKS;
		phy_resets |= SATA_PHY1_RESET;
		phy_clocks |= BIT(1);
	}
	ret = regmap_update_bits(priv->crg, SATA_CRG_CTRL, resets, resets);
	if (ret)
		return ret;
	ret = regmap_update_bits(priv->crg, SATA_CRG_PHY, phy_resets, phy_resets);
	if (ret)
		return ret;
	ret = regmap_update_bits(priv->crg, SATA_CRG_CTRL, clocks, 0);
	if (ret)
		return ret;
	return regmap_update_bits(priv->crg, SATA_CRG_PHY, phy_clocks, 0);
}

static void hi3520dv400_sata_disable(void *data)
{
	struct ahci_host_priv *hpriv = data;
	struct hi3520dv400_sata *priv = hpriv->plat_data;
	int ret;

	ret = hi3520dv400_sata_power_off(hpriv);
	if (ret)
		dev_warn(priv->dev, "failed to disable SATA PHY: %d\n", ret);
}

static int hi3520dv400_sata_init(struct ahci_host_priv *hpriv)
{
	struct hi3520dv400_sata *priv = hpriv->plat_data;
	u32 resets = SATA_CTRL_RESETS;
	u32 clocks = SATA_CTRL_CLOCKS;
	u32 phy_resets = SATA_PHY0_RESET;
	u32 phy_clocks = BIT(0);
	void __iomem *mmio = hpriv->mmio;
	unsigned int i;
	int ret;

	if (priv->ports == 2) {
		resets |= SATA_PORT1_RESETS;
		clocks |= SATA_PORT1_CLOCKS;
		phy_resets |= SATA_PHY1_RESET;
		phy_clocks |= BIT(1);
	}
	ret = regmap_update_bits(priv->crg, SATA_CRG_CTRL, resets, resets);
	if (ret)
		return ret;
	ret = regmap_update_bits(priv->crg, SATA_CRG_PHY, phy_resets, phy_resets);
	if (ret)
		return ret;
	/* The SDK selects PHY1's reference even in the one-port configuration. */
	ret = regmap_update_bits(priv->crg, SATA_CRG_PHY,
				 priv->ports == 2 ? SATA_PHY_REF_MASK : GENMASK(7, 6),
				 priv->ports == 2 ? 0x50 : 0x40);
	if (ret)
		return ret;
	ret = regmap_update_bits(priv->crg, SATA_CRG_PHY, phy_clocks, phy_clocks);
	if (ret)
		return ret;
	ret = regmap_update_bits(priv->crg, SATA_CRG_CTRL, clocks, clocks);
	if (ret)
		return ret;
	msleep(20);
	ret = regmap_update_bits(priv->crg, SATA_CRG_PHY, phy_resets, 0);
	if (ret)
		return ret;
	msleep(20);
	for (i = 0; i < priv->ports; i++) {
		/* PLL 6 GHz, disable spread spectrum, set initial equalization. */
		ret = hi3520dv400_sata_phy_write(priv, i, 0xd01);
		if (ret)
			return ret;
		ret = hi3520dv400_sata_phy_write(priv, i, 0x803);
		if (ret)
			return ret;
		ret = hi3520dv400_sata_eq(priv, i, false);
		if (ret)
			return ret;
	}
	msleep(20);
	ret = regmap_update_bits(priv->crg, SATA_CRG_CTRL, resets, 0);
	if (ret)
		return ret;
	msleep(20);
	writel(readl(mmio + SATA_PHY_CTL1) | SATA_PHY_ENDIAN, mmio + SATA_PHY_CTL1);
	writel(readl(mmio + SATA_PHY_CTL2) | SATA_PHY_ENDIAN, mmio + SATA_PHY_CTL2);
	writel((readl(mmio + SATA_PHY_RESET_MASK) | GENMASK(7, 4)) &
	       ~(priv->ports == 1 ? SATA_PHY_PORT1_BACK : 0),
	       mmio + SATA_PHY_RESET_MASK);
	for (i = 0; i < priv->ports; i++) {
		void __iomem *port = mmio + 0x100 + i * 0x80;

		writel(SATA_FIFO_THRESHOLD, port + SATA_PORT_FIFO);
		writel(SATA_PHY_RATE_6G, port + SATA_PORT_PHY);
		writel(SATA_PHY_SIGNAL_DEFAULT, port + SATA_PORT_SIGNAL);
	}
	return 0;
}

static int hi3520dv400_sata_hardreset(struct ata_link *link, unsigned int *class,
				      unsigned long deadline)
{
	struct ahci_host_priv *hpriv = link->ap->host->private_data;
	struct hi3520dv400_sata *p = hpriv->plat_data;
	bool online;
	u32 sstatus;
	int ret, eq_ret;

	ret = ahci_do_hardreset(link, class, deadline, &online);
	if (!sata_scr_read(link, SCR_STATUS, &sstatus)) {
		eq_ret = hi3520dv400_sata_eq(p, link->ap->port_no,
					     (sstatus & 0xff) == 0x33);
		if (!ret)
			ret = eq_ret;
	}
	return ret;
}

static struct ata_port_operations hi3520dv400_sata_ops = {
	.inherits = &ahci_platform_ops,
	.reset.hardreset = hi3520dv400_sata_hardreset,
};

static const struct ata_port_info hi3520dv400_sata_info = {
	.flags = AHCI_FLAG_COMMON | ATA_FLAG_NO_LPM,
	.pio_mask = ATA_PIO4,
	.udma_mask = ATA_UDMA6,
	.port_ops = &hi3520dv400_sata_ops,
};

static const struct scsi_host_template hi3520dv400_sata_sht = {
	AHCI_SHT("hi3520dv400-ahci"),
};

static int hi3520dv400_sata_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ahci_host_priv *hpriv;
	struct hi3520dv400_sata *p;
	int ret;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->dev = dev;
	p->crg = syscon_regmap_lookup_by_phandle(dev->of_node, "hisilicon,crg");
	if (IS_ERR(p->crg))
		return PTR_ERR(p->crg);
	p->misc = syscon_regmap_lookup_by_phandle(dev->of_node, "hisilicon,misc");
	if (IS_ERR(p->misc))
		return PTR_ERR(p->misc);
	p->ports = 1;
	of_property_read_u32(dev->of_node, "hisilicon,ports", &p->ports);
	if (p->ports < 1 || p->ports > 2)
		return -EINVAL;
	hpriv = ahci_platform_get_resources(pdev, 0);
	if (IS_ERR(hpriv))
		return PTR_ERR(hpriv);
	hpriv->plat_data = p;
	hpriv->flags |= AHCI_HFLAG_32BIT_ONLY | AHCI_HFLAG_NO_NCQ |
			AHCI_HFLAG_NO_FBS | AHCI_HFLAG_NO_PMP;
	hpriv->mask_port_map = BIT(p->ports) - 1;
	ret = ahci_platform_enable_resources(hpriv);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, hi3520dv400_sata_disable, hpriv);
	if (ret)
		goto disable_resources;
	ret = hi3520dv400_sata_init(hpriv);
	if (ret)
		goto disable_resources;
	ret = ahci_platform_init_host(pdev, hpriv, &hi3520dv400_sata_info, &hi3520dv400_sata_sht);
	if (!ret)
		return 0;
disable_resources:
	ahci_platform_disable_resources(hpriv);
	return ret;
}

static int hi3520dv400_sata_suspend(struct device *dev)
{
	struct ata_host *host = dev_get_drvdata(dev);
	int ret;

	ret = ahci_platform_suspend_host(dev);
	if (ret)
		return ret;
	ret = hi3520dv400_sata_power_off(host->private_data);
	if (ret)
		ahci_platform_resume_host(dev);
	return ret;
}

static int hi3520dv400_sata_resume(struct device *dev)
{
	struct ata_host *host = dev_get_drvdata(dev);
	int ret;

	ret = hi3520dv400_sata_init(host->private_data);
	if (ret)
		return ret;
	return ahci_platform_resume_host(dev);
}

static DEFINE_SIMPLE_DEV_PM_OPS(hi3520dv400_sata_pm,
			      hi3520dv400_sata_suspend, hi3520dv400_sata_resume);
static const struct of_device_id hi3520dv400_sata_match[] = {
	{ .compatible = "hisilicon,hi3520dv400-ahci" },
	{ }
};
MODULE_DEVICE_TABLE(of, hi3520dv400_sata_match);
static struct platform_driver hi3520dv400_sata_driver = {
	.probe = hi3520dv400_sata_probe,
	.remove = ata_platform_remove_one,
	.shutdown = ahci_platform_shutdown,
	.driver = {
		.name = "hi3520dv400-ahci",
		.of_match_table = hi3520dv400_sata_match,
		.pm = pm_sleep_ptr(&hi3520dv400_sata_pm),
	},
};
module_platform_driver(hi3520dv400_sata_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Hi3520DV400 AHCI SATA controller");
