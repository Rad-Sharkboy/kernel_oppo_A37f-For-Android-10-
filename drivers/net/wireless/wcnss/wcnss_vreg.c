/* Copyright (c) 2011-2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/rpm-smd-regulator.h>
#include <linux/mfd/pm8xxx/pm8921.h>
#include <linux/mfd/pm8xxx/gpio.h>
#include <linux/wcnss_wlan.h>
#include <linux/semaphore.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/leds.h>


static LIST_HEAD(power_on_lock_list);
static DEFINE_MUTEX(list_lock);
static DEFINE_SEMAPHORE(wcnss_power_on_lock);
static struct clk *proxy_rf_clk;
static bool is_power_on;
static int proxy_xo_mode = WCNSS_XO_INVALID;
DEFINE_LED_TRIGGER(wlan_indication_led);

#define RIVA_PMU_OFFSET         0x28

#define RIVA_SPARE_OFFSET       0x0b4
#define PRONTO_SPARE_OFFSET     0x1088
#define NVBIN_DLND_BIT          BIT(25)

#define PRONTO_IRIS_REG_READ_OFFSET       0x1134
#define PRONTO_IRIS_REG_CHIP_ID           0x04
/* IRIS card chip ID's */
#define WCN3660       0x0200
#define WCN3660A      0x0300
#define WCN3660B      0x0400
#define WCN3620       0x5111
#define WCN3620A      0x5112
#define WCN3610       0x9101
#define WCN3610V1     0x9110

#define WCNSS_PMU_CFG_IRIS_XO_CFG          BIT(3)
#define WCNSS_PMU_CFG_IRIS_XO_EN           BIT(4)
#define WCNSS_PMU_CFG_IRIS_XO_CFG_STS      BIT(6) /* 1: in progress, 0: done */

#define WCNSS_PMU_CFG_IRIS_RESET           BIT(7)
#define WCNSS_PMU_CFG_IRIS_RESET_STS       BIT(8) /* 1: in progress, 0: done */
#define WCNSS_PMU_CFG_IRIS_XO_READ         BIT(9)
#define WCNSS_PMU_CFG_IRIS_XO_READ_STS     BIT(10)

#define WCNSS_PMU_CFG_IRIS_XO_MODE         0x6
#define WCNSS_PMU_CFG_IRIS_XO_MODE_48      (3 << 1)

#define VREG_NULL_CONFIG            0x0000
#define VREG_GET_REGULATOR_MASK     0x0001
#define VREG_SET_VOLTAGE_MASK       0x0002
#define VREG_OPTIMUM_MODE_MASK      0x0004
#define VREG_ENABLE_MASK            0x0008
#define VDD_PA                      "qcom,iris-vddpa"

#define WCNSS_INVALID_IRIS_REG      0xbaadbaad

struct vregs_info {
	const char * const name;
	int state;
	int nominal_min;
	int low_power_min;
	int max_voltage;
	const int uA_load;
	struct regulator *regulator;
};

/* IRIS regulators for Riva hardware */
static struct vregs_info iris_vregs_riva[] = {
	{"iris_vddxo",  VREG_NULL_CONFIG, 1800000, 0, 1800000, 10000,  NULL},
	{"iris_vddrfa", VREG_NULL_CONFIG, 1300000, 0, 1300000, 100000, NULL},
	{"iris_vddpa",  VREG_NULL_CONFIG, 2900000, 0, 3000000, 515000, NULL},
	{"iris_vdddig", VREG_NULL_CONFIG, 1200000, 0, 1225000, 10000,  NULL},
};

/* WCNSS regulators for Riva hardware */
static struct vregs_info riva_vregs[] = {
	/* Riva */
	{"riva_vddmx",  VREG_NULL_CONFIG, 1050000, 0, 1150000, 0,      NULL},
	{"riva_vddcx",  VREG_NULL_CONFIG, 1050000, 0, 1150000, 0,      NULL},
	{"riva_vddpx",  VREG_NULL_CONFIG, 1800000, 0, 1800000, 0,      NULL},
};

