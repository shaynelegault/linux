// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple S5L USB PHY driver (otgphyctrl,s5l8940x).
 * Firmware can hand over after powering the PHY down. Initialize the common
 * clocks and PHY before either the gadget or the HSIC host uses the block.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>

#define OPHYPWR		0x00
#define OPHYCLK		0x04
#define ORSTCON	0x08
#define OPHYUNK1	0x1C
#define OPHYUNK4	0x60

#define OPHYPWR_ACTIVE			0x6
#define OPHYCLK_CLKSEL_MASK		0x3
#define ORSTCON_PHYSWRESET		BIT(0)

/* S5L8942X values from the native PHY power-on sequence. */
#define OPHYUNK4_START			0x200
#define OPHYUNK1_START			0x4
#define OPHYUNK1_OTG_ENABLE		BIT(1)
#define OPHY_UOTGTUNE1			0x40
#define OPHY_UOTGTUNE2			0x44
#define OPHY_UOTGTUNE1_DEVICE		0x549
#define OPHY_UOTGTUNE2_DEVICE		0x2ff3

#define PMGR_BASE		0x3F100000
#define PMGR_GATE_ON		0xF

/* usb-complex's power-state registers.
 *
 * The ADT gives usb-complex `clock-gates = <88 89 90 91 87>`, and those ids do
 * NOT index the register array.  Every id has its own register index in the
 * pmgr node's `device-clocks` table, and the id-to-index difference is not a
 * constant -- it is 10 across ids 56..83 and something else either side, so
 * *any* `base + id * 4` formula is a shortcut that holds in one neighbourhood
 * and silently misses everywhere else.  USB is one of the places it misses.
 * scripts/adt-pmgr-map.py prints the table; docs/research/p105-pmgr-gates.md
 * has the story.
 *
 *   id 88  USB-OTG    0x3f101084   the gadget: the console and the network
 *   id 89  USB-EHCI   0x3f101088   the host side, the Wi-Fi chip behind it
 *   id 90  USB-OHCI0  0x3f101090   EHCI's companion
 *   id 91  USB-OHCI1  0x3f101094
 *   id 87  USB-PHY    --           index 0: no register, nothing to gate
 *
 * This used to be `0x3F101008 + id * 4`, which put ids 88..91 at 0x1168..
 * 0x1174 -- addresses with no register behind them, which swallow writes and
 * read back zero.  The boot log said so all along: `PHY gate 87/88/89
 * pre=0x00000000` for three gates iBoot leaves ON and which must read ~0x2ff.
 * None of the five was ever switched; USB worked because iBoot leaves USB-OTG
 * on for its own DFU.
 *
 * `clock-ids` is a different property in a different namespace -- a reference
 * to a clock source, not to a gate.  otgphyctrl's `clock-ids = <5>` is
 * PREDIV3-CLK (clock register 0x3f10001c); usb-complex's `clock-ids = <292>`
 * is not a device-clocks entry at all.  Writing a gate value at `base + 5 * 4`
 * and `base + 292 * 4`, as this driver did, hit PERFCNT's gate and an empty
 * address respectively.  Neither has anything to do with USB. */
/* Native firmware enables UPERF, OTG and USBREG in this order. The ADT
 * confirms UPERF (id 40, index 0x20) and USBREG (id 42, index 0x26) as
 * USB's parents. USB-PHY's zero register index does not remove its parents.
 */
#define PMGR_UPERF		0x1080
#define PMGR_USB_OTG		0x1084
#define PMGR_USBREG		0x1098
#define PMGR_USB_EHCI		0x1088
#define PMGR_USB_OHCI0		0x1090

