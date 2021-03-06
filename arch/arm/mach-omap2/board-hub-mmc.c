/*
 * linux/arch/arm/mach-omap2/board-hub-mmc.c
 *
 * Copyright (C) 2007-2008 Texas Instruments
 * Copyright (C) 2008 Nokia Corporation
 * Author: Texas Instruments
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/gpio.h>		// from GB

#include <linux/regulator/consumer.h>		// from GB
#include <mach/hardware.h>
#include <plat/mmc.h>
#include <plat/omap-pm.h>
#include <plat/mux.h>
#include <plat/omap_device.h>

#ifdef CONFIG_MMC_EMBEDDED_SDIO
#include <linux/mmc/sdio_ids.h>
#include <linux/mmc/sdio_func.h>
#endif


#include "mux.h"
#include "hsmmc.h"
#include "control.h"

#if defined(CONFIG_REGULATOR) && defined(CONFIG_MMC_OMAP_HS) || defined(CONFIG_MMC_OMAP_HS_MODULE)

#if defined(CONFIG_REGULATOR_LP8720)
#include <linux/regulator/lp8720.h>
#endif 	// CONFIG_REGULATOR_LP8720
struct omap_hsmmc_host {
	struct	device		*dev;
	struct	mmc_host	*mmc;
	struct	mmc_request	*mrq;
	struct	mmc_command	*cmd;
	struct	mmc_data	*data;
	struct	clk		*fclk;
	struct	clk		*iclk;
	struct	clk		*dbclk;
	/*
	 * vcc == configured supply
	 * vcc_aux == optional
	 *   -	MMC1, supply for DAT4..DAT7
	 *   -	MMC2/MMC2, external level shifter voltage supply, for
	 *	chip (SDIO, eMMC, etc) or transceiver (MMC2 only)
	 */
	struct	regulator	*vcc;
	struct	regulator	*vcc_aux;
	struct	work_struct	mmc_carddetect_work;
	void	__iomem		*base;
	resource_size_t		mapbase;
	spinlock_t		irq_lock; /* Prevent races with irq handler */
	unsigned int		id;
	unsigned int		dma_len;
	unsigned int		dma_sg_idx;
	unsigned int		master_clock;
	unsigned char		bus_mode;
	unsigned char		power_mode;
	u32			*buffer;
	u32			bytesleft;
	int			suspended;
	int			irq;
	int			dma_type, dma_ch;
	struct adma_desc_table	*adma_table;
	dma_addr_t		phy_adma_table;
	int			dma_line_tx, dma_line_rx;
	int			slot_id;
	int			got_dbclk;
	int			response_busy;
	int			dpm_state;
	int			vdd;
	int			protect_card;
	int			reqs_blocked;
	int			use_reg;
	int			req_in_progress;
	unsigned int		flags;

	struct	omap_mmc_platform_data	*pdata;
};
static u16 control_pbias_offset;
static u16 control_devconf1_offset;
static u16 control_mmc1;

#define HSMMC_NAME_LEN	9

// from GB, MMC0 and MMC1 was swapping. [START]
// #define TI_FS_MMC // 0 = working for B, 1 = working for TI
// from GB, MMC0 and MMC1 was swapping. [END]

#if defined(CONFIG_REGULATOR_LP8720)
extern void subpm_set_output(subpm_output_enum outnum, int onoff);
extern void subpm_output_enable(void);
#endif


