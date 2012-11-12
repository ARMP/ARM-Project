/*
 * Copyright (C) 2009 Texas Instruments Inc.
 *
 * Modified from mach-omap2/board-zoom2.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/input/matrix_keypad.h>
#include <linux/gpio.h>
#include <linux/i2c/twl.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/fixed.h>
#include <linux/wl12xx.h>
#include <linux/mmc/host.h>
#include <linux/synaptics_i2c_rmi.h>
#include <linux/leds-omap4430sdp-display.h>

#include <media/v4l2-int-device.h>

#ifdef CONFIG_PANEL_SIL9022
#include <mach/sil9022.h>
#endif


#include <asm/mach-types.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>

#include <plat/common.h>
#include <plat/usb.h>
#include <linux/switch.h>
#include <mach/board-zoom.h>

#include "mux.h"
#include "hsmmc.h"
#include "common-board-devices.h"
#include "twl4030.h"

#define OMAP_ZOOM_WLAN_PMENA_GPIO	(101)
#define OMAP_ZOOM_WLAN_IRQ_GPIO		(162)
#define OMAP_SYNAPTICS_GPIO			(163)

/* PWM output/clock enable for LCD backlight*/
#define REG_INTBR_GPBR1				(0xc)
#define REG_INTBR_GPBR1_PWM1_OUT_EN		(0x1 << 3)
#define REG_INTBR_GPBR1_PWM1_OUT_EN_MASK	(0x1 << 3)
#define REG_INTBR_GPBR1_PWM1_CLK_EN		(0x1 << 1)
#define REG_INTBR_GPBR1_PWM1_CLK_EN_MASK	(0x1 << 1)

/* pin mux for LCD backlight*/
#define REG_INTBR_PMBR1				(0xd)
#define REG_INTBR_PMBR1_PWM1_PIN_EN		(0x3 << 4)
#define REG_INTBR_PMBR1_PWM1_PIN_MASK		(0x3 << 4)

#define MAX_CYCLES				(0x7f)
#define MIN_CYCLES				(75)
#define LCD_PANEL_ENABLE_GPIO			(7 + OMAP_MAX_GPIO_LINES)

/* Zoom2 has Qwerty keyboard*/
static uint32_t board_keymap[] = {
	KEY(0, 0, KEY_E),
	KEY(0, 1, KEY_R),
	KEY(0, 2, KEY_T),
	KEY(0, 3, KEY_HOME),
	KEY(0, 6, KEY_I),
	KEY(0, 7, KEY_LEFTSHIFT),
	KEY(1, 0, KEY_D),
	KEY(1, 1, KEY_F),
	KEY(1, 2, KEY_G),
	KEY(1, 3, KEY_SEND),
	KEY(1, 6, KEY_K),
	KEY(1, 7, KEY_ENTER),
	KEY(2, 0, KEY_X),
	KEY(2, 1, KEY_C),
	KEY(2, 2, KEY_V),
	KEY(2, 3, KEY_END),
	KEY(2, 6, KEY_DOT),
	KEY(2, 7, KEY_CAPSLOCK),
	KEY(3, 0, KEY_Z),
	KEY(3, 1, KEY_KPPLUS),
	KEY(3, 2, KEY_B),
	KEY(3, 3, KEY_F1),
	KEY(3, 6, KEY_O),
	KEY(3, 7, KEY_SPACE),
	KEY(4, 0, KEY_W),
	KEY(4, 1, KEY_Y),
	KEY(4, 2, KEY_U),
	KEY(4, 3, KEY_F2),
	KEY(4, 4, KEY_VOLUMEUP),
	KEY(4, 6, KEY_L),
	KEY(4, 7, KEY_LEFT),
	KEY(5, 0, KEY_S),
	KEY(5, 1, KEY_H),
	KEY(5, 2, KEY_J),
	KEY(5, 3, KEY_F3),
	KEY(5, 4, KEY_UNKNOWN),
	KEY(5, 5, KEY_VOLUMEDOWN),
	KEY(5, 6, KEY_M),
	KEY(5, 7, KEY_RIGHT),
	KEY(6, 0, KEY_Q),
	KEY(6, 1, KEY_A),
	KEY(6, 2, KEY_N),
	KEY(6, 3, KEY_BACKSPACE),
	KEY(6, 6, KEY_P),
	KEY(6, 7, KEY_UP),
	KEY(7, 0, KEY_PROG1),	/*MACRO 1 <User defined> */
	KEY(7, 1, KEY_PROG2),	/*MACRO 2 <User defined> */
	KEY(7, 2, KEY_PROG3),	/*MACRO 3 <User defined> */
	KEY(7, 3, KEY_PROG4),	/*MACRO 4 <User defined> */
	KEY(7, 6, KEY_SELECT),
	KEY(7, 7, KEY_DOWN)
};

