/* linux/drivers/usb/phy/phy-samsung-usb3.c
 *
 * Copyright (c) 2013 Samsung Electronics Co., Ltd.
 *              http://www.samsung.com
 *
 * Author: Vivek Gautam <gautam.vivek@samsung.com>
 *
 * Samsung USB 3.0 PHY transceiver; talks to DWC3 controller.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/usb/samsung_usb_phy.h>
#include <linux/platform_data/samsung-usbphy.h>
#include <linux/usb/phy.h>
#include "phy-samsung-usb.h"

/*
 * Sets the phy clk as EXTREFCLK (XXTI) which is internal clock from clock core.
 */
static u32 samsung_usb3phy_set_refclk(struct samsung_usbphy *sphy)
{
	u32 reg;
	u32 refclk;

	refclk = sphy->ref_clk_freq;

	reg = PHYCLKRST_REFCLKSEL_EXT_REFCLK |
		PHYCLKRST_FSEL(refclk);

	switch (refclk) {
	case FSEL_CLKSEL_50M:
		reg |= (PHYCLKRST_MPLL_MULTIPLIER_50M_REF |
			PHYCLKRST_SSC_REFCLKSEL(0x00));
		break;
	case FSEL_CLKSEL_20M:
		reg |= (PHYCLKRST_MPLL_MULTIPLIER_20MHZ_REF |
			PHYCLKRST_SSC_REFCLKSEL(0x00));
		break;
	case FSEL_CLKSEL_19200K:
		reg |= (PHYCLKRST_MPLL_MULTIPLIER_19200KHZ_REF |
			PHYCLKRST_SSC_REFCLKSEL(0x88));
		break;
	case FSEL_CLKSEL_24M:
	default:
		reg |= (PHYCLKRST_MPLL_MULTIPLIER_24MHZ_REF |
			PHYCLKRST_SSC_REFCLKSEL(0x88));
		break;
	}

	return reg;
}

static int samsung_exynos5_usb3phy_enable(struct samsung_usbphy *sphy)
{
	void __iomem *regs = sphy->regs;
	u32 phyparam0;
	u32 phyparam1;
	u32 linksystem;
	u32 phyclksel;
	u32 phytest;
	u32 phyclkrst;

	/* Reset USB 3.0 PHY */
	writel(0x0, regs + EXYNOS5_DRD_PHYREG0);

	phyparam0 = readl(regs + EXYNOS5_DRD_PHYPARAM0);
	/* Select PHY CLK source */
	phyparam0 &= ~PHYPARAM0_REF_USE_PAD;
	/* Set Loss-of-Signal Detector sensitivity */
	phyparam0 &= ~PHYPARAM0_REF_LOSLEVEL_MASK;
	phyparam0 |= PHYPARAM0_REF_LOSLEVEL;
	writel(phyparam0, regs + EXYNOS5_DRD_PHYPARAM0);

	writel(0x0, regs + EXYNOS5_DRD_PHYRESUME);

	/*
	 * Setting the Frame length Adj value[6:1] to default 0x20
	 * See xHCI 1.0 spec, 5.2.4
	 */
	linksystem = LINKSYSTEM_XHCI_VERSION_CONTROL |
			LINKSYSTEM_FLADJ(0x20);
	writel(linksystem, regs + EXYNOS5_DRD_LINKSYSTEM);

	phyparam1 = readl(regs + EXYNOS5_DRD_PHYPARAM1);
	/* Set Tx De-Emphasis level */
	phyparam1 &= ~PHYPARAM1_PCS_TXDEEMPH_MASK;
	phyparam1 |= PHYPARAM1_PCS_TXDEEMPH;
	writel(phyparam1, regs + EXYNOS5_DRD_PHYPARAM1);

	if (sphy->drv_data->cpu_type == TYPE_EXYNOS5430) {
		phyclksel = readl(regs + EXYNOS5_DRD_PHYPIPE);
		phyclksel |= PHY_CLOCK_SEL;
		writel(phyclksel, regs + EXYNOS5_DRD_PHYPIPE);
	} else {
		phyclksel = readl(regs + EXYNOS5_DRD_PHYBATCHG);
		phyclksel |= PHYBATCHG_UTMI_CLKSEL;
		writel(phyclksel, regs + EXYNOS5_DRD_PHYBATCHG);
	}

	/* PHYTEST POWERDOWN Control */
	phytest = readl(regs + EXYNOS5_DRD_PHYTEST);
	phytest &= ~(PHYTEST_POWERDOWN_SSP |
			PHYTEST_POWERDOWN_HSP);
	writel(phytest, regs + EXYNOS5_DRD_PHYTEST);

	/* UTMI Power Control */
	writel(PHYUTMI_OTGDISABLE, regs + EXYNOS5_DRD_PHYUTMI);

	phyclkrst = samsung_usb3phy_set_refclk(sphy);

	phyclkrst |= PHYCLKRST_PORTRESET |
			/* Digital power supply in normal operating mode */
			PHYCLKRST_RETENABLEN |
			/* Enable ref clock for SS function */
			PHYCLKRST_REF_SSP_EN |
			/* Enable spread spectrum */
			PHYCLKRST_SSC_EN |
			/* Power down HS Bias and PLL blocks in suspend mode */
			PHYCLKRST_COMMONONN;
	/* Disable utmi_suspend_com_n of LINK */
	phyclkrst &= ~(PHYCLKRST_EN_UTMISUSPEND);

	writel(phyclkrst, regs + EXYNOS5_DRD_PHYCLKRST);

	udelay(10);

	phyclkrst &= ~(PHYCLKRST_PORTRESET);
	writel(phyclkrst, regs + EXYNOS5_DRD_PHYCLKRST);

	return 0;
}