/* IRIS regulators for Pronto hardware */
static struct vregs_info iris_vregs_pronto[] = {
	{"qcom,iris-vddxo",  VREG_NULL_CONFIG, 1800000, 0,
		1800000, 10000,  NULL},
	{"qcom,iris-vddrfa", VREG_NULL_CONFIG, 1300000, 0,
		1300000, 100000, NULL},
	{"qcom,iris-vddpa",  VREG_NULL_CONFIG, 2900000, 0,
		3350000, 515000, NULL},
	{"qcom,iris-vdddig", VREG_NULL_CONFIG, 1225000, 0,
		1800000, 10000,  NULL},
};

/* WCNSS regulators for Pronto hardware */
static struct vregs_info pronto_vregs[] = {
	{"qcom,pronto-vddmx",  VREG_NULL_CONFIG, 950000,  0,
		1150000, 0,    NULL},
	{"qcom,pronto-vddcx",  VREG_NULL_CONFIG, RPM_REGULATOR_CORNER_NORMAL,
		RPM_REGULATOR_CORNER_NONE, RPM_REGULATOR_CORNER_SUPER_TURBO,
		0,             NULL},
	{"qcom,pronto-vddpx",  VREG_NULL_CONFIG, 1800000, 0,
		1800000, 0,    NULL},
};

/* IRIS regulators for Pronto v2 hardware */
static struct vregs_info iris_vregs_pronto_v2[] = {
	{"qcom,iris-vddxo",  VREG_NULL_CONFIG, 1800000, 0,
		1800000, 10000,  NULL},
	{"qcom,iris-vddrfa", VREG_NULL_CONFIG, 1300000, 0,
		1300000, 100000, NULL},
	{"qcom,iris-vddpa",  VREG_NULL_CONFIG, 3300000, 0,
		3300000, 515000, NULL},
	{"qcom,iris-vdddig", VREG_NULL_CONFIG, 1800000, 0,
		1800000, 10000,  NULL},
};

/* WCNSS regulators for Pronto v2 hardware */
static struct vregs_info pronto_vregs_pronto_v2[] = {
	{"qcom,pronto-vddmx",  VREG_NULL_CONFIG,
		RPM_REGULATOR_CORNER_SUPER_TURBO,  0,
		RPM_REGULATOR_CORNER_SUPER_TURBO, 0,    NULL},
	{"qcom,pronto-vddcx",  VREG_NULL_CONFIG, RPM_REGULATOR_CORNER_NORMAL,
		RPM_REGULATOR_CORNER_NONE, RPM_REGULATOR_CORNER_SUPER_TURBO,
		0,             NULL},
	{"qcom,pronto-vddpx",  VREG_NULL_CONFIG, 1800000, 0,
		1800000, 0,    NULL},
};

/* WCNSS regulators for Pronto v3 hardware */
static struct vregs_info pronto_vregs_pronto_v3[] = {
	{"qcom,pronto-vddmx",  VREG_NULL_CONFIG, RPM_REGULATOR_CORNER_NORMAL,
		RPM_REGULATOR_CORNER_NONE, RPM_REGULATOR_CORNER_SUPER_TURBO,
		0,             NULL},
	{"qcom,pronto-vddcx",  VREG_NULL_CONFIG, RPM_REGULATOR_CORNER_NORMAL,
		RPM_REGULATOR_CORNER_NONE, RPM_REGULATOR_CORNER_SUPER_TURBO,
		0,             NULL},
	{"qcom,pronto-vddpx",  VREG_NULL_CONFIG, 1800000, 0,
		1800000, 0,    NULL},
};


struct host_driver {
	char name[20];
	struct list_head list;
};

