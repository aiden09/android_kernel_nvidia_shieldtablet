/*
 * arch/arm/mach-tegra/board-t210ref-power.c
 *
 * Copyright (c) 2013-2014, NVIDIA CORPORATION. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/resource.h>
#include <linux/io.h>
#include <mach/edp.h>
#include <mach/irqs.h>
#include <linux/edp.h>
#include <linux/of_platform.h>
#include <linux/platform_data/tegra_edp.h>
#include <linux/pid_thermal_gov.h>
#include <linux/regulator/fixed.h>
#include <linux/regulator/machine.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/regulator/tegra-dfll-bypass-regulator.h>
#include <linux/tegra-fuse.h>
#include <linux/tegra-pmc.h>
#include <linux/tegra_soctherm.h>
#include <linux/pinctrl/pinconf-tegra.h>
#include <linux/nct1008.h>

#include <asm/mach-types.h>

#include "pm.h"
#include "dvfs.h"
#include "board.h"
#include "common.h"
#include "tegra-board-id.h"
#include "board-pmu-defines.h"
#include "board-common.h"
#include "board-t210ref.h"
#include "board-pmu-defines.h"
#include "devices.h"
#include "iomap.h"
#include "tegra_cl_dvfs.h"
#include "cpu-tegra.h"

static u32 tegra_chip_id;

int __init t210ref_edp_init(void)
{
	unsigned int regulator_mA;
	regulator_mA = get_maximum_cpu_current_supported();
	if (!regulator_mA) {
		regulator_mA = 14000;
	}

	pr_info("%s: CPU regulator %d mA\n", __func__, regulator_mA);
	tegra_init_cpu_edp_limits(regulator_mA);

	/* gpu maximum current */
	regulator_mA = 12000;
	pr_info("%s: GPU regulator %d mA\n", __func__, regulator_mA);
	tegra_init_gpu_edp_limits(regulator_mA);

	return 0;
}

static struct pid_thermal_gov_params soctherm_pid_params = {
	.max_err_temp = 9000,
	.max_err_gain = 1000,

	.gain_p = 1000,
	.gain_d = 0,

	.up_compensation = 20,
	.down_compensation = 20,
};

static struct thermal_zone_params soctherm_tzp = {
	.governor_name = "pid_thermal_gov",
	.governor_params = &soctherm_pid_params,
};

static struct tegra_thermtrip_pmic_data tpdata_max77620 = {
	.reset_tegra = 1,
	.pmu_16bit_ops = 0,
	.controller_type = 0,
	.pmu_i2c_addr = 0x3c,
	.i2c_controller_id = 4,
	.poweroff_reg_addr = 0x41,
	.poweroff_reg_data = 0x80,
};