static void samsung_exynos5_usb3phy_disable(struct samsung_usbphy *sphy)
{
	u32 phyutmi;
	u32 phyclkrst;
	u32 phytest;
	void __iomem *regs = sphy->regs;

	phyutmi = PHYUTMI_OTGDISABLE |
			PHYUTMI_FORCESUSPEND |
			PHYUTMI_FORCESLEEP;
	writel(phyutmi, regs + EXYNOS5_DRD_PHYUTMI);

	/* Resetting the PHYCLKRST enable bits to reduce leakage current */
	phyclkrst = readl(regs + EXYNOS5_DRD_PHYCLKRST);
	phyclkrst &= ~(PHYCLKRST_REF_SSP_EN |
			PHYCLKRST_SSC_EN |
			PHYCLKRST_COMMONONN |
			PHYCLKRST_EN_UTMISUSPEND);
	writel(phyclkrst, regs + EXYNOS5_DRD_PHYCLKRST);

	/* Control PHYTEST to remove leakage current */
	phytest = readl(regs + EXYNOS5_DRD_PHYTEST);
	phytest |= PHYTEST_POWERDOWN_SSP;
	/*
	 * To save power, it is supposed to be set to POWERDOWN mode.
	 * However, in Exynos 5430 & 5433,
	 * Even when HSP is set to POWERDOWN mode, there is current leakage.
	 * Therefore, it is recommended not to set HSP to POWERDOWN mode.
	 */
	if (sphy->drv_data->cpu_type != TYPE_EXYNOS5430)
		phytest |= PHYTEST_POWERDOWN_HSP;
	writel(phytest, regs + EXYNOS5_DRD_PHYTEST);
}

static void samsung_usb3phy_crport_handshake(struct samsung_usbphy *sphy,
							u32 val, u32 cmd)
{
	u32 usec = 100;
	u32 result;

	writel(val | cmd, sphy->regs + EXYNOS5_DRD_PHYREG0);

	do {
		result = readl(sphy->regs + EXYNOS5_DRD_PHYREG1);
		if (result & EXYNOS5_DRD_PHYREG1_CR_ACK)
			break;

		udelay(1);
	} while (usec-- > 0);

	if (!usec)
		dev_err(sphy->dev, "CRPORT handshake timeout1 (0x%08x)\n", val);

	usec = 100;

	writel(val, sphy->regs + EXYNOS5_DRD_PHYREG0);

	do {
		result = readl(sphy->regs + EXYNOS5_DRD_PHYREG1);
		if (!(result & EXYNOS5_DRD_PHYREG1_CR_ACK))
			break;

		udelay(1);
	} while (usec-- > 0);

	if (!usec)
		dev_err(sphy->dev, "CRPORT handshake timeout2 (0x%08x)\n", val);
}

