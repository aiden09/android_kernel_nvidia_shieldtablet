/*
 * arch/arm/mach-tegra/board-p2360-power.c
 * Based on arch/arm/mach-tegra/board-p1859-power.c
 *
 * Copyright (c) 2013-2014, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/mfd/max77663-core.h>
#include <linux/regulator/max77663-regulator.h>
#include <linux/regulator/max15569-regulator.h>
#include <linux/gpio.h>

#include "pm.h"
#include "board.h"
#include "gpio-names.h"
#include "board-common.h"
#include "board-p2360.h"
#include "tegra_cl_dvfs.h"
#include "devices.h"
#include <mach/board_id.h>
#include "vcm30_t124.h"

static struct max77663_gpio_config max77663_gpio_cfgs[] = {
	{
		.gpio = MAX77663_GPIO5,
		.dir = GPIO_DIR_OUT,
		.dout = GPIO_DOUT_HIGH,
		.out_drv = GPIO_OUT_DRV_PUSH_PULL,
		.alternate = GPIO_ALT_DISABLE,
	},
};

static struct max77663_platform_data max77663_pdata = {
	.irq_base	= MAX77663_IRQ_BASE,
	.gpio_base	= MAX77663_GPIO_BASE,

	.num_gpio_cfgs	= ARRAY_SIZE(max77663_gpio_cfgs),
	.gpio_cfgs	= max77663_gpio_cfgs,

	.use_power_off	= false,
};

static struct i2c_board_info __initdata max77663_regulators[] = {
	{
		/* The I2C address was determined by OTP factory setting */
		I2C_BOARD_INFO("max77663", 0x3c),
		.irq		= -1,
		.platform_data	= &max77663_pdata,
	},
};

/* MAX15569 switching regulator for vdd_cpu */
static struct regulator_consumer_supply max15569_vddcpu_supply[] = {
	REGULATOR_SUPPLY("vdd_cpu", NULL),
};

static struct regulator_init_data max15569_vddcpu_init_data = {
	.constraints = {
		.min_uV = 500000,
		.max_uV = 1520000,
		.valid_modes_mask = (REGULATOR_MODE_NORMAL |
				REGULATOR_MODE_STANDBY),
		.valid_ops_mask = (REGULATOR_CHANGE_MODE |
				REGULATOR_CHANGE_STATUS |
				REGULATOR_CHANGE_CONTROL |
				REGULATOR_CHANGE_VOLTAGE),
		.always_on = 1,
		.boot_on =  1,
		.apply_uV = 0,
	},
	.num_consumer_supplies = ARRAY_SIZE(max15569_vddcpu_supply),
	.consumer_supplies = max15569_vddcpu_supply,
};

static struct max15569_regulator_platform_data max15569_vddcpu_pdata = {
	.reg_init_data = &max15569_vddcpu_init_data,
	.max_voltage_uV = 1520000,
	.slew_rate_mv_per_us = 44,
};

static struct i2c_board_info __initdata max15569_vddcpu_boardinfo[] = {
	{
		I2C_BOARD_INFO("max15569", 0x3a),
		.platform_data  = &max15569_vddcpu_pdata,
	},
};

#ifdef CONFIG_ARCH_TEGRA_HAS_CL_DVFS
/* board parameters for cpu dfll */
static struct tegra_cl_dvfs_cfg_param p2360_cl_dvfs_param = {
	.sample_rate = 12500,

	.force_mode = TEGRA_CL_DVFS_FORCE_FIXED,
	.cf = 10,
	.ci = 0,
	.cg = 2,

	.droop_cut_value = 0xF,
	.droop_restore_ramp = 0x0,
	.scale_out_ramp = 0x0,
};

/* MAX15569: fixed 10mV steps from 600mV to 1400mV, with offset 0x0b */
#define PMU_CPU_VDD_MAP_SIZE ((1400000 - 600000) / 10000 + 1)
static struct voltage_reg_map pmu_cpu_vdd_map[PMU_CPU_VDD_MAP_SIZE];
static inline void fill_reg_map(void)
{
	int i;
	for (i = 0; i < PMU_CPU_VDD_MAP_SIZE; i++) {
		pmu_cpu_vdd_map[i].reg_value = i + 0x0b;
		pmu_cpu_vdd_map[i].reg_uV = 600000 + 10000 * i;
	}
}

static struct tegra_cl_dvfs_platform_data p2360_cl_dvfs_data = {
	.dfll_clk_name = "dfll_cpu",
	.pmu_if = TEGRA_CL_DVFS_PMU_I2C,
	.u.pmu_i2c = {
		.fs_rate = 400000,
		.slave_addr = 0x74,
		.reg = 0x07,
	},
	.vdd_map = pmu_cpu_vdd_map,
	.vdd_map_size = PMU_CPU_VDD_MAP_SIZE,
	.flags = TEGRA_CL_DVFS_DYN_OUTPUT_CFG,
	.cfg_param = &p2360_cl_dvfs_param,
};

static int __init p2360_cl_dvfs_init(void)
{
	fill_reg_map();
	tegra_cl_dvfs_device.dev.platform_data = &p2360_cl_dvfs_data;
	platform_device_register(&tegra_cl_dvfs_device);

	return 0;
}
#endif

/* MAX15569 switching regulator for vdd_gpu */
static struct regulator_consumer_supply max15569_vddgpu_supply[] = {
	REGULATOR_SUPPLY("vdd_gpu", NULL),
};

static struct regulator_init_data max15569_vddgpu_init_data = {
	.constraints = {
		.min_uV = 500000,
		.max_uV = 1520000,
		.valid_modes_mask = (REGULATOR_MODE_NORMAL |
				REGULATOR_MODE_STANDBY),
		.valid_ops_mask = (REGULATOR_CHANGE_MODE |
				REGULATOR_CHANGE_STATUS |
				REGULATOR_CHANGE_CONTROL |
				REGULATOR_CHANGE_VOLTAGE),
		.always_on = 0,
		.boot_on =  0,
		.apply_uV = 0,
		.enable_time = 210, /* for ramp up time in usec */
	},
	.num_consumer_supplies = ARRAY_SIZE(max15569_vddgpu_supply),
	.consumer_supplies = max15569_vddgpu_supply,
};

static struct max15569_regulator_platform_data max15569_vddgpu_pdata = {
	.reg_init_data = &max15569_vddgpu_init_data,
	.max_voltage_uV = 1400000,
	.slew_rate_mv_per_us = 44,
};

static struct i2c_board_info __initdata max15569_vddgpu_boardinfo[] = {
	{
		I2C_BOARD_INFO("max15569", 0x3b),
		.platform_data  = &max15569_vddgpu_pdata,
	},
};

int __init p2360_regulator_init(void)
{
	tegra_pmc_pmu_interrupt_polarity(true);

#ifdef CONFIG_ARCH_TEGRA_HAS_CL_DVFS
	p2360_cl_dvfs_init();
#endif

	max15569_vddgpu_pdata.ena_gpio = TEGRA_GPIO_PR2;

	i2c_register_board_info(4, max77663_regulators,
				ARRAY_SIZE(max77663_regulators));

	i2c_register_board_info(4, max15569_vddcpu_boardinfo, 1);
	i2c_register_board_info(4, max15569_vddgpu_boardinfo, 1);

	return 0;
}