static struct soctherm_platform_data t210ref_soctherm_data = {
	.oc_irq_base = TEGRA_SOC_OC_IRQ_BASE,
	.num_oc_irqs = TEGRA_SOC_OC_NUM_IRQ,
	.therm = {
		[THERM_CPU] = {
			.zone_enable = true,
			.passive_delay = 1000,
			.hotspot_offset = 6000,
			.num_trips = 3,
			.trips = {
				{
					.cdev_type = "tegra-shutdown",
					.trip_temp = 101000,
					.trip_type = THERMAL_TRIP_CRITICAL,
					.upper = THERMAL_NO_LIMIT,
					.lower = THERMAL_NO_LIMIT,
				},
				{
					.cdev_type = "tegra-heavy",
					.trip_temp = 99000,
					.trip_type = THERMAL_TRIP_HOT,
					.upper = THERMAL_NO_LIMIT,
					.lower = THERMAL_NO_LIMIT,
				},
				{
					.cdev_type = "cpu-balanced",
					.trip_temp = 90000,
					.trip_type = THERMAL_TRIP_PASSIVE,
					.upper = THERMAL_NO_LIMIT,
					.lower = THERMAL_NO_LIMIT,
				},
			},
			.tzp = &soctherm_tzp,
		},
		[THERM_GPU] = {
			.zone_enable = true,
			.passive_delay = 1000,
			.hotspot_offset = 6000,
			.num_trips = 3,
			.trips = {
				{
					.cdev_type = "tegra-shutdown",
					.trip_temp = 101000,
					.trip_type = THERMAL_TRIP_CRITICAL,
					.upper = THERMAL_NO_LIMIT,
					.lower = THERMAL_NO_LIMIT,
				},
				{
					.cdev_type = "tegra-heavy",
					.trip_temp = 99000,
					.trip_type = THERMAL_TRIP_HOT,
					.upper = THERMAL_NO_LIMIT,
					.lower = THERMAL_NO_LIMIT,
				},
				{
					.cdev_type = "gpu-balanced",
					.trip_temp = 90000,
					.trip_type = THERMAL_TRIP_PASSIVE,
					.upper = THERMAL_NO_LIMIT,
					.lower = THERMAL_NO_LIMIT,
				},
			},
			.tzp = &soctherm_tzp,
		},
		[THERM_MEM] = {
			.zone_enable = true,
			.num_trips = 1,
			.trips = {
				{
					.cdev_type = "tegra-shutdown",
					.trip_temp = 101000, /* = GPU shut */
					.trip_type = THERMAL_TRIP_CRITICAL,
					.upper = THERMAL_NO_LIMIT,
					.lower = THERMAL_NO_LIMIT,
				},
			},
			.tzp = &soctherm_tzp,
		},
		[THERM_PLL] = {
			.zone_enable = true,
			.tzp = &soctherm_tzp,
		},
	},
	.throttle = {
		[THROTTLE_HEAVY] = {
			.priority = 100,
			.devs = {
				[THROTTLE_DEV_CPU] = {
					.enable = true,
					.depth = 80,
					.throttling_depth = "heavy_throttling",
				},
				[THROTTLE_DEV_GPU] = {
					.enable = true,
					.throttling_depth = "heavy_throttling",
				},
			},
		},
	},
};

/* Only the diffs from t210ref_soctherm_data structure */
static struct soctherm_platform_data t132ref_v1_soctherm_data = {
	.therm = {
		[THERM_CPU] = {
			.zone_enable = true,
			.passive_delay = 1000,
			.hotspot_offset = 10000,
		},
		[THERM_PLL] = {
			.zone_enable = true,
			.passive_delay = 1000,
			.num_trips = 3,
			.trips = {
				{
					.cdev_type = "tegra-shutdown",
					.trip_temp = 97000,
					.trip_type = THERMAL_TRIP_CRITICAL,
					.upper = THERMAL_NO_LIMIT,
					.lower = THERMAL_NO_LIMIT,
				},
				{
					.cdev_type = "tegra-heavy",
					.trip_temp = 94000,
					.trip_type = THERMAL_TRIP_HOT,
					.upper = THERMAL_NO_LIMIT,
					.lower = THERMAL_NO_LIMIT,
				},
				{
					.cdev_type = "cpu-balanced",
					.trip_temp = 84000,
					.trip_type = THERMAL_TRIP_PASSIVE,
					.upper = THERMAL_NO_LIMIT,
					.lower = THERMAL_NO_LIMIT,
				},
			},
			.tzp = &soctherm_tzp,
		},
	},
};

/* Only the diffs from t210ref_soctherm_data structure */
static struct soctherm_platform_data t132ref_v2_soctherm_data = {
	.therm = {
		[THERM_CPU] = {
			.zone_enable = true,
			.passive_delay = 1000,
			.hotspot_offset = 10000,
			.num_trips = 3,
			.trips = {
				{
					.cdev_type = "tegra-shutdown",
					.trip_temp = 105000,
					.trip_type = THERMAL_TRIP_CRITICAL,
					.upper = THERMAL_NO_LIMIT,
					.lower = THERMAL_NO_LIMIT,
				},
				{
					.cdev_type = "tegra-heavy",
					.trip_temp = 102000,
					.trip_type = THERMAL_TRIP_HOT,
					.upper = THERMAL_NO_LIMIT,
					.lower = THERMAL_NO_LIMIT,
				},
				{
					.cdev_type = "cpu-balanced",
					.trip_temp = 92000,
					.trip_type = THERMAL_TRIP_PASSIVE,
					.upper = THERMAL_NO_LIMIT,
					.lower = THERMAL_NO_LIMIT,
				},
			},
			.tzp = &soctherm_tzp,
		},
		[THERM_GPU] = {
			.zone_enable = true,
			.passive_delay = 1000,
			.hotspot_offset = 5000,
			.num_trips = 3,
			.trips = {
				{
					.cdev_type = "tegra-shutdown",
					.trip_temp = 101000,
					.trip_type = THERMAL_TRIP_CRITICAL,
					.upper = THERMAL_NO_LIMIT,
					.lower = THERMAL_NO_LIMIT,
				},
				{
					.cdev_type = "tegra-heavy",
					.trip_temp = 99000,
					.trip_type = THERMAL_TRIP_HOT,
					.upper = THERMAL_NO_LIMIT,
					.lower = THERMAL_NO_LIMIT,
				},
				{
					.cdev_type = "gpu-balanced",
					.trip_temp = 89000,
					.trip_type = THERMAL_TRIP_PASSIVE,
					.upper = THERMAL_NO_LIMIT,
					.lower = THERMAL_NO_LIMIT,
				},
			},
			.tzp = &soctherm_tzp,
		},
	},
};