static u32 samsung_usb3phy_crport_read(struct usb_phy *phy, u32 addr)
{
	struct samsung_usbphy *sphy;
	u32 usec = 100;
	u32 result;

	sphy = phy_to_sphy(phy);

	writel(EXYNOS5_DRD_PHYREG0_CR_DATA_IN(addr),
			sphy->regs + EXYNOS5_DRD_PHYREG0);
	samsung_usb3phy_crport_handshake(sphy,
			EXYNOS5_DRD_PHYREG0_CR_DATA_IN(addr),
			EXYNOS5_DRD_PHYREG0_CR_CR_CAP_ADDR);

	writel(EXYNOS5_DRD_PHYREG0_CR_READ, sphy->regs + EXYNOS5_DRD_PHYREG0);

	do {
		result = readl(sphy->regs + EXYNOS5_DRD_PHYREG1);
		if (result & EXYNOS5_DRD_PHYREG1_CR_ACK)
			break;
		udelay(1);
	} while (usec-- > 0);

	if (!usec)
		dev_err(sphy->dev, "CRPORT handshake timeout\n");

	return readl(sphy->regs + EXYNOS5_DRD_PHYREG1) >> 1;
}

static void samsung_usb3phy_crport_ctrl(struct samsung_usbphy *sphy,
							u32 addr, u32 data)
{
	/* Write Address */
	writel(EXYNOS5_DRD_PHYREG0_CR_DATA_IN(addr),
			sphy->regs + EXYNOS5_DRD_PHYREG0);
	samsung_usb3phy_crport_handshake(sphy,
			EXYNOS5_DRD_PHYREG0_CR_DATA_IN(addr),
			EXYNOS5_DRD_PHYREG0_CR_CR_CAP_ADDR);

	/* Write Data */
	writel(EXYNOS5_DRD_PHYREG0_CR_DATA_IN(data),
			sphy->regs + EXYNOS5_DRD_PHYREG0);
	samsung_usb3phy_crport_handshake(sphy,
			EXYNOS5_DRD_PHYREG0_CR_DATA_IN(data),
			EXYNOS5_DRD_PHYREG0_CR_CR_CAP_DATA);
	samsung_usb3phy_crport_handshake(sphy,
			EXYNOS5_DRD_PHYREG0_CR_DATA_IN(data),
			EXYNOS5_DRD_PHYREG0_CR_WRITE);
}

// static void samsung_usb3phy_tune(struct usb_phy *phy)
// {
// 	struct samsung_usbphy *sphy;
// 	u32 refclk;
// 	u32 temp;

// 	sphy = phy_to_sphy(phy);
// 	refclk = sphy->ref_clk_freq;

// 	if (sphy->drv_data->cpu_type == TYPE_EXYNOS5420) {
// 		u32 phyparam0 = readl(sphy->regs + EXYNOS5_DRD_PHYPARAM0);