static void omap_hsmmc1_before_set_reg(struct device *dev, int slot,
				  int power_on, int vdd)
{
	u32 reg, prog_io;
	struct omap_mmc_platform_data *mmc = dev->platform_data;

	if (mmc->slots[0].remux)
		mmc->slots[0].remux(dev, slot, power_on);

	/*
	 * Assume we power both OMAP VMMC1 (for CMD, CLK, DAT0..3) and the
	 * card with Vcc regulator (from twl4030 or whatever).  OMAP has both
	 * 1.8V and 3.0V modes, controlled by the PBIAS register.
	 *
	 * In 8-bit modes, OMAP VMMC1A (for DAT4..7) needs a supply, which
	 * is most naturally TWL VSIM; those pins also use PBIAS.
	 *
	 * FIXME handle VMMC1A as needed ...
	 */
	if (power_on) {
		if (cpu_is_omap2430()) {
			reg = omap_ctrl_readl(OMAP243X_CONTROL_DEVCONF1);
			if ((1 << vdd) >= MMC_VDD_30_31)
				reg |= OMAP243X_MMC1_ACTIVE_OVERWRITE;
			else
				reg &= ~OMAP243X_MMC1_ACTIVE_OVERWRITE;
			omap_ctrl_writel(reg, OMAP243X_CONTROL_DEVCONF1);
		}

		if (mmc->slots[0].internal_clock) {
			reg = omap_ctrl_readl(OMAP2_CONTROL_DEVCONF0);
			reg |= OMAP2_MMCSDIO1ADPCLKISEL;
			omap_ctrl_writel(reg, OMAP2_CONTROL_DEVCONF0);
		}

		reg = omap_ctrl_readl(control_pbias_offset);
		if (cpu_is_omap3630()) {
			/* Set MMC I/O to 52Mhz */
			prog_io = omap_ctrl_readl(OMAP343X_CONTROL_PROG_IO1);
			prog_io |= OMAP3630_PRG_SDMMC1_SPEEDCTRL;
			omap_ctrl_writel(prog_io, OMAP343X_CONTROL_PROG_IO1);
		} else {
			reg |= OMAP2_PBIASSPEEDCTRL0;
		}
		reg &= ~OMAP2_PBIASLITEPWRDNZ0;
		omap_ctrl_writel(reg, control_pbias_offset);

	} else {
		reg = omap_ctrl_readl(control_pbias_offset);
		reg &= ~OMAP2_PBIASLITEPWRDNZ0;
		omap_ctrl_writel(reg, control_pbias_offset);
	}
}

static void omap_hsmmc1_after_set_reg(struct device *dev, int slot,
				 int power_on, int vdd)
{
	u32 reg;

	/* 100ms delay required for PBIAS configuration */
	msleep(10);

	if (power_on) {
		reg = omap_ctrl_readl(control_pbias_offset);
		reg |= (OMAP2_PBIASLITEPWRDNZ0 | OMAP2_PBIASSPEEDCTRL0);
		if ((1 << vdd) <= MMC_VDD_165_195)
			reg &= ~OMAP2_PBIASLITEVMODE0;
		else
			reg |= OMAP2_PBIASLITEVMODE0;
		omap_ctrl_writel(reg, control_pbias_offset);
	} else {
	
		reg = omap_ctrl_readl(control_pbias_offset);
		reg |= (OMAP2_PBIASSPEEDCTRL0 | OMAP2_PBIASLITEPWRDNZ0 |
			OMAP2_PBIASLITEVMODE0);
		omap_ctrl_writel(reg, control_pbias_offset);
	}
}

static int omap_hsmmc_1_set_power(struct device *dev, int slot, int power_on,
				  int vdd)
{
    int ret = 0;
	struct omap_hsmmc_host *host = platform_get_drvdata(to_platform_device(dev));

	omap_hsmmc1_before_set_reg(dev, slot, power_on, vdd);

	//if((host->suspended) != 1) // not suspend case! do control regulator ON,OFF regularly
    {

	    if (power_on) 
        {
		    subpm_set_output(LDO1, 1);
		    subpm_output_enable();
	    }
	    else 
        {
		    subpm_set_output(LDO1, 0);
		    subpm_output_enable();
    	}
	}
#if 0
	else	// suspend case! ON is allowed , OFF is not allowed
	{
	    if (power_on) 
        {
            //if (g_check_on == 1 ) return 0;
		    subpm_set_output(LDO1, 1);
		    subpm_output_enable();
            //g_check_on = 1;
	    }

    }
#endif

	omap_hsmmc1_after_set_reg(dev, slot, power_on, vdd);

	return ret;
}

static int omap_hsmmc_1_set_sleep(struct device *dev, int slot, int sleep,
				  int vdd, int cardsleep)
{
#if 0
	struct omap_hsmmc_host *host =
		platform_get_drvdata(to_platform_device(dev));
	int mode = sleep ? REGULATOR_MODE_STANDBY : REGULATOR_MODE_NORMAL;

	return regulator_set_mode(host->vcc, mode);
#else
	return 0;
#endif
}