static struct soctherm_throttle battery_oc_throttle_t13x = {
	.throt_mode = BRIEF,
	.polarity = SOCTHERM_ACTIVE_LOW,
	.priority = 50,
	.intr = true,
	.alarm_cnt_threshold = 15,
	.alarm_filter = 5100000,
	.devs = {
		[THROTTLE_DEV_CPU] = {
			.enable = true,
			.depth = 50,
			.throttling_depth = "low_throttling",
		},
		[THROTTLE_DEV_GPU] = {
			.enable = true,
			.throttling_depth = "medium_throttling",
		},
	},
};

int __init t210ref_soctherm_init(void)
{
	const int t13x_cpu_edp_temp_margin = 5000,
		t13x_gpu_edp_temp_margin = 6000;
	int cpu_edp_temp_margin, gpu_edp_temp_margin;
	int cp_rev, ft_rev;
	struct board_info board_info;
	enum soctherm_therm_id therm_cpu = THERM_CPU;

	tegra_get_board_info(&board_info);
	tegra_chip_id = tegra_get_chip_id();

	cp_rev = tegra_fuse_calib_base_get_cp(NULL, NULL);
	ft_rev = tegra_fuse_calib_base_get_ft(NULL, NULL);

	cpu_edp_temp_margin = t13x_cpu_edp_temp_margin;
	gpu_edp_temp_margin = t13x_gpu_edp_temp_margin;
	if (!cp_rev) {
		/* ATE rev is NEW: use v2 table */
		t210ref_soctherm_data.therm[THERM_CPU] =
			t132ref_v2_soctherm_data.therm[THERM_CPU];
		t210ref_soctherm_data.therm[THERM_GPU] =
			t132ref_v2_soctherm_data.therm[THERM_GPU];
	} else {
		/* ATE rev is Old or Mid: use PLLx sensor only */
		t210ref_soctherm_data.therm[THERM_CPU] =
			t132ref_v1_soctherm_data.therm[THERM_CPU];
		t210ref_soctherm_data.therm[THERM_PLL] =
			t132ref_v1_soctherm_data.therm[THERM_PLL];
		therm_cpu = THERM_PLL; /* override CPU with PLL zone */
	}

	/* do this only for supported CP,FT fuses */
	if ((cp_rev >= 0) && (ft_rev >= 0)) {
		tegra_platform_edp_init(
			t210ref_soctherm_data.therm[therm_cpu].trips,
			&t210ref_soctherm_data.therm[therm_cpu].num_trips,
			cpu_edp_temp_margin);
		tegra_platform_gpu_edp_init(
			t210ref_soctherm_data.therm[THERM_GPU].trips,
			&t210ref_soctherm_data.therm[THERM_GPU].num_trips,
			gpu_edp_temp_margin);
		tegra_add_cpu_vmax_trips(
			t210ref_soctherm_data.therm[therm_cpu].trips,
			&t210ref_soctherm_data.therm[therm_cpu].num_trips);
		tegra_add_tgpu_trips(
			t210ref_soctherm_data.therm[THERM_GPU].trips,
			&t210ref_soctherm_data.therm[THERM_GPU].num_trips);
		tegra_add_vc_trips(
			t210ref_soctherm_data.therm[therm_cpu].trips,
			&t210ref_soctherm_data.therm[therm_cpu].num_trips);
		tegra_add_core_vmax_trips(
			t210ref_soctherm_data.therm[THERM_PLL].trips,
			&t210ref_soctherm_data.therm[THERM_PLL].num_trips);
	}

	tegra_add_cpu_vmin_trips(
		t210ref_soctherm_data.therm[therm_cpu].trips,
		&t210ref_soctherm_data.therm[therm_cpu].num_trips);
	tegra_add_gpu_vmin_trips(
		t210ref_soctherm_data.therm[THERM_GPU].trips,
		&t210ref_soctherm_data.therm[THERM_GPU].num_trips);
	tegra_add_core_vmin_trips(
		t210ref_soctherm_data.therm[THERM_PLL].trips,
		&t210ref_soctherm_data.therm[THERM_PLL].num_trips);

	t210ref_soctherm_data.tshut_pmu_trip_data = &tpdata_max77620;
	/* Enable soc_therm OC throttling on selected platforms */

	memcpy(&t210ref_soctherm_data.throttle[THROTTLE_OC4],
	       &battery_oc_throttle_t13x,
	       sizeof(battery_oc_throttle_t13x));

	return tegra_soctherm_init(&t210ref_soctherm_data);
}