// 		if (phy->otg->state >= OTG_STATE_A_IDLE) {	/* for host mode */
// 			/* txpreempamptune[16:15] 2'b00 : default value */
// 			phyparam0 &= ~PHYPARAM0_TXPREEMPAMPTUNE_MASK;
// 			phyparam0 |= PHYPARAM0_TXPREEMPAMPTUNE(0x0);
// 			/* txvreftune[25:22] 4'b0011 : defalut value */
// 			phyparam0 &= ~PHYPARAM0_TXVREFTUNE_MASK;
// 			phyparam0 |= PHYPARAM0_TXVREFTUNE(0x3);
// 		} else {				/* for device mode */
// 			/* txpreempamptune[16:15] 2'b01 :1X  */
// 			phyparam0 &= ~PHYPARAM0_TXPREEMPAMPTUNE_MASK;
// 			phyparam0 |= PHYPARAM0_TXPREEMPAMPTUNE(0x1);
// 			/* txvreftune[25:22] 4'b1011: +10% */
// 			phyparam0 &= ~PHYPARAM0_TXVREFTUNE_MASK;
// 			phyparam0 |= PHYPARAM0_TXVREFTUNE(0xb);
// 		}
// 		writel(phyparam0, sphy->regs + EXYNOS5_DRD_PHYPARAM0);
// 		dev_info(sphy->dev, "set phyparam0 = 0x%x\n", phyparam0);
// 	} else if (sphy->drv_data->cpu_type == TYPE_EXYNOS5430) {
// 		u32 phyparam0 = readl(sphy->regs + EXYNOS5_DRD_PHYPARAM0);

// 		if (phy->otg->state >= OTG_STATE_A_IDLE) {	/* for host mode */
// 			/* compdistune[2:0] 3'b111 : +4.5% */
// 			phyparam0 &= ~PHYPARAM0_COMPDISTUNE_MASK;
// 			phyparam0 |= PHYPARAM0_COMPDISTUNE(0x7);
// 			/* sqrxtune[8:6] 3'b011 : default value */
// 			phyparam0 &= ~PHYPARAM0_SQRXTUNE_MASK;
// 			phyparam0 |= PHYPARAM0_SQRXTUNE(0x3);
// 			/* txpreempamptune[16:15] 2'b00 : default value */
// 			phyparam0 &= ~PHYPARAM0_TXPREEMPAMPTUNE_MASK;
// 			phyparam0 |= PHYPARAM0_TXPREEMPAMPTUNE(0x0);
// 			/* txvreftune[25:22] 4'b0001 : -2.5% */
// 			phyparam0 &= ~PHYPARAM0_TXVREFTUNE_MASK;
// 			phyparam0 |= PHYPARAM0_TXVREFTUNE(0x1);
// 		} else {				/* for device mode */
// 			/* compdistune[2:0] 3'b100 : default value */
// 			phyparam0 &= ~PHYPARAM0_COMPDISTUNE_MASK;
// 			phyparam0 |= PHYPARAM0_COMPDISTUNE(0x4);
// 			/* sqrxtune[8:6] 3'b111 : -20% */
// 			phyparam0 &= ~PHYPARAM0_SQRXTUNE_MASK;
// 			phyparam0 |= PHYPARAM0_SQRXTUNE(0x7);
// 			/* txpreempamptune[16:15] 2'b11 : 3X  */
// 			phyparam0 &= ~PHYPARAM0_TXPREEMPAMPTUNE_MASK;
// 			phyparam0 |= PHYPARAM0_TXPREEMPAMPTUNE(0x3);
// 			/* txvreftune[25:22] 4'b1111 : i5% */
// 			phyparam0 &= ~PHYPARAM0_TXVREFTUNE_MASK;
// 			phyparam0 |= PHYPARAM0_TXVREFTUNE(0xf);
// 		}
// 		writel(phyparam0, sphy->regs + EXYNOS5_DRD_PHYPARAM0);
// 		dev_info(sphy->dev, "set phyparam0 = 0x%x\n", phyparam0);
// 	} 

// 	if (!sphy->drv_data->need_crport_tuning)
// 		return;

// 	if (sphy->drv_data->cpu_type == TYPE_EXYNOS5430) {
// 		/* HS Impedance Setting */
// 		temp = samsung_usb3phy_crport_read(phy,
// 			EXYNOS5_DRD_PHYHS_TX_PARA_RES_CIRCUIT);
// 		temp |= TX_PARA_RES_CIRCUIT_EN_OVRD;
// 		temp &= ~TX_PARA_RES_CIRCUIT_MASK;
// 		/* choose one of them : 0x0, 0x4, 0x8, 0xb, 0xf */
// 		temp |= TX_PARA_RES_CIRCUIT(0x8);
// 		samsung_usb3phy_crport_ctrl(sphy,
// 			EXYNOS5_DRD_PHYHS_TX_PARA_RES_CIRCUIT, temp);