static void omap4_hsmmc1_before_set_reg(struct device *dev, int slot,
				  int power_on, int vdd)
{
	u32 reg;

	/*
	 * Assume we power both OMAP VMMC1 (for CMD, CLK, DAT0..3) and the
	 * card with Vcc regulator (from twl4030 or whatever).  OMAP has both
	 * 1.8V and 3.0V modes, controlled by the PBIAS register.
	 *
	 * In 8-bit modes, OMAP VMMC1A (for DAT4..7) needs a supply, which
	 * is most naturally TWL VSIM; those pins also use PBIAS.
	 *
	 * FIXME handle VMMC1A as needed ...
	 */
	reg = omap4_ctrl_pad_readl(control_pbias_offset);
	reg &= ~(OMAP4_MMC1_PBIASLITE_PWRDNZ_MASK |
		OMAP4_MMC1_PWRDNZ_MASK);
	omap4_ctrl_pad_writel(reg, control_pbias_offset);
}

static void omap4_hsmmc1_after_set_reg(struct device *dev, int slot,
				 int power_on, int vdd)
{
	u32 reg;
	unsigned long timeout;

	if (power_on) {
		reg = omap4_ctrl_pad_readl(control_pbias_offset);
		reg |= OMAP4_MMC1_PBIASLITE_PWRDNZ_MASK;
		if ((1 << vdd) <= MMC_VDD_165_195)
			reg &= ~OMAP4_MMC1_PBIASLITE_VMODE_MASK;
		else
			reg |= OMAP4_MMC1_PBIASLITE_VMODE_MASK;
		reg |= (OMAP4_MMC1_PBIASLITE_PWRDNZ_MASK |
			OMAP4_MMC1_PWRDNZ_MASK);
		omap4_ctrl_pad_writel(reg, control_pbias_offset);

		timeout = jiffies + msecs_to_jiffies(5);
		do {
			reg = omap4_ctrl_pad_readl(control_pbias_offset);
			if (!(reg & OMAP4_MMC1_PBIASLITE_VMODE_ERROR_MASK))
				break;
			usleep_range(100, 200);
		} while (!time_after(jiffies, timeout));

		if (reg & OMAP4_MMC1_PBIASLITE_VMODE_ERROR_MASK) {
			pr_err("Pbias Voltage is not same as LDO\n");
			/* Caution : On VMODE_ERROR Power Down MMC IO */
			reg &= ~(OMAP4_MMC1_PWRDNZ_MASK);
			omap4_ctrl_pad_writel(reg, control_pbias_offset);
		}
	}
}

static void hsmmc23_before_set_reg(struct device *dev, int slot,
				   int power_on, int vdd)
{
	struct omap_mmc_platform_data *mmc = dev->platform_data;

	if (mmc->slots[0].remux)
		mmc->slots[0].remux(dev, slot, power_on);

	if (power_on) {
		/* Only MMC2 supports a CLKIN */
		if (mmc->slots[0].internal_clock) {
			u32 reg;

			reg = omap_ctrl_readl(control_devconf1_offset);
			reg |= OMAP2_MMCSDIO2ADPCLKISEL;
			omap_ctrl_writel(reg, control_devconf1_offset);
		}
	}
}

static int nop_mmc_set_power(struct device *dev, int slot, int power_on,
							int vdd)
{
	return 0;
}

#ifdef CONFIG_MMC_EMBEDDED_SDIO
static struct sdio_embedded_func wifi_func_array[] = {
	{
		.f_class        = SDIO_CLASS_BT_A,
		.f_maxblksize   = 512,
	},
	{
		.f_class        = SDIO_CLASS_WLAN,
		.f_maxblksize   = 512,
	},
};

