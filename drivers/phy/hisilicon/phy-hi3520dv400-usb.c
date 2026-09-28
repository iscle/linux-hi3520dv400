// SPDX-License-Identifier: GPL-2.0-or-later
/* Hi3520DV400 USB2 PHY, using the register sequence from the stock driver. */
#include <linux/delay.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define USB_CRG_CTRL		0x130
#define USB_CRG_PHY		0x134
#define USB_CTRL_RESETS		(BIT(12) | BIT(13) | BIT(14) | BIT(16))
#define USB_CTRL_CLOCKS		(BIT(0) | BIT(1) | BIT(2) | BIT(4) | BIT(5) | BIT(6))
#define USB_PHY_POR		BIT(8)
#define USB_PHY_PORT_RESETS	(BIT(9) | BIT(10))
#define USB_PHY_TEST_RESET	BIT(11)
#define USB_PHY_REF_CLOCK	BIT(0)
#define USB_PHY_REF_SELECT	BIT(16)
#define MISC_USB_CTRL		0x50
#define USB_WORD_INTERFACE	BIT(0)
#define USB_BURST16_ENABLE	BIT(9)
#define MISC_TEST_ACCESS	0x10
#define USB_TEST_ACCESS		BIT(20)

struct hi3520dv400_usb_phy {
	struct regmap *crg;
	struct regmap *misc;
};

/* Analog setup used by the supplied Dahua stock kernel's hi3521d-usb2-phy.
 * Offsets are the PHY test-register window in the MISC controller.
 */
static const struct reg_sequence hi3520dv400_usb_tuning[] = {
	{ 0x8018, 0x0c, 2000 }, /* reference clock */
	{ 0x8028, 0x01, 20 },   /* port 0 disconnect threshold */
	{ 0x8428, 0x01, 20 },   /* port 1 disconnect threshold */
	{ 0x8018, 0x0c, 20 },
	{ 0x8014, 0x92, 20 },   /* comparator threshold */
	{ 0x8024, 0x04, 20 },   /* port 0 pre-emphasis */
	{ 0x8424, 0x04, 20 },   /* port 1 pre-emphasis */
	{ 0x8420, 0xc4, 20 },   /* port 1 pre-driver */
	{ 0x8020, 0xc4, 20 },   /* port 0 pre-driver */
};

static int hi3520dv400_usb_update(struct regmap *map, unsigned int reg,
				  unsigned int mask, unsigned int val,
				unsigned int delay_us)
{
	int ret;

	ret = regmap_update_bits(map, reg, mask, val);
	if (ret)
		return ret;
	usleep_range(delay_us, delay_us + 50);
	return 0;
}

static int hi3520dv400_usb_power_off(struct phy *phy)
{
	struct hi3520dv400_usb_phy *priv = phy_get_drvdata(phy);
	int ret;

	ret = hi3520dv400_usb_update(priv->crg, USB_CRG_CTRL,
				     USB_CTRL_RESETS, USB_CTRL_RESETS, 200);
	if (ret)
		return ret;
	ret = hi3520dv400_usb_update(priv->crg, USB_CRG_PHY,
				     USB_PHY_POR | USB_PHY_PORT_RESETS,
			USB_PHY_POR | USB_PHY_PORT_RESETS, 100);
	if (ret)
		return ret;
	ret = hi3520dv400_usb_update(priv->crg, USB_CRG_PHY,
				     USB_PHY_REF_CLOCK | USB_PHY_REF_SELECT, 0, 300);
	if (ret)
		return ret;
	return hi3520dv400_usb_update(priv->crg, USB_CRG_CTRL,
				     USB_CTRL_CLOCKS, 0, 200);
}

static int hi3520dv400_usb_power_on(struct phy *phy)
{
	struct hi3520dv400_usb_phy *priv = phy_get_drvdata(phy);
	int ret;

	ret = hi3520dv400_usb_update(priv->crg, USB_CRG_CTRL,
				     USB_CTRL_RESETS, USB_CTRL_RESETS, 200);
	if (ret)
		return ret;
	ret = hi3520dv400_usb_update(priv->crg, USB_CRG_PHY,
				     USB_PHY_POR | USB_PHY_PORT_RESETS,
			USB_PHY_POR | USB_PHY_PORT_RESETS, 200);
	if (ret)
		goto power_off;
	ret = regmap_update_bits(priv->misc, MISC_USB_CTRL,
				 USB_WORD_INTERFACE | USB_BURST16_ENABLE, 0);
	if (ret)
		goto power_off;
	msleep(20);
	ret = hi3520dv400_usb_update(priv->crg, USB_CRG_PHY,
				     USB_PHY_REF_CLOCK, USB_PHY_REF_CLOCK, 100);
	if (ret)
		goto power_off;
	ret = hi3520dv400_usb_update(priv->crg, USB_CRG_PHY,
				     USB_PHY_POR | USB_PHY_TEST_RESET, 0, 300);
	if (ret)
		goto power_off;
	ret = hi3520dv400_usb_update(priv->misc, MISC_TEST_ACCESS,
				     USB_TEST_ACCESS, USB_TEST_ACCESS, 2);
	if (ret)
		goto power_off;
	ret = regmap_multi_reg_write(priv->misc, hi3520dv400_usb_tuning,
				     ARRAY_SIZE(hi3520dv400_usb_tuning));
	if (ret)
		goto power_off;
	ret = hi3520dv400_usb_update(priv->crg, USB_CRG_PHY,
				     USB_PHY_PORT_RESETS, 0, 300);
	if (ret)
		goto power_off;
	ret = hi3520dv400_usb_update(priv->crg, USB_CRG_CTRL,
				     USB_CTRL_RESETS | USB_CTRL_CLOCKS, USB_CTRL_CLOCKS, 200);
	if (!ret)
		return 0;
power_off:
	if (hi3520dv400_usb_power_off(phy))
		dev_warn(&phy->dev, "failed to power off after initialization failure\n");
	return ret;
}

static const struct phy_ops hi3520dv400_usb_ops = {
	.power_on = hi3520dv400_usb_power_on,
	.power_off = hi3520dv400_usb_power_off,
	.owner = THIS_MODULE,
};

static int hi3520dv400_usb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hi3520dv400_usb_phy *priv;
	struct phy_provider *provider;
	struct phy *phy;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->crg = syscon_regmap_lookup_by_phandle(dev->of_node, "hisilicon,crg");
	if (IS_ERR(priv->crg))
		return dev_err_probe(dev, PTR_ERR(priv->crg), "missing CRG controller\n");
	priv->misc = syscon_regmap_lookup_by_phandle(dev->of_node, "hisilicon,misc");
	if (IS_ERR(priv->misc))
		return dev_err_probe(dev, PTR_ERR(priv->misc), "missing MISC controller\n");
	phy = devm_phy_create(dev, NULL, &hi3520dv400_usb_ops);
	if (IS_ERR(phy))
		return dev_err_probe(dev, PTR_ERR(phy), "failed to create PHY\n");
	phy_set_drvdata(phy, priv);
	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id hi3520dv400_usb_match[] = {
	{ .compatible = "hisilicon,hi3520dv400-usb2-phy" },
	{ }
};
MODULE_DEVICE_TABLE(of, hi3520dv400_usb_match);

static struct platform_driver hi3520dv400_usb_driver = {
	.probe = hi3520dv400_usb_probe,
	.driver = {
		.name = "hi3520dv400-usb2-phy",
		.of_match_table = hi3520dv400_usb_match,
	},
};
module_platform_driver(hi3520dv400_usb_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HiSilicon Hi3520DV400 USB2 PHY");