// 		/* Disable Incremantal Tune */
// 		temp = samsung_usb3phy_crport_read(phy,
// 			EXYNOS5_DRD_PHYHS_RESIST_REQ_EN_OVRD);
// 		temp |= DIS_INC_TUNE(0x1);
// 		samsung_usb3phy_crport_ctrl(sphy,
// 			EXYNOS5_DRD_PHYHS_RESIST_REQ_EN_OVRD, temp);
// 	} else {
// 		temp = LOSLEVEL_OVRD_IN_LOS_BIAS_5420 |
// 			LOSLEVEL_OVRD_IN_EN |
// 			LOSLEVEL_OVRD_IN_LOS_LEVEL_DEFAULT;
// 		samsung_usb3phy_crport_ctrl(sphy,
// 			EXYNOS5_DRD_PHYSS_LOSLEVEL_OVRD_IN, temp);

// 		temp = TX_VBOOSTLEVEL_OVRD_IN_VBOOST_5420;
// 		samsung_usb3phy_crport_ctrl(sphy,
// 			EXYNOS5_DRD_PHYSS_TX_VBOOSTLEVEL_OVRD_IN, temp);

// 		switch (refclk) {
// 		case FSEL_CLKSEL_50M:
// 			temp = RXDET_MEAS_TIME_50M;
// 			break;
// 		case FSEL_CLKSEL_20M:
// 		case FSEL_CLKSEL_19200K:
// 			temp = RXDET_MEAS_TIME_20M;
// 			break;
// 		case FSEL_CLKSEL_24M:
// 		default:
// 			temp = RXDET_MEAS_TIME_24M;
// 			break;
// 		}
// 		samsung_usb3phy_crport_ctrl(sphy,
// 			EXYNOS5_DRD_PHYSS_RXDET_MEAS_TIME, temp);
// 	}
// }

#ifdef CONFIG_USB_ANDROID_SAMSUNG_USBTUNE
u32 usbtune;

void samsung_usb3phy_tune_read(struct usb_phy *phy)
{
	struct samsung_usbphy *sphy;

	sphy = phy_to_sphy(phy);

	usbtune = readl(sphy->regs + EXYNOS5_DRD_PHYPARAM0);

	printk("%s param0=0x%x(SQRXTUNE=0x%x, TXVREFTUNE=0x%x) \n",
				__func__, usbtune, ((usbtune>>6) & 0x7), ((usbtune>>22) & 0xf));	

}
EXPORT_SYMBOL_GPL(samsung_usb3phy_tune_read);

void samsung_usb3phy_tune_write(struct usb_phy *phy)
{
	struct samsung_usbphy *sphy;

	sphy = phy_to_sphy(phy);

	writel(usbtune, sphy->regs + EXYNOS5_DRD_PHYPARAM0);

	usbtune = readl(sphy->regs + EXYNOS5_DRD_PHYPARAM0);

	printk("%s param0=0x%x(SQRXTUNE=0x%x, TXVREFTUNE=0x%x)\n",
				__func__, usbtune, ((usbtune>>6) & 0x7), ((usbtune>>22) & 0xf));

}
EXPORT_SYMBOL_GPL(samsung_usb3phy_tune_write);

#endif