static struct embedded_sdio_data omap_wifi_emb_data = {
	.cis    = {
		.vendor         = SDIO_VENDOR_ID_BRCM,//SDIO_VENDOR_ID_TI,
		.device         = SDIO_DEVICE_ID_BCM4329,//SDIO_DEVICE_ID_TI_WL12xx,
		.blksize        = 512,
#ifdef CONFIG_ARCH_OMAP3
		.max_dtr        = 50000000,//24000000,
#else
		.max_dtr        = 48000000,
#endif
	},
	.cccr   = {
		.multi_block	= 1,
		.low_speed	= 0,
		.wide_bus	= 1,
		.high_power	= 0,
#ifdef CONFIG_ARCH_OMAP3
		.high_speed	= 1,//0,
#else
		.high_speed	= 1,
#endif
		.disable_cd	= 1,
	},
	.funcs  = wifi_func_array,
	.num_funcs = MMC_QUIRK_VDD_165_195 | MMC_QUIRK_LENIENT_FUNC0,
};
#endif

static inline void omap_hsmmc_mux(struct omap_mmc_platform_data *mmc_controller,
			int controller_nr)
{
	if ((mmc_controller->slots[0].switch_pin > 0) && \
		(mmc_controller->slots[0].switch_pin < OMAP_MAX_GPIO_LINES))
		omap_mux_init_gpio(mmc_controller->slots[0].switch_pin,
					OMAP_PIN_INPUT_PULLUP);
	if ((mmc_controller->slots[0].gpio_wp > 0) && \
		(mmc_controller->slots[0].gpio_wp < OMAP_MAX_GPIO_LINES))
		omap_mux_init_gpio(mmc_controller->slots[0].gpio_wp,
					OMAP_PIN_INPUT_PULLUP);
	if (cpu_is_omap34xx()) {
		if (controller_nr == 0) {
			omap_mux_init_signal("sdmmc1_clk",
				OMAP_PIN_INPUT_PULLUP);
			omap_mux_init_signal("sdmmc1_cmd",
				OMAP_PIN_INPUT_PULLUP);
			omap_mux_init_signal("sdmmc1_dat0",
				OMAP_PIN_INPUT_PULLUP);
			if (mmc_controller->slots[0].caps &
				(MMC_CAP_4_BIT_DATA | MMC_CAP_8_BIT_DATA)) {
				omap_mux_init_signal("sdmmc1_dat1",
					OMAP_PIN_INPUT_PULLUP);
				omap_mux_init_signal("sdmmc1_dat2",
					OMAP_PIN_INPUT_PULLUP);
				omap_mux_init_signal("sdmmc1_dat3",
					OMAP_PIN_INPUT_PULLUP);
			}
			if (mmc_controller->slots[0].caps &
						MMC_CAP_8_BIT_DATA) {
				omap_mux_init_signal("sdmmc1_dat4",
					OMAP_PIN_INPUT_PULLUP);
				omap_mux_init_signal("sdmmc1_dat5",
					OMAP_PIN_INPUT_PULLUP);
				omap_mux_init_signal("sdmmc1_dat6",
					OMAP_PIN_INPUT_PULLUP);
				omap_mux_init_signal("sdmmc1_dat7",
					OMAP_PIN_INPUT_PULLUP);
			}
		}
		if (controller_nr == 1) {
			/* MMC2 */
			omap_mux_init_signal("sdmmc2_clk",
				OMAP_PIN_INPUT_PULLUP);
			omap_mux_init_signal("sdmmc2_cmd",
				OMAP_PIN_INPUT_PULLUP);
			omap_mux_init_signal("sdmmc2_dat0",
				OMAP_PIN_INPUT_PULLUP);

			/*
			 * For 8 wire configurations, Lines DAT4, 5, 6 and 7
			 * need to be muxed in the board-*.c files
			 */
			if (mmc_controller->slots[0].caps &
				(MMC_CAP_4_BIT_DATA | MMC_CAP_8_BIT_DATA)) {
				omap_mux_init_signal("sdmmc2_dat1",
					OMAP_PIN_INPUT_PULLUP);
				omap_mux_init_signal("sdmmc2_dat2",
					OMAP_PIN_INPUT_PULLUP);
				omap_mux_init_signal("sdmmc2_dat3",
					OMAP_PIN_INPUT_PULLUP);
			}
			if (mmc_controller->slots[0].caps &
							MMC_CAP_8_BIT_DATA) {
				omap_mux_init_signal("sdmmc2_dat4.sdmmc2_dat4",
					OMAP_PIN_INPUT_PULLUP);
				omap_mux_init_signal("sdmmc2_dat5.sdmmc2_dat5",
					OMAP_PIN_INPUT_PULLUP);
				omap_mux_init_signal("sdmmc2_dat6.sdmmc2_dat6",
					OMAP_PIN_INPUT_PULLUP);
				omap_mux_init_signal("sdmmc2_dat7.sdmmc2_dat7",
					OMAP_PIN_INPUT_PULLUP);
			}
		}

		/*
		 * For MMC3 the pins need to be muxed in the board-*.c files
		 */
	}
}