static struct pid_thermal_gov_params cpu_pid_params = {
	.max_err_temp = 4000,
	.max_err_gain = 1000,

	.gain_p = 1000,
	.gain_d = 0,

	.up_compensation = 15,
	.down_compensation = 15,
};

static struct thermal_zone_params cpu_tzp = {
	.governor_name = "pid_thermal_gov",
	.governor_params = &cpu_pid_params,
};

static struct thermal_zone_params board_tzp = {
	.governor_name = "pid_thermal_gov"
};

#ifdef CONFIG_TEGRA_SKIN_THROTTLE
static struct thermal_trip_info skin_trips[] = {
	{
		.cdev_type = "skin-balanced",
		.trip_temp = 43000,
		.trip_type = THERMAL_TRIP_PASSIVE,
		.upper = THERMAL_NO_LIMIT,
		.lower = THERMAL_NO_LIMIT,
		.hysteresis = 0,
	}
};

static struct therm_est_subdevice skin_devs[] = {
	{
		.dev_data = "Tdiode_tegra",
		.coeffs = {
			2, 1, 1, 1,
			1, 1, 1, 1,
			1, 1, 1, 0,
			1, 1, 0, 0,
			0, 0, -1, -7
		},
	},
	{
		.dev_data = "Tboard_tegra",
		.coeffs = {
			-11, -7, -5, -3,
			-3, -2, -1, 0,
			0, 0, 1, 1,
			1, 2, 2, 3,
			4, 6, 11, 18
		},
	},
};

static struct therm_est_subdevice tn8ffd_skin_devs[] = {
	{
		.dev_data = "Tdiode",
		.coeffs = {
			3, 0, 0, 0,
			1, 0, -1, 0,
			1, 0, 0, 1,
			1, 0, 0, 0,
			0, 1, 2, 2
		},
	},
	{
		.dev_data = "Tboard",
		.coeffs = {
			1, 1, 2, 8,
			6, -8, -13, -9,
			-9, -8, -17, -18,
			-18, -16, 2, 17,
			15, 27, 42, 60
		},
	},
};

static struct pid_thermal_gov_params skin_pid_params = {
	.max_err_temp = 4000,
	.max_err_gain = 1000,

	.gain_p = 1000,
	.gain_d = 0,

	.up_compensation = 15,
	.down_compensation = 15,
};

static struct thermal_zone_params skin_tzp = {
	.governor_name = "pid_thermal_gov",
	.governor_params = &skin_pid_params,
};

static struct therm_est_data skin_data = {
	.num_trips = ARRAY_SIZE(skin_trips),
	.trips = skin_trips,
	.polling_period = 1100,
	.passive_delay = 15000,
	.tc1 = 10,
	.tc2 = 1,
	.tzp = &skin_tzp,
	.use_activator = 1,
};