static int samsung_usb3phy_init(struct usb_phy *phy)
{
	struct samsung_usbphy *sphy;
	unsigned long flags;
	int ret = 0;

	sphy = phy_to_sphy(phy);

	dev_vdbg(sphy->dev, "%s\n", __func__);

	/* Enable the phy clock */
	ret = clk_enable(sphy->clk);
	if (ret) {
		dev_err(sphy->dev, "%s: clk_enable failed\n", __func__);
		return ret;
	}

	spin_lock_irqsave(&sphy->lock, flags);

	sphy->usage_count++;

	if (sphy->usage_count - 1) {
		dev_vdbg(sphy->dev, "PHY is already initialized\n");
		goto exit;
	}

	/* setting default phy-type for USB 3.0 */
	samsung_usbphy_set_type(&sphy->phy, USB_PHY_TYPE_DEVICE);

	/* Disable phy isolation */
	samsung_usbphy_set_isolation(sphy, false);

	/* Initialize usb phy registers */
	samsung_exynos5_usb3phy_enable(sphy);

exit:
	spin_unlock_irqrestore(&sphy->lock, flags);

	/* Disable the phy clock */
	clk_disable(sphy->clk);

	dev_dbg(sphy->dev, "end of %s\n", __func__);

	return ret;
}

static void __samsung_usb3phy_shutdown(struct samsung_usbphy *sphy)
{
	/* setting default phy-type for USB 3.0 */
	samsung_usbphy_set_type(&sphy->phy, USB_PHY_TYPE_DEVICE);

	/* De-initialize usb phy registers */
	samsung_exynos5_usb3phy_disable(sphy);

	/* Enable phy isolation */
	samsung_usbphy_set_isolation(sphy, true);
}

/*
 * Shutdown phy if it's not in use
 */
static void samsung_usb3phy_idle(struct samsung_usbphy *sphy)
{
	unsigned long flags;
	int ret;

	dev_vdbg(sphy->dev, "%s\n", __func__);

	ret = clk_enable(sphy->clk);
	if (ret < 0) {
		dev_err(sphy->dev, "%s: clk_enable failed\n", __func__);
		return;
	}

	spin_lock_irqsave(&sphy->lock, flags);

	if (!sphy->usage_count)
		__samsung_usb3phy_shutdown(sphy);
	else
		dev_vdbg(sphy->dev, "%s: PHY is currently in use\n", __func__);

	spin_unlock_irqrestore(&sphy->lock, flags);

	clk_disable(sphy->clk);
}

/*
 * The function passed to the usb driver for phy shutdown
 */
static void samsung_usb3phy_shutdown(struct usb_phy *phy)
{
	struct samsung_usbphy *sphy;
	unsigned long flags;

	sphy = phy_to_sphy(phy);

	dev_vdbg(sphy->dev, "%s\n", __func__);

	if (clk_enable(sphy->clk)) {
		dev_err(sphy->dev, "%s: clk_enable failed\n", __func__);
		return;
	}

	spin_lock_irqsave(&sphy->lock, flags);

	if (!sphy->usage_count) {
		dev_vdbg(sphy->dev, "PHY is already shutdown\n");
		goto exit;
	}

	sphy->usage_count--;

	if (sphy->usage_count) {
		dev_vdbg(sphy->dev, "PHY is still in use\n");
		goto exit;
	}

	__samsung_usb3phy_shutdown(sphy);
exit:
	spin_unlock_irqrestore(&sphy->lock, flags);

	dev_dbg(sphy->dev, "end of %s\n", __func__);

	clk_disable(sphy->clk);
}

static bool samsung_usb3phy_is_active(struct usb_phy *phy)
{
	struct samsung_usbphy *sphy = phy_to_sphy(phy);

	return !!sphy->usage_count;
}

static int samsung_usb3phy_read(struct usb_phy *phy, u32 reg)
{
	struct samsung_usbphy *sphy = phy_to_sphy(phy);
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&sphy->lock, flags);
	val = readl(sphy->regs + reg);
	spin_unlock_irqrestore(&sphy->lock, flags);

	return val;
}

static int samsung_usb3phy_write(struct usb_phy *phy, u32 val, u32 reg)
{
	struct samsung_usbphy *sphy = phy_to_sphy(phy);
	unsigned long flags;

	spin_lock_irqsave(&sphy->lock, flags);
	writel(val, sphy->regs + reg);
	spin_unlock_irqrestore(&sphy->lock, flags);

	return 0;
}

static struct usb_phy_io_ops samsung_usb3phy_io_ops = {
	.read		= samsung_usb3phy_read,
	.write		= samsung_usb3phy_write,
};