enum {
	IRIS_3660, /* also 3660A and 3680 */
	IRIS_3620,
	IRIS_3610
};


int xo_auto_detect(u32 reg)
{
	reg >>= 30;

	switch (reg) {
	case IRIS_3660:
		return WCNSS_XO_48MHZ;

	case IRIS_3620:
		return WCNSS_XO_19MHZ;

	case IRIS_3610:
		return WCNSS_XO_19MHZ;

	default:
		return WCNSS_XO_INVALID;
	}
}

int wcnss_get_iris_name(char *iris_name)
{
	struct wcnss_wlan_config *cfg = NULL;
	int iris_id;

	cfg = wcnss_get_wlan_config();

	if (cfg) {
		iris_id = cfg->iris_id;
		iris_id = iris_id >> 16;
	} else {
		return 1;
	}

	switch (iris_id) {
	case WCN3660:
		memcpy(iris_name, "WCN3660", sizeof("WCN3660"));
		break;
	case WCN3660A:
		memcpy(iris_name, "WCN3660A", sizeof("WCN3660A"));
		break;
	case WCN3660B:
		memcpy(iris_name, "WCN3660B", sizeof("WCN3660B"));
		break;
	case WCN3620:
		memcpy(iris_name, "WCN3620", sizeof("WCN3620"));
		break;
	case WCN3620A:
		memcpy(iris_name, "WCN3620A", sizeof("WCN3620A"));
		break;
	case WCN3610:
		memcpy(iris_name, "WCN3610", sizeof("WCN3610"));
		break;
	case WCN3610V1:
		memcpy(iris_name, "WCN3610V1", sizeof("WCN3610V1"));
		break;
	default:
		return 1;
	}

	return 0;
}
EXPORT_SYMBOL(wcnss_get_iris_name);

int validate_iris_chip_id(u32 reg)
{
	int iris_id;
	iris_id = reg >> 16;

	switch (iris_id) {
	case WCN3660:
	case WCN3660A:
	case WCN3660B:
	case WCN3620:
	case WCN3620A:
	case WCN3610:
	case WCN3610V1:
		return 0;
	default:
		return 1;
	}
}

/* All IRIS setup runs in sleepable proxy-notifier context. */
static int iris_wait_clear(void __iomem *pmu, u32 mask)
{
	u32 status;

	return readl_poll_timeout(pmu, status, !(status & mask), 50, 100000);
}

static int wcnss_iris_reset(u32 reg, void __iomem *pmu)
{
	int rc;

	writel_relaxed(reg | WCNSS_PMU_CFG_IRIS_RESET, pmu);
	rc = iris_wait_clear(pmu, WCNSS_PMU_CFG_IRIS_RESET_STS);
	writel_relaxed(reg & ~WCNSS_PMU_CFG_IRIS_RESET, pmu);
	return rc;
}