static struct matrix_keymap_data board_map_data = {
	.keymap			= board_keymap,
	.keymap_size		= ARRAY_SIZE(board_keymap),
};

static struct twl4030_keypad_data zoom_kp_twl4030_data = {
	.keymap_data	= &board_map_data,
	.rows		= 8,
	.cols		= 8,
	.rep		= 1,
};
static struct __initdata twl4030_power_data zoom_t2scripts_data;

static struct regulator_consumer_supply zoom_vmmc1_supply = {
	.supply		= "vmmc",
};

static struct regulator_consumer_supply zoom_vsim_supply = {
	.supply		= "vmmc_aux",
};

static struct regulator_consumer_supply zoom_vmmc2_supply = {
	.supply		= "vmmc",
};

static struct regulator_consumer_supply zoom_vmmc3_supply = {
	.supply		= "vmmc",
	.dev_name	= "omap_hsmmc.2",
};

/* VMMC1 for OMAP VDD_MMC1 (i/o) and MMC1 card */
static struct regulator_init_data zoom_vmmc1 = {
	.constraints = {
		.min_uV			= 1850000,
		.max_uV			= 3150000,
		.valid_modes_mask	= REGULATOR_MODE_NORMAL
					| REGULATOR_MODE_STANDBY,
		.valid_ops_mask		= REGULATOR_CHANGE_VOLTAGE
					| REGULATOR_CHANGE_MODE
					| REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies  = 1,
	.consumer_supplies      = &zoom_vmmc1_supply,
};

/* VMMC2 for MMC2 card */
static struct regulator_init_data zoom_vmmc2 = {
	.constraints = {
		.min_uV			= 1850000,
		.max_uV			= 1850000,
		.apply_uV		= true,
		.valid_modes_mask	= REGULATOR_MODE_NORMAL
					| REGULATOR_MODE_STANDBY,
		.valid_ops_mask		= REGULATOR_CHANGE_MODE
					| REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies  = 1,
	.consumer_supplies      = &zoom_vmmc2_supply,
};

/* VSIM for OMAP VDD_MMC1A (i/o for DAT4..DAT7) */
static struct regulator_init_data zoom_vsim = {
	.constraints = {
		.min_uV			= 1800000,
		.max_uV			= 3000000,
		.valid_modes_mask	= REGULATOR_MODE_NORMAL
					| REGULATOR_MODE_STANDBY,
		.valid_ops_mask		= REGULATOR_CHANGE_VOLTAGE
					| REGULATOR_CHANGE_MODE
					| REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies  = 1,
	.consumer_supplies      = &zoom_vsim_supply,
};

static struct gpio_switch_platform_data headset_switch_data = {
	.name		= "h2w",
	.gpio		= OMAP_MAX_GPIO_LINES + 2, /* TWL4030 GPIO_2 */
};

static struct platform_device headset_switch_device = {
	.name		= "switch-gpio",
	.id		= -1,
	.dev		= {
		.platform_data = &headset_switch_data,
	}
};

static struct regulator_init_data zoom_vmmc3 = {
	.constraints = {
		.valid_ops_mask	= REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies	= 1,
	.consumer_supplies = &zoom_vmmc3_supply,
};

static struct fixed_voltage_config zoom_vwlan = {
	.supply_name		= "vwl1271",
	.microvolts		= 1800000, /* 1.8V */
	.gpio			= OMAP_ZOOM_WLAN_PMENA_GPIO,
	.startup_delay		= 70000, /* 70msec */
	.enable_high		= 1,
	.enabled_at_boot	= 0,
	.init_data		= &zoom_vmmc3,
};

static struct platform_device *zoom_board_devices[] __initdata = {
	&headset_switch_device,
};

static struct platform_device omap_vwlan_device = {
	.name		= "reg-fixed-voltage",
	.id		= 1,
	.dev = {
		.platform_data	= &zoom_vwlan,
	},
};

static struct wl12xx_platform_data omap_zoom_wlan_data __initdata = {
	.irq = OMAP_GPIO_IRQ(OMAP_ZOOM_WLAN_IRQ_GPIO),
	/* ZOOM ref clock is 26 MHz */
	.board_ref_clock = 1,
};

static void zoom_pwm_config(u8 brightness)
{
	u8 pwm_off = 0;

	pwm_off = (MIN_CYCLES * (LED_FULL - brightness) +
		   MAX_CYCLES * (brightness - LED_OFF)) /
		(LED_FULL - LED_OFF);

	pwm_off = clamp(pwm_off, (u8)MIN_CYCLES, (u8)MAX_CYCLES);

	/* start at 0 */
	twl_i2c_write_u8(TWL4030_MODULE_PWM1, 0, 0);
	twl_i2c_write_u8(TWL4030_MODULE_PWM1, pwm_off, 1);
}

static void zoom_pwm_enable(int enable)
{
	u8 gpbr1;

	twl_i2c_read_u8(TWL4030_MODULE_INTBR, &gpbr1, REG_INTBR_GPBR1);
	gpbr1 &= ~REG_INTBR_GPBR1_PWM1_OUT_EN_MASK;
	gpbr1 |= (enable ? REG_INTBR_GPBR1_PWM1_OUT_EN : 0);
	twl_i2c_write_u8(TWL4030_MODULE_INTBR, gpbr1, REG_INTBR_GPBR1);

	twl_i2c_read_u8(TWL4030_MODULE_INTBR, &gpbr1, REG_INTBR_GPBR1);
	gpbr1 &= ~REG_INTBR_GPBR1_PWM1_CLK_EN_MASK;
	gpbr1 |= (enable ? REG_INTBR_GPBR1_PWM1_CLK_EN : 0);
	twl_i2c_write_u8(TWL4030_MODULE_INTBR, gpbr1, REG_INTBR_GPBR1);
}

static void zoom_set_primary_brightness(u8 brightness)
{
	u8 pmbr1;
	static int zoom_pwm1_config;
	static int zoom_pwm1_output_enabled;

	if (zoom_pwm1_config == 0) {
		twl_i2c_read_u8(TWL4030_MODULE_INTBR, &pmbr1, REG_INTBR_PMBR1);

		pmbr1 &= ~REG_INTBR_PMBR1_PWM1_PIN_MASK;
		pmbr1 |=  REG_INTBR_PMBR1_PWM1_PIN_EN;
		twl_i2c_write_u8(TWL4030_MODULE_INTBR, pmbr1, REG_INTBR_PMBR1);

		zoom_pwm1_config = 1;
	}

	if (!brightness) {
		zoom_pwm_enable(0);
		zoom_pwm1_output_enabled = 0;
		return;
	}

	zoom_pwm_config(brightness);
	if (zoom_pwm1_output_enabled == 0) {
		zoom_pwm_enable(1);
		zoom_pwm1_output_enabled = 1;
	}
}

static struct omap4430_sdp_disp_led_platform_data zoom_disp_led_data = {
	.flags = LEDS_CTRL_AS_ONE_DISPLAY,
	.primary_display_set = zoom_set_primary_brightness,
	.secondary_display_set = NULL,
};


static struct platform_device zoom_disp_led = {
	.name   =       "display_led",
	.id     =       -1,
	.dev    = {
		.platform_data = &zoom_disp_led_data,
	},
};

static struct omap2_hsmmc_info mmc[] = {
	{
		.name		= "external",
		.mmc		= 1,
		.caps		= MMC_CAP_4_BIT_DATA,
		.gpio_wp	= -EINVAL,
		.power_saving	= true,
	},
	{
		.name		= "internal",
		.mmc		= 2,
		.caps		= MMC_CAP_4_BIT_DATA | MMC_CAP_8_BIT_DATA,
		.gpio_cd	= -EINVAL,
		.gpio_wp	= -EINVAL,
		.nonremovable	= true,
		.power_saving	= true,
	},
	{
		.name		= "wl1271",
		.mmc		= 3,
		.caps		= MMC_CAP_4_BIT_DATA | MMC_CAP_POWER_OFF_CARD,
		.gpio_wp	= -EINVAL,
		.gpio_cd	= -EINVAL,
		.nonremovable	= true,
	},
	{}      /* Terminator */
};

static struct regulator_consumer_supply zoom_vpll2_supplies[] = {
	REGULATOR_SUPPLY("vdds_dsi", "omapdss"),
	REGULATOR_SUPPLY("vdds_dsi", "omapdss_dsi1"),
};

static struct regulator_consumer_supply zoom_vdda_dac_supply =
	REGULATOR_SUPPLY("vdda_dac", "omapdss_venc");

static struct regulator_init_data zoom_vpll2 = {
	.constraints = {
		.min_uV                 = 1800000,
		.max_uV                 = 1800000,
		.valid_modes_mask       = REGULATOR_MODE_NORMAL
					| REGULATOR_MODE_STANDBY,
		.valid_ops_mask         = REGULATOR_CHANGE_MODE
					| REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies		= ARRAY_SIZE(zoom_vpll2_supplies),
	.consumer_supplies		= zoom_vpll2_supplies,
};

static struct regulator_init_data zoom_vdac = {
	.constraints = {
		.min_uV                 = 1800000,
		.max_uV                 = 1800000,
		.valid_modes_mask       = REGULATOR_MODE_NORMAL
					| REGULATOR_MODE_STANDBY,
		.valid_ops_mask         = REGULATOR_CHANGE_MODE
					| REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies		= 1,
	.consumer_supplies		= &zoom_vdda_dac_supply,
};

static int zoom_twl_gpio_setup(struct device *dev,
		unsigned gpio, unsigned ngpio)
{
	int ret;

	/* gpio + 0 is "mmc0_cd" (input/IRQ) */
	mmc[0].gpio_cd = gpio + 0;
	omap2_hsmmc_init(mmc);

	/* link regulators to MMC adapters ... we "know" the
	 * regulators will be set up only *after* we return.
	*/
	zoom_vmmc1_supply.dev = mmc[0].dev;
	zoom_vsim_supply.dev = mmc[0].dev;
	zoom_vmmc2_supply.dev = mmc[1].dev;

	ret = gpio_request_one(LCD_PANEL_ENABLE_GPIO, GPIOF_OUT_INIT_LOW,
			       "lcd enable");
	if (ret)
		pr_err("Failed to get LCD_PANEL_ENABLE_GPIO (gpio%d).\n",
				LCD_PANEL_ENABLE_GPIO);

	return ret;
}

/* EXTMUTE callback function */
static void zoom2_set_hs_extmute(int mute)
{
	gpio_set_value(ZOOM2_HEADSET_EXTMUTE_GPIO, mute);
}

static int zoom_batt_table[] = {
/* 0 C*/
30800, 29500, 28300, 27100,
26000, 24900, 23900, 22900, 22000, 21100, 20300, 19400, 18700, 17900,
17200, 16500, 15900, 15300, 14700, 14100, 13600, 13100, 12600, 12100,
11600, 11200, 10800, 10400, 10000, 9630,  9280,  8950,  8620,  8310,
8020,  7730,  7460,  7200,  6950,  6710,  6470,  6250,  6040,  5830,
5640,  5450,  5260,  5090,  4920,  4760,  4600,  4450,  4310,  4170,
4040,  3910,  3790,  3670,  3550
};

static struct twl4030_bci_platform_data zoom_bci_data = {
	.battery_tmp_tbl	= zoom_batt_table,
	.tblsize		= ARRAY_SIZE(zoom_batt_table),
};

static struct twl4030_usb_data zoom_usb_data = {
	.usb_mode	= T2_USB_MODE_ULPI,
};

static struct twl4030_gpio_platform_data zoom_gpio_data = {
	.gpio_base	= OMAP_MAX_GPIO_LINES,
	.irq_base	= TWL4030_GPIO_IRQ_BASE,
	.irq_end	= TWL4030_GPIO_IRQ_END,
	.setup		= zoom_twl_gpio_setup,
};

static struct twl4030_madc_platform_data zoom_madc_data = {
	.irq_line	= 1,
};

static struct twl4030_codec_audio_data zoom_audio_data;

static struct twl4030_codec_data zoom_codec_data = {
	.audio_mclk = 26000000,
	.audio = &zoom_audio_data,
};

static struct twl4030_platform_data zoom_twldata = {
	.irq_base	= TWL4030_IRQ_BASE,
	.irq_end	= TWL4030_IRQ_END,