static int
samsung_usb3phy_lpa_event(struct notifier_block *nb,
			  unsigned long event,
			  void *data)
{
	struct samsung_usbphy *sphy = container_of(nb,
					struct samsung_usbphy, lpa_nb);
	int ret = NOTIFY_OK;

	switch (event) {
	case USB_LPA_RESUME:
		/*
		 * There is issue, when USB3.0 PHY is in active state
		 * after LPA resume even if it was shutdown before entering
		 * LPA. This leads to increased power consumption if no
		 * USB drivers use the PHY. Here we shutdown the PHY
		 * (if it is not already in use), so it is in defined state
		 * (OFF) after LPA resume.
		 */
		samsung_usb3phy_idle(sphy);

		break;
	default:
		ret = NOTIFY_DONE;
	}

	return ret;
}

static int samsung_usb3phy_probe(struct platform_device *pdev)
{
	struct samsung_usbphy *sphy;
	struct samsung_usbphy_data *pdata = pdev->dev.platform_data;
	struct device *dev = &pdev->dev;
	struct resource *phy_mem;
	void __iomem	*phy_base;
	struct clk *clk;
	int ret;

	phy_mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	phy_base = devm_ioremap_resource(dev, phy_mem);
	if (IS_ERR(phy_base))
		return PTR_ERR(phy_base);

	sphy = devm_kzalloc(dev, sizeof(*sphy), GFP_KERNEL);
	if (!sphy)
		return -ENOMEM;

	clk = devm_clk_get(dev, "usbdrd30");
	if (IS_ERR(clk)) {
		dev_err(dev, "Failed to get device clock\n");
		return PTR_ERR(clk);
	}

	sphy->dev = dev;

	if (dev->of_node) {
		ret = samsung_usbphy_parse_dt(sphy);
		if (ret < 0)
			return ret;
	} else {
		if (!pdata) {
			dev_err(dev, "no platform data specified\n");
			return -EINVAL;
		}
	}

	sphy->plat		= pdata;
	sphy->regs		= phy_base;
	sphy->clk		= clk;
	sphy->phy.dev		= sphy->dev;
	sphy->phy.label		= "samsung-usb3phy";
	sphy->phy.type		= USB_PHY_TYPE_USB3;
	sphy->phy.init		= samsung_usb3phy_init;
	sphy->phy.shutdown	= samsung_usb3phy_shutdown;
	// sphy->phy.is_active	= samsung_usb3phy_is_active;
	// sphy->phy.tune		= samsung_usb3phy_tune;
	sphy->phy.io_ops	= &samsung_usb3phy_io_ops;
	sphy->drv_data		= samsung_usbphy_get_driver_data(pdev);
	sphy->ref_clk_freq	= samsung_usbphy_get_refclk_freq(sphy);

	spin_lock_init(&sphy->lock);

	ret = clk_prepare(sphy->clk);
	if (ret) {
		dev_err(dev, "clk_prepare failed\n");
		return ret;
	}

	sphy->lpa_nb.notifier_call = samsung_usb3phy_lpa_event;
	sphy->lpa_nb.next = NULL;
	sphy->lpa_nb.priority = 0;

	ret = register_samsung_usb_lpa_notifier(&sphy->lpa_nb);
	if (ret)
		dev_err(dev, "Failed to register lpa notifier\n");

	ret = usb_add_phy_dev(&sphy->phy);
	if (ret) {
		dev_err(dev, "Failed to add PHY\n");
		goto err1;
	}

	platform_set_drvdata(pdev, sphy);

	return 0;

err1:
	clk_unprepare(sphy->clk);

	return ret;
}