/* The host side: EHCI and its HSIC port, where the Wi-Fi chip is.
 *
 *   PMGR +0x1088   USB-EHCI's power-state register (ADT clock-gates id 89).
 *                  iBoot leaves it off, and EHCI and OHCI then read as
 *                  nothing at all; switched on, EHCI answers (HCIVERSION
 *                  0x0100, three ports).  Measured 2026-09-26.  The register
 *                  is right; the reasoning that first reached it -- 'reset
 *                  ids sit ten above the gate numbering' -- was not, see the
 *                  device-clocks note above.
 *   PMGR +0x1090   USB-OHCI0 (id 90), EHCI's companion controller, the other
 *                  usb-complex gate iBoot leaves off.  This used to be
 *                  +0x1140, read as 'gate 90' through the old formula; that
 *                  register is MCA, an audio block, and switching it on is
 *                  what actually happened every time EHCI came up.
 *   usb-complex    +0 bit 2: HSIC enable.  AppleS5L8930XUSBArbitrator's
 *                  _configureHSIC ORs the ADT's usb_ctl (0x64) into this
 *                  register (iOS 6.1 kernelcache 0x808afffc); bits 5/6 there
 *                  are clock-off bits it clears again for active clients, so
 *                  bit 2 is what it leaves.  Bit 7 is the OTG PHY's own
 *                  (set on power-up, 0x344 on the same object) -- not ours.
 *   hsic-supply    the Wi-Fi chip's REG_ON, PMU GPIO3.  With all three on
 *                  and the port powered, PORTSC3 read 0x1803: connected.
 *
 * The regulator is taken when the host PHY powers on, not at probe: the PMU
 * sits behind I2C and may come later, and the OTG PHY -- the gadget, the
 * console, the network -- must not wait for it. */
#define USBCPLX_HSIC_EN		BIT(2)

struct s5l_usbphy_soc_data {
	bool cold_init;
};

static const struct s5l_usbphy_soc_data s5l8942x_usbphy_data = {
	.cold_init = true,
};

struct s5l_usbphy {
	void __iomem *base;
	void __iomem *pmgr;
	void __iomem *usbcplx;		/* NULL without a second reg */
	struct regulator *hsic_supply;
	bool hsic_supply_on;
	struct regulator *lpo_supply;	/* the chip's 32 kHz sleep clock */
	bool lpo_supply_on;
	struct device *dev;
	struct clk *refclk;
	const struct s5l_usbphy_soc_data *data;
	struct mutex lock;
	unsigned int init_users;
	struct phy *phys[2];		/* [0] OTG, [1] host/HSIC */
};

/* XNU's clock_gate_switch: request the state in bits 3:0, wait for bits 7:4
 * to follow.  Bit 8 is cleared with the request, as XNU does. */
static int s5l_pmgr_on(struct s5l_usbphy *p, u32 off)
{
	u32 v = readl(p->pmgr + off);
	int n;

	writel((v & ~0x10f) | PMGR_GATE_ON, p->pmgr + off);
	for (n = 0; n < 1000; n++) {
		v = readl(p->pmgr + off);
		if ((v & 0xff) == 0xff)
			return 0;
		udelay(1);
	}
	dev_err(p->dev, "PMGR +0x%x stuck at 0x%08x\n", off, v);
	return -ETIMEDOUT;
}

static int s5l_usbphy_clk_sel(unsigned long rate)
{
	switch (rate) {
	case 12000000:
		return 0;
	case 24000000:
		return 1;
	case 48000000:
		return 2;
	default:
		return -EINVAL;
	}
}

static bool s5l_usbphy_is_ready(struct s5l_usbphy *p)
{
	int clksel = s5l_usbphy_clk_sel(clk_get_rate(p->refclk));

	return clksel >= 0 &&
	       (readl(p->base + OPHYCLK) & OPHYCLK_CLKSEL_MASK) == clksel &&
	       readl(p->base + OPHYPWR) == OPHYPWR_ACTIVE &&
	       !(readl(p->base + ORSTCON) & ORSTCON_PHYSWRESET) &&
	       readl(p->base + OPHY_UOTGTUNE1) == OPHY_UOTGTUNE1_DEVICE &&
	       readl(p->base + OPHY_UOTGTUNE2) == OPHY_UOTGTUNE2_DEVICE &&
	       readl(p->base + OPHYUNK4) == OPHYUNK4_START;
}