static int __init omap_hsmmc_pdata_init(struct omap2_hsmmc_info *c,
					struct omap_mmc_platform_data *mmc)
{
	char *hc_name;

	hc_name = kzalloc(sizeof(char) * (HSMMC_NAME_LEN + 1), GFP_KERNEL);
	if (!hc_name) {
		pr_err("Cannot allocate memory for controller slot name\n");
		kfree(hc_name);
		return -ENOMEM;
	}

	if (c->name)
		strncpy(hc_name, c->name, HSMMC_NAME_LEN);
	else
		snprintf(hc_name, (HSMMC_NAME_LEN + 1), "mmc%islot%i",
								c->mmc, 1);

#ifdef CONFIG_MMC_EMBEDDED_SDIO
	if (c->mmc == CONFIG_TIWLAN_MMC_CONTROLLER) {
		mmc->slots[0].mmc_data.embedded_sdio = &omap_wifi_emb_data;
		mmc->slots[0].mmc_data.register_status_notify =&omap_wifi_status_register;
		mmc->slots[0].mmc_data.status = &omap_wifi_status;
	}
#endif

	mmc->slots[0].name = hc_name;
	mmc->nr_slots = 1;
	mmc->slots[0].caps = c->caps;
	mmc->slots[0].internal_clock = !c->ext_clock;
	mmc->dma_mask = 0xffffffff;
	if (cpu_is_omap44xx())
		mmc->reg_offset = OMAP4_MMC_REG_OFFSET;
	else
		mmc->reg_offset = 0;

	mmc->slots[0].switch_pin = c->gpio_cd;
	mmc->slots[0].gpio_wp = c->gpio_wp;

	mmc->slots[0].remux = c->remux;
	mmc->slots[0].init_card = c->init_card;

	if (c->cover_only)
		mmc->slots[0].cover = 1;

	if (c->nonremovable)
		mmc->slots[0].nonremovable = 1;

	if (c->power_saving)
		mmc->slots[0].power_saving = 1;

	if (c->no_off)
		mmc->slots[0].no_off = 1;

	if (c->no_off_init)
		mmc->slots[0].no_regulator_off_init = c->no_off_init;

	if (c->vcc_aux_disable_is_sleep)
		mmc->slots[0].vcc_aux_disable_is_sleep = 1;

	if (cpu_is_omap44xx()) {
		if (omap_rev() > OMAP4430_REV_ES1_0)
			mmc->slots[0].features |= HSMMC_HAS_UPDATED_RESET;
		if (c->mmc >= 3 && c->mmc <= 5)
			mmc->slots[0].features |= HSMMC_HAS_48MHZ_MASTER_CLK;
	}

	if (c->mmc_data) {
		memcpy(&mmc->slots[0].mmc_data, c->mmc_data,
				sizeof(struct mmc_platform_data));
		mmc->slots[0].card_detect =
				(mmc_card_detect_func)c->mmc_data->status;
	}

	/*
	 * NOTE:  MMC slots should have a Vcc regulator set up.
	 * This may be from a TWL4030-family chip, another
	 * controllable regulator, or a fixed supply.
	 *
	 * temporary HACK: ocr_mask instead of fixed supply
	 */
	mmc->slots[0].ocr_mask = c->ocr_mask;

	if (cpu_is_omap3517() || cpu_is_omap3505())
		mmc->slots[0].set_power = nop_mmc_set_power;
	else
		mmc->slots[0].features |= HSMMC_HAS_PBIAS;