static int configure_iris_xo(struct device *dev, struct wcnss_wlan_config *cfg,
			     int *iris_xo_set)
{
	void __iomem *base = cfg->msm_wcnss_base;
	void __iomem *pmu_conf_reg, *spare_reg, *iris_read_reg;
	u32 reg, i = 0, iris_reg = WCNSS_INVALID_IRIS_REG;
	bool pronto = wcnss_hardware_type() == WCNSS_PRONTO_HW;
	bool detect = wcnss_xo_auto_detect_enabled();
	int detected = WCNSS_XO_INVALID;
	int mode, rc;
	struct clk *clk, *rf;

	if (!base)
		return -ENODEV;
	clk = clk_get(dev, pronto ? "xo" : "cxo");
	if (IS_ERR(clk))
		return PTR_ERR(clk);
	/* Enable IRIS XO */
	rc = clk_prepare_enable(clk);
	if (rc) {
		pr_err("clk enable failed\n");
		goto put_clk;
	}

	/* NV bit is set to indicate that platform driver is capable
	 * of doing NV download.
	 */
	pr_debug("wcnss: Indicate NV bin download\n");
	spare_reg = base + (pronto ? PRONTO_SPARE_OFFSET : RIVA_SPARE_OFFSET);
	reg = readl_relaxed(spare_reg);
	reg |= NVBIN_DLND_BIT;
	writel_relaxed(reg, spare_reg);

	pmu_conf_reg = base + (pronto ? PRONTO_PMU_OFFSET : RIVA_PMU_OFFSET);
	writel_relaxed(0, pmu_conf_reg);
	reg = readl_relaxed(pmu_conf_reg);
	reg |= WCNSS_PMU_CFG_GC_BUS_MUX_SEL_TOP |
			WCNSS_PMU_CFG_IRIS_XO_EN;
	writel_relaxed(reg, pmu_conf_reg);

	if (detect) {
		iris_read_reg = base +
			PRONTO_IRIS_REG_READ_OFFSET;
		iris_reg = readl_relaxed(iris_read_reg);
	}

	rc = wcnss_iris_reset(reg, pmu_conf_reg);
	if (rc)
		goto abort_pmu;

	if (iris_reg != WCNSS_INVALID_IRIS_REG) {
		iris_reg &= 0xffff;
		iris_reg |= PRONTO_IRIS_REG_CHIP_ID;
		writel_relaxed(iris_reg, iris_read_reg);
		do {
			/* Iris read */
			reg = readl_relaxed(pmu_conf_reg);
			reg |= WCNSS_PMU_CFG_IRIS_XO_READ;
			writel_relaxed(reg, pmu_conf_reg);

			/* Wait for PMU_CFG.iris_reg_read_sts */
			rc = iris_wait_clear(pmu_conf_reg, WCNSS_PMU_CFG_IRIS_XO_READ_STS);
			if (rc)
				goto abort_pmu;

			iris_reg = readl_relaxed(iris_read_reg);
			pr_info("wcnss: IRIS Reg: %08x\n", iris_reg);

			if (!validate_iris_chip_id(iris_reg))
				break;
			if (i >= 4) {
				pr_info("wcnss: IRIS Card absent/invalid\n");
				detected = WCNSS_XO_INVALID;
				/* Reset iris read bit */
				reg &= ~WCNSS_PMU_CFG_IRIS_XO_READ;
				/* Clear XO_MODE[b2:b1] bits.
				 * Clear implies 19.2 MHz TCXO
				 */
				reg &= ~(WCNSS_PMU_CFG_IRIS_XO_MODE);
				goto xo_configure;
			}
			reg &= ~WCNSS_PMU_CFG_IRIS_XO_READ;
			writel_relaxed(reg, pmu_conf_reg);
			rc = wcnss_iris_reset(reg, pmu_conf_reg);
			if (rc)
				goto abort_pmu;
		} while (i++ < 5);
		detected = xo_auto_detect(iris_reg);

		/* Reset iris read bit */
		reg &= ~WCNSS_PMU_CFG_IRIS_XO_READ;

	} else if (detect)
		/* Default to 48 MHZ */
		detected = WCNSS_XO_48MHZ;
	else
		detected = WCNSS_XO_INVALID;

	cfg->iris_id = iris_reg;

	/* Clear XO_MODE[b2:b1] bits. Clear implies 19.2 MHz TCXO */
	reg &= ~(WCNSS_PMU_CFG_IRIS_XO_MODE);

	if ((cfg->use_48mhz_xo && detected == WCNSS_XO_INVALID) ||
	    detected == WCNSS_XO_48MHZ)
		reg |= WCNSS_PMU_CFG_IRIS_XO_MODE_48;

xo_configure:
	mode = (reg & WCNSS_PMU_CFG_IRIS_XO_MODE) == WCNSS_PMU_CFG_IRIS_XO_MODE_48 ?
		WCNSS_XO_48MHZ : WCNSS_XO_19MHZ;
	writel_relaxed(reg, pmu_conf_reg);

	rc = wcnss_iris_reset(reg, pmu_conf_reg);
	if (rc)
		goto abort_pmu;

	/* Start IRIS XO configuration */
	reg |= WCNSS_PMU_CFG_IRIS_XO_CFG;
	writel_relaxed(reg, pmu_conf_reg);

	/* Wait for XO configuration to finish */
	rc = iris_wait_clear(pmu_conf_reg, WCNSS_PMU_CFG_IRIS_XO_CFG_STS);
	if (rc)
		goto abort_pmu;

	/* Stop IRIS XO configuration */
	reg &= ~(WCNSS_PMU_CFG_GC_BUS_MUX_SEL_TOP |
			WCNSS_PMU_CFG_IRIS_XO_CFG);
	writel_relaxed(reg, pmu_conf_reg);
	clk_disable_unprepare(clk);

	if (mode == WCNSS_XO_19MHZ) {
		rf = clk_get(dev, "rf_clk");
		if (IS_ERR(rf)) {
			rc = PTR_ERR(rf);
			goto put_clk;
		}
		rc = clk_prepare_enable(rf);
		if (rc) {
			clk_put(rf);
			goto put_clk;
		}
		proxy_rf_clk = rf;
	}
	*iris_xo_set = mode;
	msleep(20);
	goto put_clk;

abort_pmu:
	pr_err_ratelimited("wcnss: IRIS handshake timed out\n");
	reg &= ~(WCNSS_PMU_CFG_GC_BUS_MUX_SEL_TOP | WCNSS_PMU_CFG_IRIS_RESET |
		 WCNSS_PMU_CFG_IRIS_XO_READ | WCNSS_PMU_CFG_IRIS_XO_CFG |
		 WCNSS_PMU_CFG_IRIS_XO_EN);
	writel_relaxed(reg, pmu_conf_reg);
	clk_disable_unprepare(clk);
put_clk:
	clk_put(clk);
	return rc;
}