static int s5l_usbphy_cold_init(struct s5l_usbphy *p)
{
	unsigned long rate = clk_get_rate(p->refclk);
	int clksel = s5l_usbphy_clk_sel(rate);
	u32 v;

	if (clksel < 0) {
		dev_err(p->dev, "unsupported PHY reference clock: %lu Hz\n", rate);
		return clksel;
	}

	writel(OPHYUNK4_START, p->base + OPHYUNK4);
	writel(OPHYPWR_ACTIVE, p->base + OPHYPWR);
	writel(OPHYUNK1_START, p->base + OPHYUNK1);
	writel(OPHY_UOTGTUNE1_DEVICE, p->base + OPHY_UOTGTUNE1);
	writel(OPHY_UOTGTUNE2_DEVICE, p->base + OPHY_UOTGTUNE2);
	udelay(10);

	v = readl(p->base + OPHYCLK);
	writel((v & ~OPHYCLK_CLKSEL_MASK) | clksel, p->base + OPHYCLK);
	if ((readl(p->base + OPHYCLK) & OPHYCLK_CLKSEL_MASK) != clksel) {
		dev_err(p->dev, "PHY reference clock selection did not latch\n");
		return -EIO;
	}

	return 0;
}

static int s5l_usbphy_init(struct phy *phy)
{
	struct s5l_usbphy *p = phy_get_drvdata(phy);
	u32 v;
	int ret = 0;

	mutex_lock(&p->lock);
	if (p->init_users) {
		p->init_users++;
		goto out;
	}

	if (p->data && p->data->cold_init) {
		ret = clk_prepare_enable(p->refclk);
		if (ret)
			goto out;

		/* Native firmware enables these parents in this order. */
		if ((readl(p->pmgr + PMGR_UPERF) & 0xff) != 0xff ||
		    (readl(p->pmgr + PMGR_USB_OTG) & 0xff) != 0xff ||
		    (readl(p->pmgr + PMGR_USBREG) & 0xff) != 0xff)
			usleep_range(10000, 11000);

		ret = s5l_pmgr_on(p, PMGR_UPERF);
		if (ret)
			goto disable_clock;
	}

	ret = s5l_pmgr_on(p, PMGR_USB_OTG);
	if (ret)
		goto disable_clock;
	if (p->data && p->data->cold_init) {
		ret = s5l_pmgr_on(p, PMGR_USBREG);
		if (ret)
			goto disable_clock;
	}
	udelay(100);

	/* Legacy compatibles retain the tuning established by firmware. */
	if (p->data && p->data->cold_init && !s5l_usbphy_is_ready(p)) {
		ret = s5l_usbphy_cold_init(p);
		if (ret)
			goto disable_clock;
	}
	udelay(10);

	writel(readl(p->base + ORSTCON) | ORSTCON_PHYSWRESET,
	       p->base + ORSTCON);
	udelay(20);
	writel(readl(p->base + ORSTCON) & ~ORSTCON_PHYSWRESET,
	       p->base + ORSTCON);
	udelay(1000);
	ret = readl_poll_timeout(p->base + ORSTCON, v,
				 !(v & ORSTCON_PHYSWRESET), 1, 1000);
	if (ret) {
		dev_err(p->dev, "PHY reset did not clear: 0x%08x\n", v);
		goto disable_clock;
	}
	if (p->data && p->data->cold_init && !s5l_usbphy_is_ready(p)) {
		dev_err(p->dev, "PHY power or tuning did not latch\n");
		ret = -EIO;
		goto disable_clock;
	}
	p->init_users = 1;
	goto out;

disable_clock:
	if (p->data && p->data->cold_init)
		clk_disable_unprepare(p->refclk);
out:
	mutex_unlock(&p->lock);
	return ret;
}

static int s5l_usbphy_exit(struct phy *phy)
{
	struct s5l_usbphy *p = phy_get_drvdata(phy);

	mutex_lock(&p->lock);
	if (p->init_users && !--p->init_users &&
	    p->data && p->data->cold_init)
		clk_disable_unprepare(p->refclk);
	mutex_unlock(&p->lock);
	return 0;
}

/* Native USB connect enables the OTG PHY before clearing DCTL's soft
 * disconnect. Initializing the shared PHY alone leaves this bit clear.
 * Keep it in the OTG output's power callbacks so HSIC remains independent.
 */