	if (cpu_is_omap44xx() && (omap_rev() > OMAP4430_REV_ES1_0))
		mmc->slots[0].features |= HSMMC_HAS_UPDATED_RESET;

	switch (c->mmc) {
	case 1:
		if (mmc->slots[0].features & HSMMC_HAS_PBIAS) {
			/* on-chip level shifting via PBIAS0/PBIAS1 */
			if (cpu_is_omap44xx()) {
				mmc->slots[0].before_set_reg =
						omap4_hsmmc1_before_set_reg;
				mmc->slots[0].after_set_reg =
						omap4_hsmmc1_after_set_reg;
			} else {
				mmc->slots[0].before_set_reg =
						omap_hsmmc1_before_set_reg;
				mmc->slots[0].after_set_reg =
						omap_hsmmc1_after_set_reg;
			}
		}

		/* OMAP3630 HSMMC1 supports only 4-bit */
		if (cpu_is_omap3630() &&
				(c->caps & MMC_CAP_8_BIT_DATA)) {
			c->caps &= ~MMC_CAP_8_BIT_DATA;
			c->caps |= MMC_CAP_4_BIT_DATA;
			mmc->slots[0].caps = c->caps;
		}
#ifdef CONFIG_MACH_LGE_HUB
		mmc->slots[0].ocr_mask  = MMC_VDD_29_30;
		mmc->slots[0].set_power = omap_hsmmc_1_set_power;
		mmc->slots[0].set_sleep = omap_hsmmc_1_set_sleep;
#endif
		break;
	case 2:
		if (c->ext_clock)
			c->transceiver = 1;
		if (c->transceiver && (c->caps & MMC_CAP_8_BIT_DATA)) {
			c->caps &= ~MMC_CAP_8_BIT_DATA;
			c->caps |= MMC_CAP_4_BIT_DATA;
		}
		/* FALLTHROUGH */
	case 3:
		if (mmc->slots[0].features & HSMMC_HAS_PBIAS) {
			/* off-chip level shifting, or none */
			mmc->slots[0].before_set_reg = hsmmc23_before_set_reg;
			mmc->slots[0].after_set_reg = NULL;
		}

#if defined (CONFIG_MMC_EMBEDDED_SDIO) || (CONFIG_MACH_LGE_HUB)
		//mmc->slots[0].ocr_mask  = MMC_VDD_165_195; // ORG~~~
		mmc->slots[0].ocr_mask  = (c->mmc == 3) ? MMC_VDD_29_30 : MMC_VDD_165_195; // LGE_CHANGE_S, [WiFi][kihoon1.lee@lge.com], 20120413 for Blackg ICS wifi bringup voltage change test
#endif

		break;
	case 4:
	case 5:
		mmc->slots[0].before_set_reg = NULL;
		mmc->slots[0].after_set_reg = NULL;

#if defined (CONFIG_MMC_EMBEDDED_SDIO) || (CONFIG_MACH_LGE_HUB)
			mmc->slots[0].ocr_mask  = MMC_VDD_165_195;
#endif

		break;
	default:
		pr_err("MMC%d configuration not supported!\n", c->mmc);
		kfree(hc_name);
		return -ENODEV;
	}
	return 0;
}

static struct omap_device_pm_latency omap_hsmmc_latency[] = {
	[0] = {
		.deactivate_func = omap_device_idle_hwmods,
		.activate_func	 = omap_device_enable_hwmods,
		.flags		 = OMAP_DEVICE_LATENCY_AUTO_ADJUST,
	},
	/*
	 * XXX There should also be an entry here to power off/on the
	 * MMC regulators/PBIAS cells, etc.
	 */
};

#define MAX_OMAP_MMC_HWMOD_NAME_LEN		16