static void release_iris_xo(void)
{
	if (proxy_rf_clk) {
		clk_disable_unprepare(proxy_rf_clk);
		clk_put(proxy_rf_clk);
		proxy_rf_clk = NULL;
	}
	msleep(20);
}

static int wcnss_vregs_off(struct vregs_info *regs, unsigned int count,
			   const struct wcnss_wlan_config *cfg)
{
	while (count) {
		struct vregs_info *v = &regs[--count];
		int rc, max_voltage = v->max_voltage;

		if (!(v->state & VREG_GET_REGULATOR_MASK))
			continue;
		if (v->state & VREG_ENABLE_MASK) {
			rc = regulator_disable(v->regulator);
			if (rc)
				goto failed;
			v->state &= ~VREG_ENABLE_MASK;
		}
		if (v->state & VREG_OPTIMUM_MODE_MASK) {
			rc = regulator_set_optimum_mode(v->regulator, 0);
			if (rc < 0)
				goto failed;
			v->state &= ~VREG_OPTIMUM_MODE_MASK;
		}
		if (v->state & VREG_SET_VOLTAGE_MASK) {
			if (cfg->vbatt < WCNSS_VBATT_THRESHOLD && !strcmp(v->name, VDD_PA))
				max_voltage = WCNSS_VBATT_LOW;
			rc = regulator_set_voltage(v->regulator, v->low_power_min, max_voltage);
			if (rc)
				goto failed;
			v->state &= ~VREG_SET_VOLTAGE_MASK;
		}
		regulator_put(v->regulator);
		v->state = VREG_NULL_CONFIG;
		v->regulator = NULL;
		continue;
failed:
		pr_err_ratelimited("wcnss: failed to release %s: %d\n", v->name, rc);
		return rc;
	}
	return 0;
}