static int s5l_usbphy_set_otg(struct phy *phy, bool enable)
{
	struct s5l_usbphy *p = phy_get_drvdata(phy);
	u32 value, expected;
	int ret = 0;

	if (!p->data || !p->data->cold_init)
		return 0;

	mutex_lock(&p->lock);
	if (!p->init_users) {
		ret = -EIO;
		goto out;
	}
	value = readl(p->base + OPHYUNK1);
	expected = enable ? OPHYUNK1_OTG_ENABLE : 0;
	writel((value & ~OPHYUNK1_OTG_ENABLE) | expected,
	       p->base + OPHYUNK1);
	if ((readl(p->base + OPHYUNK1) & OPHYUNK1_OTG_ENABLE) != expected) {
		dev_err(p->dev, "OTG PHY power state did not latch\n");
		ret = -EIO;
	}
out:
	mutex_unlock(&p->lock);
	return ret;
}

static int s5l_usbphy_power_on(struct phy *phy)
{
	return s5l_usbphy_set_otg(phy, true);
}

static int s5l_usbphy_power_off(struct phy *phy)
{
	return s5l_usbphy_set_otg(phy, false);
}

static const struct phy_ops s5l_usbphy_ops = {
	.init      = s5l_usbphy_init,
	.exit      = s5l_usbphy_exit,
	.power_on  = s5l_usbphy_power_on,
	.power_off = s5l_usbphy_power_off,
	.owner     = THIS_MODULE,
};

static int s5l_hostphy_power_on(struct phy *phy)
{
	struct s5l_usbphy *p = phy_get_drvdata(phy);
	int ret;

	/* The 32 kHz clock first, the way a chip expects its sleep clock
	 * before it is let out of reset.  Optional like hsic-supply. */
	if (!p->lpo_supply) {
		struct regulator *r = devm_regulator_get_optional(p->dev, "lpo");

		if (IS_ERR(r)) {
			if (PTR_ERR(r) == -EPROBE_DEFER)
				return -EPROBE_DEFER;
		} else {
			p->lpo_supply = r;
		}
	}
	if (p->lpo_supply && !p->lpo_supply_on) {
		ret = regulator_enable(p->lpo_supply);
		if (ret)
			return ret;
		p->lpo_supply_on = true;
		msleep(10);
	}

	if (!p->hsic_supply) {
		struct regulator *r = devm_regulator_get_optional(p->dev, "hsic");

		if (IS_ERR(r)) {
			if (PTR_ERR(r) == -EPROBE_DEFER)
				return -EPROBE_DEFER;
			dev_warn(p->dev, "no hsic-supply (%ld): the HSIC device stays unpowered\n",
				 PTR_ERR(r));
		} else {
			p->hsic_supply = r;
		}
	}
	if (p->hsic_supply && !p->hsic_supply_on) {
		ret = regulator_enable(p->hsic_supply);
		if (ret)
			return ret;
		p->hsic_supply_on = true;
		msleep(10);
	}

	ret = s5l_pmgr_on(p, PMGR_USB_EHCI);
	if (ret)
		return ret;
	ret = s5l_pmgr_on(p, PMGR_USB_OHCI0);
	if (ret)
		return ret;
	if (p->usbcplx)
		writel(readl(p->usbcplx) | USBCPLX_HSIC_EN, p->usbcplx);

	dev_dbg(p->dev, "host PHY on: EHCI(89)=0x%08x OHCI0(90)=0x%08x usb-complex=0x%08x\n",
		 readl(p->pmgr + PMGR_USB_EHCI), readl(p->pmgr + PMGR_USB_OHCI0),
		 p->usbcplx ? readl(p->usbcplx) : 0);
	return 0;
}

static int s5l_hostphy_power_off(struct phy *phy)
{
	struct s5l_usbphy *p = phy_get_drvdata(phy);

	if (p->hsic_supply_on) {
		regulator_disable(p->hsic_supply);
		p->hsic_supply_on = false;
	}
	if (p->lpo_supply_on) {
		regulator_disable(p->lpo_supply);
		p->lpo_supply_on = false;
	}
	return 0;
}


static int s5l_hostphy_init(struct phy *phy)
{
	struct s5l_usbphy *p = phy_get_drvdata(phy);

	return p->data && p->data->cold_init ? s5l_usbphy_init(phy) : 0;
}