void __init omap_init_hsmmc(struct omap2_hsmmc_info *hsmmcinfo, int ctrl_nr)
{
	struct omap_hwmod *oh;
	struct omap_device *od;
	struct omap_device_pm_latency *ohl;
	char oh_name[MAX_OMAP_MMC_HWMOD_NAME_LEN];
	struct omap_mmc_platform_data *mmc_data;
	struct omap_mmc_dev_attr *mmc_dev_attr;
	char *name;
	int l;
	int ohl_cnt = 0;

	mmc_data = kzalloc(sizeof(struct omap_mmc_platform_data), GFP_KERNEL);
	if (!mmc_data) {
		pr_err("Cannot allocate memory for mmc device!\n");
		goto done;
	}

	if (omap_hsmmc_pdata_init(hsmmcinfo, mmc_data) < 0) {
		pr_err("%s fails!\n", __func__);
		goto done;
	}
	// ICS temp
	//omap_hsmmc_mux(mmc_data, (ctrl_nr - 1));

	name = "omap_hsmmc";
	ohl = omap_hsmmc_latency;
	ohl_cnt = ARRAY_SIZE(omap_hsmmc_latency);

	l = snprintf(oh_name, MAX_OMAP_MMC_HWMOD_NAME_LEN,
		     "mmc%d", ctrl_nr);
	WARN(l >= MAX_OMAP_MMC_HWMOD_NAME_LEN,
	     "String buffer overflow in MMC%d device setup\n", ctrl_nr);
	oh = omap_hwmod_lookup(oh_name);
	if (!oh) {
		pr_err("Could not look up %s\n", oh_name);
		kfree(mmc_data->slots[0].name);
		goto done;
	}

	if (oh->dev_attr != NULL) {
		mmc_dev_attr = oh->dev_attr;
		mmc_data->controller_flags = mmc_dev_attr->flags;
	}

	od = omap_device_build(name, ctrl_nr - 1, oh, mmc_data,
		sizeof(struct omap_mmc_platform_data), ohl, ohl_cnt, false);
	if (IS_ERR(od)) {
		WARN(1, "Can't build omap_device for %s:%s.\n", name, oh->name);
		kfree(mmc_data->slots[0].name);
		goto done;
	}
	/*
	 * return device handle to board setup code
	 * required to populate for regulator framework structure
	 */
	hsmmcinfo->dev = &od->pdev.dev;

done:
	kfree(mmc_data);
}

void __init omap2_hsmmc_init(struct omap2_hsmmc_info *controllers)
{
	u32 reg;

	if (!cpu_is_omap44xx()) {
		if (cpu_is_omap2430()) {
			control_pbias_offset = OMAP243X_CONTROL_PBIAS_LITE;
			control_devconf1_offset = OMAP243X_CONTROL_DEVCONF1;
		} else {
			control_pbias_offset = OMAP343X_CONTROL_PBIAS_LITE;
			control_devconf1_offset = OMAP343X_CONTROL_DEVCONF1;
		}
	} else {
		control_pbias_offset =
			OMAP4_CTRL_MODULE_PAD_CORE_CONTROL_PBIASLITE;
		control_mmc1 = OMAP4_CTRL_MODULE_PAD_CORE_CONTROL_MMC1;
		reg = omap4_ctrl_pad_readl(control_mmc1);
		reg |= (OMAP4_SDMMC1_PUSTRENGTH_GRP0_MASK |
			OMAP4_SDMMC1_PUSTRENGTH_GRP1_MASK);
		reg &= ~(OMAP4_SDMMC1_PUSTRENGTH_GRP2_MASK |
			OMAP4_SDMMC1_PUSTRENGTH_GRP3_MASK);
		reg |= (OMAP4_USBC1_DR0_SPEEDCTRL_MASK|
			OMAP4_SDMMC1_DR1_SPEEDCTRL_MASK |
			OMAP4_SDMMC1_DR2_SPEEDCTRL_MASK);
		omap4_ctrl_pad_writel(reg, control_mmc1);
	}
// from GB MMC0 and MMC1 was swapping. [START]
#ifdef TI_FS_MMC
	for (; controllers->mmc; controllers++)
		omap_init_hsmmc(controllers, controllers->mmc);
#else
	omap_init_hsmmc(&controllers[1], controllers[1].mmc);
	omap_init_hsmmc(&controllers[0], controllers[0].mmc);
	omap_init_hsmmc(&controllers[2], controllers[2].mmc);
#endif // TI_FS_MMC
// from GB MMC0 and MMC1 was swapping. [END]

}

#endif