/* Common helper routine to turn on all WCNSS & IRIS vregs */
static int wcnss_vregs_on(struct device *dev,
		struct vregs_info regulators[], uint size,
			  const struct wcnss_wlan_config *cfg)
{
	int i, rc = 0, reg_cnt;

	for (i = 0; i < size; i++) {
		struct regulator *reg = regulator_get(dev, regulators[i].name);

		if (IS_ERR(reg)) {
			rc = PTR_ERR(reg);
			pr_err("regulator get of %s failed (%d)\n", regulators[i].name, rc);
			return rc;
		}
		regulators[i].regulator = reg;
		regulators[i].state |= VREG_GET_REGULATOR_MASK;
		reg_cnt = regulator_count_voltages(regulators[i].regulator);
		/* Set voltage to nominal. Exclude swtiches e.g. LVS */
		if ((regulators[i].nominal_min || regulators[i].max_voltage)
				&& (reg_cnt > 0)) {

			int nominal_min = regulators[i].nominal_min;
			int max_voltage = regulators[i].max_voltage;

			if (cfg->vbatt < WCNSS_VBATT_THRESHOLD &&
			    !strcmp(regulators[i].name, VDD_PA)) {
				nominal_min = WCNSS_VBATT_INITIAL;
				max_voltage = WCNSS_VBATT_LOW;
			}

			rc = regulator_set_voltage(regulators[i].regulator,
					nominal_min, max_voltage);
			if (rc) {
				pr_err("regulator_set_voltage(%s) failed (%d)\n",
						regulators[i].name, rc);
				return rc;
			}
			regulators[i].state |= VREG_SET_VOLTAGE_MASK;
		}

		/* Vote for PWM/PFM mode if needed */
		if (regulators[i].uA_load && (reg_cnt > 0)) {
			rc = regulator_set_optimum_mode(regulators[i].regulator,
					regulators[i].uA_load);
			if (rc < 0) {
				pr_err("regulator_set_optimum_mode(%s) failed (%d)\n",
						regulators[i].name, rc);
				return rc;
			}
			regulators[i].state |= VREG_OPTIMUM_MODE_MASK;
		}

		/* Enable the regulator */
		rc = regulator_enable(regulators[i].regulator);
		if (rc) {
			pr_err("vreg %s enable failed (%d)\n",
				regulators[i].name, rc);
			return rc;
		}
		regulators[i].state |= VREG_ENABLE_MASK;
	}

	return rc;

}

struct wcnss_supply_set {
	struct vregs_info *core, *iris;
	unsigned int num_core, num_iris;
};

#define WCNSS_SUPPLIES(c, i) { (c), (i), ARRAY_SIZE(c), ARRAY_SIZE(i) }
static const struct wcnss_supply_set riva = WCNSS_SUPPLIES(riva_vregs, iris_vregs_riva);
static const struct wcnss_supply_set pronto = WCNSS_SUPPLIES(pronto_vregs, iris_vregs_pronto);
static const struct wcnss_supply_set pronto_v2 =
	WCNSS_SUPPLIES(pronto_vregs_pronto_v2, iris_vregs_pronto_v2);
static const struct wcnss_supply_set pronto_v3 =
	WCNSS_SUPPLIES(pronto_vregs_pronto_v3, iris_vregs_pronto_v2);
static const struct wcnss_supply_set *proxy_supplies;

static const struct wcnss_supply_set *wcnss_select_supplies(
	enum wcnss_hw_type hw, const struct wcnss_wlan_config *cfg)
{
	if (hw == WCNSS_RIVA_HW)
		return &riva;
	if (hw != WCNSS_PRONTO_HW)
		return NULL;
	if (cfg->is_pronto_vt)
		return &pronto_v2;
	return cfg->is_pronto_v3 ? &pronto_v3 : &pronto;
}