static int s5l_hostphy_exit(struct phy *phy)
{
	struct s5l_usbphy *p = phy_get_drvdata(phy);

	return p->data && p->data->cold_init ? s5l_usbphy_exit(phy) : 0;
}

static const struct phy_ops s5l_hostphy_ops = {
	.init      = s5l_hostphy_init,
	.exit      = s5l_hostphy_exit,
	.power_on  = s5l_hostphy_power_on,
	.power_off = s5l_hostphy_power_off,
	.owner     = THIS_MODULE,
};

/* <&otgphy 0> is the OTG PHY dwc2 has always had, <&otgphy 1> the host
 * side.  With #phy-cells = <0> there are no args, and that is the OTG PHY. */
static struct phy *s5l_usbphy_xlate(struct device *dev,
				    const struct of_phandle_args *args)
{
	struct s5l_usbphy *p = dev_get_drvdata(dev);
	unsigned int idx = args->args_count ? args->args[0] : 0;

	if (idx >= ARRAY_SIZE(p->phys))
		return ERR_PTR(-EINVAL);
	return p->phys[idx];
}

static int s5l_usbphy_probe(struct platform_device *pdev)
{
	struct s5l_usbphy *p;
	struct phy *phy;
	struct phy_provider *provider;

	p = devm_kzalloc(&pdev->dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	p->dev = &pdev->dev;
	p->data = device_get_match_data(&pdev->dev);
	mutex_init(&p->lock);
	if (p->data && p->data->cold_init) {
		p->refclk = devm_clk_get(&pdev->dev, "ref");
		if (IS_ERR(p->refclk))
			return dev_err_probe(&pdev->dev, PTR_ERR(p->refclk),
					     "failed to get PHY reference clock\n");
	}

	p->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(p->base)) {
		dev_err(&pdev->dev, "failed to ioremap PHY regs\n");
		return PTR_ERR(p->base);
	}

	p->pmgr = devm_ioremap(&pdev->dev, PMGR_BASE, 0x2000);
	if (!p->pmgr) {
		dev_err(&pdev->dev, "failed to ioremap PMGR\n");
		return -ENOMEM;
	}

	/* usb-complex, for the HSIC enable bit: optional, the second reg. */
	if (platform_get_resource(pdev, IORESOURCE_MEM, 1)) {
		p->usbcplx = devm_platform_ioremap_resource(pdev, 1);
		if (IS_ERR(p->usbcplx))
			p->usbcplx = NULL;
	}
	platform_set_drvdata(pdev, p);

	phy = devm_phy_create(&pdev->dev, NULL, &s5l_usbphy_ops);
	if (IS_ERR(phy)) {
		dev_err(&pdev->dev, "failed to create PHY\n");
		return PTR_ERR(phy);
	}
	phy_set_drvdata(phy, p);
	p->phys[0] = phy;

	phy = devm_phy_create(&pdev->dev, NULL, &s5l_hostphy_ops);
	if (IS_ERR(phy)) {
		dev_err(&pdev->dev, "failed to create host PHY\n");
		return PTR_ERR(phy);
	}
	phy_set_drvdata(phy, p);
	p->phys[1] = phy;

	provider = devm_of_phy_provider_register(&pdev->dev, s5l_usbphy_xlate);
	if (IS_ERR(provider))
		return PTR_ERR(provider);

	return 0;
}

static const struct of_device_id s5l_usbphy_of_match[] = {
	{ .compatible = "apple,s5l8942x-otgphy", .data = &s5l8942x_usbphy_data },
	{ .compatible = "apple,s5l8940x-otgphy" },
	{ .compatible = "apple,s5l8930x-otgphy" },
	{ },
};
MODULE_DEVICE_TABLE(of, s5l_usbphy_of_match);

static struct platform_driver s5l_usbphy_driver = {
	.probe  = s5l_usbphy_probe,
	.driver = {
		.name           = "phy-apple-s5l-usb",
		.of_match_table = s5l_usbphy_of_match,
	},
};
module_platform_driver(s5l_usbphy_driver);

MODULE_DESCRIPTION("Apple S5L USB PHY driver");
MODULE_LICENSE("GPL v2");
