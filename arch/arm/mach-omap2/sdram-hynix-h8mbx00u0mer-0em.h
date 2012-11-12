/*
 * SDRC register values for the Hynix H8MBX00U0MER-0EM
 *
 * Copyright (C) 2009 Texas Instruments, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __ARCH_ARM_MACH_OMAP2_SDRAM_HYNIX_H8MBX00U0MER0EM
#define __ARCH_ARM_MACH_OMAP2_SDRAM_HYNIX_H8MBX00U0MER0EM

#include <plat/sdrc.h>

/* Hynix H8MBX00U0MER-0EM */
static struct omap_sdrc_params h8mbx00u0mer0em_sdrc_params[] = {
// from GB
#if 1
	[0] = {
		.rate        = 200000000,
		.actim_ctrla = 0x92E1C4C6,
		.actim_ctrlb = 0x0002121C,
		.rfr_ctrl    = 0x0005E601,
		.mr          = 0x00000032,
	},
	[1] = {
		.rate        = 100000000,
		.actim_ctrla = 0x4A15B485,
		.actim_ctrlb = 0x0001120e,
		.rfr_ctrl    = 0x0002da01,
		.mr          = 0x00000032,
	},
	[2] = {
		.rate        = 0
	},
#else
	[0] = {
		.rate        = 200000000,
		.actim_ctrla = 0xa2e1b4c6,
		.actim_ctrlb = 0x0002131c,
		.rfr_ctrl    = 0x0005e601,
		.mr          = 0x00000032,
	},
	[1] = {
		.rate        = 166000000,
		.actim_ctrla = 0x629db4c6,
		.actim_ctrlb = 0x00012214,
		.rfr_ctrl    = 0x0004dc01,
		.mr          = 0x00000032,
	},
	[2] = {
		.rate        = 100000000,
		.actim_ctrla = 0x51912284,
		.actim_ctrlb = 0x0002120e,
		.rfr_ctrl    = 0x0002d101,
		.mr          = 0x00000022,
	},
	[3] = {
		.rate        = 83000000,
		.actim_ctrla = 0x31512283,
		.actim_ctrlb = 0x0001220a,
		.rfr_ctrl    = 0x00025501,
		.mr          = 0x00000022,
	},
	[4] = {
		.rate        = 0
	},
#endif
};

#endif