static int wcnss_release_supplies(const struct wcnss_wlan_config *cfg)
{
	const struct wcnss_supply_set *supplies = proxy_supplies;
	int attempt, rc;

	for (attempt = 0; attempt < 2; attempt++) {
		rc = wcnss_vregs_off(supplies->iris, supplies->num_iris, cfg);
		if (!rc)
			rc = wcnss_vregs_off(supplies->core, supplies->num_core, cfg);
		if (!rc) {
			proxy_supplies = NULL;
			break;
		}
		if (!attempt)
			usleep_range(1000, 2000);
	}
	return rc;
}

int wcnss_wlan_power(struct device *dev, struct wcnss_wlan_config *cfg,
		     enum wcnss_opcode on, int *iris_xo_set)
{
	const struct wcnss_supply_set *supplies;
	int rc = 0;

	if (!dev || !cfg)
		return -EINVAL;
	down(&wcnss_power_on_lock);
	if (on && is_power_on)
		goto report_mode;

	if (is_power_on) {
		release_iris_xo();
		is_power_on = false;
		proxy_xo_mode = WCNSS_XO_INVALID;
	}
	if (proxy_supplies) {
		rc = wcnss_release_supplies(cfg);
		if (rc)
			goto out;
	}
	if (!on)
		goto out;

	supplies = wcnss_select_supplies(wcnss_hardware_type(), cfg);
	if (!supplies) {
		rc = -EINVAL;
		goto out;
	}
	proxy_supplies = supplies;
	rc = wcnss_vregs_on(dev, supplies->core, supplies->num_core, cfg);
	if (!rc)
		rc = wcnss_vregs_on(dev, supplies->iris, supplies->num_iris, cfg);
	if (!rc)
		rc = configure_iris_xo(dev, cfg, &proxy_xo_mode);
	if (rc) {
		wcnss_release_supplies(cfg);
		goto out;
	}
	is_power_on = true;
report_mode:
	if (iris_xo_set)
		*iris_xo_set = proxy_xo_mode;
out:
	up(&wcnss_power_on_lock);
	return rc;
}
EXPORT_SYMBOL(wcnss_wlan_power);

/*
 * During SSR WCNSS should not be 'powered on' until all the host drivers
 * finish their shutdown routines.  Host drivers use below APIs to
 * synchronize power-on. WCNSS will not be 'powered on' until all the
 * requests(to lock power-on) are freed.
 */
int wcnss_req_power_on_lock(char *driver_name)
{
	struct host_driver *node;

	if (!driver_name)
		return -EINVAL;

	node = kmalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return -ENOMEM;
	strlcpy(node->name, driver_name, sizeof(node->name));

	mutex_lock(&list_lock);
	/* Lock when the first request is added */
	if (list_empty(&power_on_lock_list))
		down(&wcnss_power_on_lock);
	list_add(&node->list, &power_on_lock_list);
	if (wlan_indication_led)
		led_trigger_event(wlan_indication_led, LED_FULL);
	mutex_unlock(&list_lock);

	return 0;
}
EXPORT_SYMBOL(wcnss_req_power_on_lock);

int wcnss_free_power_on_lock(char *driver_name)
{
	struct host_driver *node;
	bool released = false;

	if (!driver_name)
		return -EINVAL;

	mutex_lock(&list_lock);
	list_for_each_entry(node, &power_on_lock_list, list) {
		if (strncmp(node->name, driver_name, sizeof(node->name)))
			continue;

		list_del(&node->list);
		kfree(node);
		released = true;
		if (list_empty(&power_on_lock_list)) {
			up(&wcnss_power_on_lock);
			if (wlan_indication_led)
				led_trigger_event(wlan_indication_led, LED_OFF);
		}
		break;
	}
	mutex_unlock(&list_lock);

	return released ? 0 : -ENOENT;
}
EXPORT_SYMBOL(wcnss_free_power_on_lock);

void wcnss_en_wlan_led_trigger(void)
{
	led_trigger_register_simple("wlan-indication-led",
		&wlan_indication_led);
}