	/* platform_data for children goes here */
	.bci		= &zoom_bci_data,
	.madc		= &zoom_madc_data,
	.usb		= &zoom_usb_data,
	.gpio		= &zoom_gpio_data,
	.keypad		= &zoom_kp_twl4030_data,
	.power		= &zoom_t2scripts_data,
	.codec		= &zoom_codec_data,
	.vmmc1          = &zoom_vmmc1,
	.vmmc2          = &zoom_vmmc2,
	.vsim           = &zoom_vsim,
	.vpll2		= &zoom_vpll2,
	.vdac		= &zoom_vdac,
};

static void synaptics_dev_init(void)
{
	/* Set the ts_gpio pin mux */
	omap_mux_init_signal("gpio_163", OMAP_PIN_INPUT_PULLUP);

	if (gpio_request(OMAP_SYNAPTICS_GPIO, "touch") < 0) {
		printk(KERN_ERR "can't get synaptics pen down GPIO\n");
		return;
	}
	gpio_direction_input(OMAP_SYNAPTICS_GPIO);
	gpio_set_debounce(OMAP_SYNAPTICS_GPIO, 310);
}

static int synaptics_power(int power_state)
{
	/* TODO: synaptics is powered by vbatt */
	return 0;
}

static struct synaptics_i2c_rmi_platform_data synaptics_platform_data[] = {
	{
		.version        = 0x0,
		.power          = &synaptics_power,
		.flags          = SYNAPTICS_SWAP_XY,
		.irqflags       = IRQF_TRIGGER_LOW,
	}
};

static struct i2c_board_info __initdata zoom2_i2c_bus2_info[] = {
	{
		I2C_BOARD_INFO(SYNAPTICS_I2C_RMI_NAME,  0x20),
		.platform_data = &synaptics_platform_data,
		.irq = OMAP_GPIO_IRQ(OMAP_SYNAPTICS_GPIO),
	},
#if (defined(CONFIG_VIDEO_IMX046) || defined(CONFIG_VIDEO_IMX046_MODULE)) && \
	defined(CONFIG_VIDEO_OMAP3)
	{
		I2C_BOARD_INFO(IMX046_NAME, IMX046_I2C_ADDR),
		.platform_data = &zoom2_imx046_platform_data,
	},
#endif
#if (defined(CONFIG_VIDEO_LV8093) || defined(CONFIG_VIDEO_LV8093_MODULE)) && \
	defined(CONFIG_VIDEO_OMAP3)
	{
		I2C_BOARD_INFO(LV8093_NAME,  LV8093_AF_I2C_ADDR),
		.platform_data = &zoom2_lv8093_platform_data,
	},
#endif
};

static struct i2c_board_info __initdata zoom2_i2c_bus3_info[] = {
#ifdef CONFIG_PANEL_SIL9022
	{
		I2C_BOARD_INFO(SIL9022_DRV_NAME, SI9022_I2CSLAVEADDRESS),
	},
#endif
};
static int __init omap_i2c_init(void)
{
	if (machine_is_omap_zoom2()) {
		zoom_audio_data.ramp_delay_value = 3;	/* 161 ms */
		zoom_audio_data.hs_extmute = 1;
		zoom_audio_data.set_hs_extmute = zoom2_set_hs_extmute;
	}
	omap_pmic_init(1, 2400, "twl5030", INT_34XX_SYS_NIRQ, &zoom_twldata);
	omap_register_i2c_bus(2, 100, zoom2_i2c_bus2_info,
			ARRAY_SIZE(zoom2_i2c_bus2_info));
	omap_register_i2c_bus(3, 400, zoom2_i2c_bus3_info,
			ARRAY_SIZE(zoom2_i2c_bus3_info));
	return 0;
}

static void enable_board_wakeup_source(void)
{
	/* T2 interrupt line (keypad) */
	omap_mux_init_signal("sys_nirq",
		OMAP_WAKEUP_EN | OMAP_PIN_INPUT_PULLUP);
}

void __init zoom_peripherals_init(void)
{
	platform_add_devices(zoom_board_devices,
		ARRAY_SIZE(zoom_board_devices));
	twl4030_get_scripts(&zoom_t2scripts_data);
	omap_i2c_init();
	synaptics_dev_init();
	platform_device_register(&omap_vwlan_device);
	platform_device_register(&zoom_disp_led);
	usb_musb_init(NULL);
	enable_board_wakeup_source();
	omap_serial_init();
	zoom2_cam_init();
	#ifdef CONFIG_PANEL_SIL9022
	config_hdmi_gpio();
	zoom_hdmi_reset_enable(1);
	#endif
}