static int samsung_usb3phy_remove(struct platform_device *pdev)
{
	struct samsung_usbphy *sphy = platform_get_drvdata(pdev);

	usb_remove_phy(&sphy->phy);
	unregister_samsung_usb_lpa_notifier(&sphy->lpa_nb);
	clk_unprepare(sphy->clk);

	if (sphy->pmuregs)
		iounmap(sphy->pmuregs);
	if (sphy->sysreg)
		iounmap(sphy->sysreg);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int samsung_usb3phy_resume(struct device *dev)
{
	struct samsung_usbphy *sphy = dev_get_drvdata(dev);

	/*
	 * There is issue, when USB3.0 PHY is in active state
	 * after system resume even if it was shutdown before entering
	 * system suspend. This leads to increased power consumption
	 * if no USB drivers use the PHY. Here we shutdown the PHY
	 * (if it is not already in use), so it is in defined state
	 * (OFF) after system resume.
	 */
	samsung_usb3phy_idle(sphy);

	return 0;
}

static const struct dev_pm_ops samsung_usb3phy_dev_pm_ops = {
	.resume		= samsung_usb3phy_resume,
};

#define DEV_PM_OPS     (&samsung_usb3phy_dev_pm_ops)
#else
#define DEV_PM_OPS     NULL
#endif /* CONFIG_PM_SLEEP */

static struct samsung_usbphy_drvdata usb3phy_exynos5250 = {
	.cpu_type		= TYPE_EXYNOS5250,
	.devphy_en_mask		= EXYNOS_USBPHY_ENABLE,
	.need_crport_tuning	= false,
};

static struct samsung_usbphy_drvdata usb3phy_exynos5420 = {
	.cpu_type		= TYPE_EXYNOS5420,
	.devphy_en_mask		= EXYNOS_USBPHY_ENABLE,
	.need_crport_tuning	= true,
};

static struct samsung_usbphy_drvdata usb3phy_exynos5430 = {
	.cpu_type		= TYPE_EXYNOS5430,
	.devphy_en_mask		= EXYNOS_USBPHY_ENABLE,
	.need_crport_tuning	= true,
};

static struct samsung_usbphy_drvdata usb3phy_exynos5 = {
	.cpu_type		= TYPE_EXYNOS5,
	.devphy_en_mask		= EXYNOS_USBPHY_ENABLE,
	.need_crport_tuning	= false,
};

#ifdef CONFIG_OF
static const struct of_device_id samsung_usbphy_dt_match[] = {
	{
		.compatible = "samsung,exynos5250-usb3phy",
		.data = &usb3phy_exynos5250,
	}, {
		.compatible = "samsung,exynos5420-usb3phy",
		.data = &usb3phy_exynos5420,
	}, {
		.compatible = "samsung,exynos5430-usb3phy",
		.data = &usb3phy_exynos5430,
	}, {
		.compatible = "samsung,exynos5-usb3phy",
		.data = &usb3phy_exynos5,
	},
	{},
};
MODULE_DEVICE_TABLE(of, samsung_usbphy_dt_match);
#endif

static struct platform_device_id samsung_usbphy_driver_ids[] = {
	{
		.name		= "exynos5250-usb3phy",
		.driver_data	= (unsigned long)&usb3phy_exynos5250,
	}, {
		.name		= "exynos5420-usb3phy",
		.driver_data	= (unsigned long)&usb3phy_exynos5420,
	}, {
		.name		= "exynos5430-usb3phy",
		.driver_data	= (unsigned long)&usb3phy_exynos5430,
	}, {
		.name		= "exynos5-usb3phy",
		.driver_data	= (unsigned long)&usb3phy_exynos5,
	},
	{},
};

MODULE_DEVICE_TABLE(platform, samsung_usbphy_driver_ids);

static struct platform_driver samsung_usb3phy_driver = {
	.probe		= samsung_usb3phy_probe,
	.remove		= samsung_usb3phy_remove,
	.id_table	= samsung_usbphy_driver_ids,
	.driver		= {
		.name	= "samsung-usb3phy",
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(samsung_usbphy_dt_match),
		.pm	= DEV_PM_OPS,
	},
};

module_platform_driver(samsung_usb3phy_driver);

MODULE_DESCRIPTION("Samsung USB 3.0 phy controller");
MODULE_AUTHOR("Vivek Gautam <gautam.vivek@samsung.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:samsung-usb3phy");