static int __init t210ref_skin_init(void)
{
	struct board_info board_info;

	tegra_get_board_info(&board_info);
	if (!of_machine_is_compatible("nvidia,e2141"))
		return 0;

	if (board_info.board_id == BOARD_P1761 ||
			board_info.board_id == BOARD_E1784 ||
			board_info.board_id == BOARD_E1991 ||
			board_info.board_id == BOARD_E1971 ||
			board_info.board_id == BOARD_E1922) {
		skin_data.ndevs = ARRAY_SIZE(tn8ffd_skin_devs);
		skin_data.devs = tn8ffd_skin_devs;
		skin_data.toffset = 4034;
	} else {
		skin_data.ndevs = ARRAY_SIZE(skin_devs);
		skin_data.devs = skin_devs;
		skin_data.toffset = 9793;
	}

	tegra_skin_therm_est_device.dev.platform_data = &skin_data;
	platform_device_register(&tegra_skin_therm_est_device);

	return 0;
}
late_initcall(t210ref_skin_init);
#endif

static struct nct1008_platform_data t210ref_nct72_pdata = {
	.loc_name = "tegra",
	.supported_hwrev = true,
	.conv_rate = 0x06, /* 4Hz conversion rate */
	.offset = 0,
	.extended_range = true,

	.sensors = {
		[LOC] = {
			.tzp = &board_tzp,
			.shutdown_limit = 120, /* C */
			.passive_delay = 1000,
			.num_trips = 1,
			.trips = {
				{
					.cdev_type = "therm_est_activ",
					.trip_temp = 40000,
					.trip_type = THERMAL_TRIP_ACTIVE,
					.hysteresis = 1000,
					.upper = THERMAL_NO_LIMIT,
					.lower = THERMAL_NO_LIMIT,
					.mask = 1,
				},
			},
		},
		[EXT] = {
			.tzp = &cpu_tzp,
			.shutdown_limit = 95, /* C */
			.passive_delay = 1000,
			.num_trips = 2,
			.trips = {
				{
					.cdev_type = "shutdown_warning",
					.trip_temp = 93000,
					.trip_type = THERMAL_TRIP_PASSIVE,
					.upper = THERMAL_NO_LIMIT,
					.lower = THERMAL_NO_LIMIT,
					.mask = 0,
				},
				{
					.cdev_type = "cpu-balanced",
					.trip_temp = 83000,
					.trip_type = THERMAL_TRIP_PASSIVE,
					.upper = THERMAL_NO_LIMIT,
					.lower = THERMAL_NO_LIMIT,
					.hysteresis = 1000,
					.mask = 1,
				},
			}
		}
	}
};

#ifdef CONFIG_TEGRA_SKIN_THROTTLE
static struct nct1008_platform_data t210ref_nct72_tskin_pdata = {
	.loc_name = "skin",

	.supported_hwrev = true,
	.conv_rate = 0x06, /* 4Hz conversion rate */
	.offset = 0,
	.extended_range = true,

	.sensors = {
		[LOC] = {
			.shutdown_limit = 95, /* C */
			.num_trips = 0,
			.tzp = NULL,
		},
		[EXT] = {
			.shutdown_limit = 85, /* C */
			.passive_delay = 10000,
			.polling_delay = 1000,
			.tzp = &skin_tzp,
			.num_trips = 1,
			.trips = {
				{
					.cdev_type = "skin-balanced",
					.trip_temp = 50000,
					.trip_type = THERMAL_TRIP_PASSIVE,
					.upper = THERMAL_NO_LIMIT,
					.lower = THERMAL_NO_LIMIT,
					.mask = 1,
				},
			},
		}
	}
};
#endif

static struct i2c_board_info t210ref_i2c_nct72_board_info[] = {
	{
		I2C_BOARD_INFO("nct72", 0x4c),
		.platform_data = &t210ref_nct72_pdata,
		.irq = -1,
	},
};

#ifdef CONFIG_TEGRA_SKIN_THROTTLE
static struct i2c_board_info t210ref_skin_i2c_nct72_board_info[] = {
	{
		I2C_BOARD_INFO("nct72", 0x4d),
		.platform_data = &t210ref_nct72_tskin_pdata,
		.irq = -1,
	}
};
#endif

