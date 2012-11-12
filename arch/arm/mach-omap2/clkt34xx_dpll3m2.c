/*
 * OMAP34xx M2 divider clock code
 *
 * Copyright (C) 2007-2008 Texas Instruments, Inc.
 * Copyright (C) 2007-2010 Nokia Corporation
 *
 * Paul Walmsley
 * Jouni Högander
 *
 * Parts of this code are based on code written by
 * Richard Woodruff, Tony Lindgren, Tuukka Tikkanen, Karthik Dasu
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#undef DEBUG

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/clk.h>
#include <linux/io.h>

#include <plat/clock.h>
#include <plat/sram.h>
#include <plat/sdrc.h>

#include "clock.h"
#include "clock3xxx.h"
#include "clock34xx.h"
#include "sdrc.h"
#include "cm2xxx_3xxx.h"
#include "cm-regbits-34xx.h"

#define CYCLES_PER_MHZ			1000000

/*
 * CORE DPLL (DPLL3) M2 divider rate programming functions
 *
 * These call into SRAM code to do the actual CM writes, since the SDRAM
 * is clocked from DPLL3.
 */

/**
 * omap3_core_dpll_m2_set_rate - set CORE DPLL M2 divider
 * @clk: struct clk * of DPLL to set
 * @rate: rounded target rate
 *
 * Program the DPLL M2 divider with the rounded target rate.  Returns
 * -EINVAL upon error, or 0 upon success.
 */
int omap3_core_dpll_m2_set_rate(struct clk *clk, unsigned long rate)
{
	u32 new_div = 0;
	u32 unlock_dll = 0;
	u32 c;
	unsigned long validrate, sdrcrate, _mpurate;
	struct omap_sdrc_params *sdrc_cs0;
	struct omap_sdrc_params *sdrc_cs1;
	int ret;

	if (!clk || !rate)
	{
		//printk("clk # rate compare fail\n");/*[LGE_CHANGED] 20120816 pyocool.cho@lge.com  timming setting*/
		return -EINVAL;
	}

	validrate = omap2_clksel_round_rate_div(clk, rate, &new_div);
	if (validrate != rate)
	{
		//printk("validrate & rate comapare fail\n");/*[LGE_CHANGED] 20120816 pyocool.cho@lge.com  timming setting*/
		return -EINVAL;
	}

	sdrcrate = sdrc_ick_p->rate;
	//printk("sdrcrate 0x%x\n",sdrcrate);/*[LGE_CHANGED] 20120816 pyocool.cho@lge.com  timming setting*/
	if (rate > clk->rate)
	{
		sdrcrate <<= ((rate / clk->rate) >> 1);
		//printk(" not small:: change sdrcrate 0x%x\n",sdrcrate);/*[LGE_CHANGED] 20120816 pyocool.cho@lge.com  timming setting*/
	}
	else
	{
		sdrcrate >>= ((clk->rate / rate) >> 1);
		//printk("small:: change sdrcrate 0x%x\n",sdrcrate);/*[LGE_CHANGED] 20120816 pyocool.cho@lge.com  timming setting*/
	}

	ret = omap2_sdrc_get_params(sdrcrate, &sdrc_cs0, &sdrc_cs1);
	if (ret)
	{
		//printk("omap2_sdrc_get_params fail\n");/*[LGE_CHANGED] 20120816 pyocool.cho@lge.com  timming setting*/
		return -EINVAL;
	}

	if (sdrcrate < MIN_SDRC_DLL_LOCK_FREQ) 
	{
		//printk("clock: will unlock SDRC DLL\n");/*[LGE_CHANGED] 20120816 pyocool.cho@lge.com  timming setting*/
		unlock_dll = 1;
	}

	/*
	 * XXX This only needs to be done when the CPU frequency changes
	 */
	_mpurate = arm_fck_p->rate / CYCLES_PER_MHZ;
	c = (_mpurate << SDRC_MPURATE_SCALE) >> SDRC_MPURATE_BASE_SHIFT;
	c += 1;  /* for safety */
	c *= SDRC_MPURATE_LOOPS;
	c >>= SDRC_MPURATE_SCALE;
	if (c == 0)
		c = 1;
	
	pr_debug("clock: changing CORE DPLL rate from %lu to %lu\n", clk->rate,validrate);/*[LGE_CHANGED] 20120816 pyocool.cho@lge.com  timming setting*/
	pr_debug("clock: SDRC CS0 timing params used:"
		 " RFR %08x CTRLA %08x CTRLB %08x MR %08x\n",
		 sdrc_cs0->rfr_ctrl, sdrc_cs0->actim_ctrla,
		 sdrc_cs0->actim_ctrlb, sdrc_cs0->mr);/*[LGE_CHANGED] 20120816 pyocool.cho@lge.com  timming setting*/
	if (sdrc_cs1)
		pr_debug("clock: SDRC CS1 timing params used: "
		 " RFR %08x CTRLA %08x CTRLB %08x MR %08x\n",
		 sdrc_cs1->rfr_ctrl, sdrc_cs1->actim_ctrla,
		 sdrc_cs1->actim_ctrlb, sdrc_cs1->mr);/*[LGE_CHANGED] 20120816 pyocool.cho@lge.com  timming setting*/

	if (sdrc_cs1)
		omap3_configure_core_dpll(
				  new_div, unlock_dll, c, rate > clk->rate,
				  sdrc_cs0->rfr_ctrl, sdrc_cs0->actim_ctrla,
				  sdrc_cs0->actim_ctrlb, sdrc_cs0->mr,
				  sdrc_cs1->rfr_ctrl, sdrc_cs1->actim_ctrla,
				  sdrc_cs1->actim_ctrlb, sdrc_cs1->mr);
	else
		omap3_configure_core_dpll(
				  new_div, unlock_dll, c, rate > clk->rate,
				  sdrc_cs0->rfr_ctrl, sdrc_cs0->actim_ctrla,
				  sdrc_cs0->actim_ctrlb, sdrc_cs0->mr,
				  0, 0, 0, 0);
	clk->rate = rate;

	return 0;
}


int omap3_core_l3_set_rate(struct clk *clk, unsigned long rate)
{
	struct clk *dpll3_m2_ck = clk_get(NULL, "dpll3_m2_ck");
	int l3_div, ret;

	l3_div = omap2_cm_read_mod_reg(CORE_MOD, CM_CLKSEL) &
		OMAP3430_CLKSEL_L3_MASK;
	ret = omap3_core_dpll_m2_set_rate(dpll3_m2_ck, (rate * l3_div));

	clk_put(dpll3_m2_ck);

	clk->rate = dpll3_m2_ck->rate / l3_div;
	return ret;
}

long omap3_core_l3_round_rate(struct clk *clk, unsigned long target_rate)
{
	struct clk *dpll3_m2_ck = clk_get(NULL, "dpll3_m2_ck");
	long m2_clk;
	int l3_div;
	u32 new_div;

	l3_div = omap2_cm_read_mod_reg(CORE_MOD, CM_CLKSEL) &
		OMAP3430_CLKSEL_L3_MASK;

	m2_clk = omap2_clksel_round_rate_div(clk, (target_rate * l3_div),
					&new_div);
	clk_put(dpll3_m2_ck);
	return m2_clk / l3_div;
}

