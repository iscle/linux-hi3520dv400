// SPDX-License-Identifier: GPL-2.0-or-later
/* Hi3520DV400 clock/reset controller. Register layout from HiSilicon SDK. */
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>
#include <dt-bindings/clock/hisilicon,hi3520dv400-crg.h>

#define CRG_AXI_CLKSEL		0x50
#define CRG_ETH			0x14c
#define CRG_UART		0x154
#define AXI_CLKSEL_SHIFT	2
#define AXI_CLKSEL_MASK		3

struct hi3520dv400_crg {
	void __iomem *base;
	/* Serializes clock and reset updates to the shared Ethernet register. */
	spinlock_t lock;
	struct reset_controller_dev rcdev;
	struct clk_hw_onecell_data *clks;
};

static const u8 hi3520dv400_reset_bits[] = { 0, 2, 5 };

static int hi3520dv400_reset_update(struct reset_controller_dev *rcdev,
				    unsigned long id, bool assert)
{
	struct hi3520dv400_crg *crg = container_of(rcdev,
					struct hi3520dv400_crg, rcdev);
	unsigned long flags;
	u32 value, mask = BIT(hi3520dv400_reset_bits[id]);

	spin_lock_irqsave(&crg->lock, flags);
	value = readl(crg->base + CRG_ETH);
	writel(assert ? value | mask : value & ~mask, crg->base + CRG_ETH);
	spin_unlock_irqrestore(&crg->lock, flags);
	return 0;
}

static int hi3520dv400_reset_assert(struct reset_controller_dev *rcdev,
				    unsigned long id)
{
	return hi3520dv400_reset_update(rcdev, id, true);
}

static int hi3520dv400_reset_deassert(struct reset_controller_dev *rcdev,
				      unsigned long id)
{
	return hi3520dv400_reset_update(rcdev, id, false);
}

static const struct reset_control_ops hi3520dv400_reset_ops = {
	.assert = hi3520dv400_reset_assert,
	.deassert = hi3520dv400_reset_deassert,
};

static int hi3520dv400_crg_probe(struct platform_device *pdev)
{
	static const unsigned long axi_rates[] = {
		24000000, 200000000, 250000000, 300000000,
	};
	static const struct {
		unsigned int id;
		const char *name;
		const char *parent;
		u16 offset;
		u8 bit;
	} gates[] = {
		{ HI3520DV400_CLK_UART0, "hi3520dv400-uart0", "hi3520dv400-24m", CRG_UART, 15 },
		{ HI3520DV400_CLK_ETH_PHY, "hi3520dv400-eth-phy", NULL, CRG_ETH, 6 },
		{ HI3520DV400_CLK_ETH_BUS, "hi3520dv400-eth-bus",
		  "hi3520dv400-eth-phy", CRG_ETH, 7 },
		{ HI3520DV400_CLK_ETH, "hi3520dv400-eth", "hi3520dv400-eth-bus", CRG_ETH, 1 },
		{ HI3520DV400_CLK_ETH_MACIF, "hi3520dv400-macif",
		  "hi3520dv400-eth-bus", CRG_ETH, 3 },
	};
	struct device *dev = &pdev->dev;
	struct hi3520dv400_crg *crg;
	struct clk_hw **hws;
	unsigned int i, sel;
	int ret;

	crg = devm_kzalloc(dev, sizeof(*crg), GFP_KERNEL);
	if (!crg)
		return -ENOMEM;
	crg->clks = devm_kzalloc(dev, struct_size(crg->clks, hws,
						  HI3520DV400_CLK_NR), GFP_KERNEL);
	if (!crg->clks)
		return -ENOMEM;
	crg->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(crg->base))
		return PTR_ERR(crg->base);
	spin_lock_init(&crg->lock);
	crg->clks->num = HI3520DV400_CLK_NR;
	hws = crg->clks->hws;
	sel = (readl(crg->base + CRG_AXI_CLKSEL) >> AXI_CLKSEL_SHIFT) &
	      AXI_CLKSEL_MASK;
	hws[HI3520DV400_CLK_APB] =
		devm_clk_hw_register_fixed_rate(dev, "hi3520dv400-apb", NULL,
						0, axi_rates[sel] / 4);
	hws[HI3520DV400_CLK_24M] =
		devm_clk_hw_register_fixed_rate(dev, "hi3520dv400-24m", NULL,
						0, 24000000);
	for (i = 0; i < ARRAY_SIZE(gates); i++)
		hws[gates[i].id] = devm_clk_hw_register_gate(dev,
			gates[i].name, gates[i].parent, 0,
			crg->base + gates[i].offset, gates[i].bit, 0, &crg->lock);
	for (i = 0; i < HI3520DV400_CLK_NR; i++) {
		if (IS_ERR(hws[i]))
			return dev_err_probe(dev, PTR_ERR(hws[i]),
					     "failed to register clock %u\n", i);
	}
	crg->rcdev.owner = THIS_MODULE;
	crg->rcdev.ops = &hi3520dv400_reset_ops;
	crg->rcdev.of_node = dev->of_node;
	crg->rcdev.nr_resets = ARRAY_SIZE(hi3520dv400_reset_bits);
	ret = devm_reset_controller_register(dev, &crg->rcdev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register resets\n");
	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, crg->clks);
}

static const struct of_device_id hi3520dv400_crg_match[] = {
	{ .compatible = "hisilicon,hi3520dv400-crg" },
	{ }
};
MODULE_DEVICE_TABLE(of, hi3520dv400_crg_match);

static struct platform_driver hi3520dv400_crg_driver = {
	.probe = hi3520dv400_crg_probe,
	.driver = {
		.name = "hi3520dv400-crg",
		.of_match_table = hi3520dv400_crg_match,
	},
};
module_platform_driver(hi3520dv400_crg_driver);

MODULE_DESCRIPTION("HiSilicon Hi3520DV400 clock and reset controller");
MODULE_LICENSE("GPL");