//------------------------------------------------------------------------
// THIS APIs currently are not used. GB APIs...
//------------------------------------------------------------------------
#if 0
#if 0
static struct hsmmc_controller {
	char				name[HSMMC_NAME_LEN + 1];
} hsmmc[OMAP44XX_NR_MMC];
#else
static struct twl_mmc_controller {
	struct omap_mmc_platform_data	*mmc;
	/* Vcc == configured supply
	 * Vcc_alt == optional
	 *   -	MMC1, supply for DAT4..DAT7
	 *   -	MMC2/MMC2, external level shifter voltage supply, for
	 *	chip (SDIO, eMMC, etc) or transceiver (MMC2 only)
	 */
	struct regulator		*vcc;
	struct regulator		*vcc_aux;
	char				name[HSMMC_NAME_LEN + 1];
} hsmmc[OMAP34XX_NR_MMC];
#endif

// from GB
static int twl_mmc_get_ro(struct device *dev, int slot)
{
	struct omap_mmc_platform_data *mmc = dev->platform_data;

	/* NOTE: assumes write protect signal is active-high */
	return gpio_get_value_cansleep(mmc->slots[0].gpio_wp);
}
/*
 * MMC Slot Initialization.
 */
static int twl_mmc_late_init(struct device *dev)
{
	struct omap_mmc_platform_data *mmc = dev->platform_data;
	int ret = 0;
	int i;

// prime@sdcmicro.com Block the following code because it's already done by underlying driver in 2.6.35 [START]
#if 0
	/* MMC/SD/SDIO doesn't require a card detect switch */
	if (gpio_is_valid(mmc->slots[0].switch_pin)) {
		ret = gpio_request(mmc->slots[0].switch_pin, "mmc_cd");
		if (ret)
			goto done;
		ret = gpio_direction_input(mmc->slots[0].switch_pin);
		if (ret)
			goto err;
	}
#endif
// prime@sdcmicro.com Block the following code because it's already done by underlying driver in 2.6.35 [END]
	/* require at least main regulator */
	for (i = 0; i < ARRAY_SIZE(hsmmc); i++) {
		if (hsmmc[i].name == mmc->slots[0].name) {
			struct regulator *reg;

			hsmmc[i].mmc = mmc;

			switch (i) {
			case 0:
				mmc->slots[0].ocr_mask = MMC_VDD_29_30;
				break;
			case 1:
				reg = regulator_get(dev, "vmmc2");
				if (IS_ERR(reg)) {
					ret = PTR_ERR(reg);
					hsmmc[i].vcc = NULL;
					goto err;
				}
				hsmmc[i].vcc = reg;
				mmc->slots[0].ocr_mask = MMC_VDD_29_30;
				break;
			case 2:
				mmc->slots[0].ocr_mask = MMC_VDD_29_30;
				break;
			default:
				break;
			}
		}
	}

	return 0;

err:
	gpio_free(mmc->slots[0].switch_pin);
done:
	mmc->slots[0].card_detect_irq = 0;
	mmc->slots[0].card_detect = NULL;

	dev_err(dev, "err %d configuring card detect\n", ret);
	return ret;
}

static void twl_mmc_cleanup(struct device *dev)
{
	struct omap_mmc_platform_data *mmc = dev->platform_data;
	int i;

	gpio_free(mmc->slots[0].switch_pin);
	for(i = 0; i < ARRAY_SIZE(hsmmc); i++) {
		regulator_put(hsmmc[i].vcc);
		regulator_put(hsmmc[i].vcc_aux);
	}
}

#ifdef CONFIG_PM
static int twl_mmc_suspend(struct device *dev, int slot)
{
	struct omap_mmc_platform_data *mmc = dev->platform_data;

	disable_irq(mmc->slots[0].card_detect_irq);
	return 0;
}

static int twl_mmc_resume(struct device *dev, int slot)
{
	struct omap_mmc_platform_data *mmc = dev->platform_data;

	enable_irq(mmc->slots[0].card_detect_irq);
	return 0;
}

#else		// CONFIG_PM
#define twl_mmc_suspend	NULL
#define twl_mmc_resume	NULL
#endif	// CONFIG_PM
// from GB
#endif
//------------------------------------------------------------------------