int t210ref_thermal_sensors_init(void)
{
	int nct72_port = TEGRA_GPIO_PI6;
	int ret = 0;
	int i;
	struct thermal_trip_info *trip_state;
	struct board_info board_info;

	tegra_get_board_info(&board_info);
	/* raise NCT's thresholds if soctherm CP,FT fuses are ok */
	if ((tegra_fuse_calib_base_get_cp(NULL, NULL) >= 0) &&
	    (tegra_fuse_calib_base_get_ft(NULL, NULL) >= 0)) {
		t210ref_nct72_pdata.sensors[EXT].shutdown_limit += 20;
		for (i = 0; i < t210ref_nct72_pdata.sensors[EXT].num_trips;
			 i++) {
			trip_state = &t210ref_nct72_pdata.sensors[EXT].trips[i];
			if (!strncmp(trip_state->cdev_type, "cpu-balanced",
					THERMAL_NAME_LENGTH)) {
				trip_state->cdev_type = "_none_";
				break;
			}
		}
	} else {
		tegra_platform_edp_init(
			t210ref_nct72_pdata.sensors[EXT].trips,
			&t210ref_nct72_pdata.sensors[EXT].num_trips,
					12000); /* edp temperature margin */
		tegra_add_cpu_vmax_trips(
			t210ref_nct72_pdata.sensors[EXT].trips,
			&t210ref_nct72_pdata.sensors[EXT].num_trips);
		tegra_add_tgpu_trips(
			t210ref_nct72_pdata.sensors[EXT].trips,
			&t210ref_nct72_pdata.sensors[EXT].num_trips);
		tegra_add_vc_trips(
			t210ref_nct72_pdata.sensors[EXT].trips,
			&t210ref_nct72_pdata.sensors[EXT].num_trips);
		tegra_add_core_vmax_trips(
			t210ref_nct72_pdata.sensors[EXT].trips,
			&t210ref_nct72_pdata.sensors[EXT].num_trips);
	}

	/* vmin trips are bound to soctherm on norrin and bowmore */
	if (!(board_info.board_id == BOARD_PM374 ||
		board_info.board_id == BOARD_E2141 ||
		board_info.board_id == BOARD_E1971 ||
		board_info.board_id == BOARD_E1991))
		tegra_add_all_vmin_trips(t210ref_nct72_pdata.sensors[EXT].trips,
			&t210ref_nct72_pdata.sensors[EXT].num_trips);

	/* T210_interposer use GPIO_PC7 for alert*/
	if (board_info.board_id == BOARD_E2141)
		nct72_port = TEGRA_GPIO_PC7;

	t210ref_i2c_nct72_board_info[0].irq = gpio_to_irq(nct72_port);

	ret = gpio_request(nct72_port, "temp_alert");
	if (ret < 0)
		return ret;

	ret = gpio_direction_input(nct72_port);
	if (ret < 0) {
		pr_info("%s: calling gpio_free(nct72_port)", __func__);
		gpio_free(nct72_port);
	}

	/* norrin has thermal sensor on GEN1-I2C i.e. instance 0 */
	if (board_info.board_id == BOARD_PM374)
		i2c_register_board_info(0, t210ref_i2c_nct72_board_info,
					1); /* only register device[0] */
	/* t210ref has thermal sensor on GEN2-I2C i.e. instance 1 */
	else if (board_info.board_id == BOARD_PM358 ||
			board_info.board_id == BOARD_PM359 ||
			board_info.board_id == BOARD_PM370 ||
			board_info.board_id == BOARD_PM363)
		i2c_register_board_info(1, t210ref_i2c_nct72_board_info,
		ARRAY_SIZE(t210ref_i2c_nct72_board_info));
	else if (board_info.board_id == BOARD_PM375) {
		t210ref_nct72_pdata.sensors[EXT].shutdown_limit = 100;
		t210ref_nct72_pdata.sensors[LOC].shutdown_limit = 95;
		i2c_register_board_info(0, t210ref_i2c_nct72_board_info,
					1); /* only register device[0] */
	} else {
	/* E2141 has thermal sensor on GEN1-I2C, skin temp sensor on GEN2-I2C */
		i2c_register_board_info(0, t210ref_i2c_nct72_board_info,
			ARRAY_SIZE(t210ref_i2c_nct72_board_info));
#ifdef CONFIG_TEGRA_SKIN_THROTTLE
		i2c_register_board_info(1, t210ref_skin_i2c_nct72_board_info,
			ARRAY_SIZE(t210ref_skin_i2c_nct72_board_info));
#endif
	}

	return ret;
}