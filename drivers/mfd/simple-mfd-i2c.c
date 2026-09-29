// SPDX-License-Identifier: GPL-2.0-only
/*
 * Simple MFD - I2C
 *
 * Author(s):
 * 	Michael Walle <michael@walle.cc>
 * 	Lee Jones <lee.jones@linaro.org>
 *
 * This driver creates a single register map with the intention for it to be
 * shared by all sub-devices.  Children can use their parent's device structure
 * (dev.parent) in order to reference it.
 *
 * Once the register map has been successfully initialised, any sub-devices
 * represented by child nodes in Device Tree or via the MFD cells in this file
 * will be subsequently registered.
 */

#include <linux/array_size.h>
#include <linux/bits.h>
#include <linux/dev_printk.h>
#include <linux/interrupt.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/mfd/core.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/stddef.h>

#include "simple-mfd-i2c.h"

static const struct regmap_config regmap_config_8r_8v = {
	.reg_bits = 8,
	.val_bits = 8,
};

static int simple_mfd_i2c_probe(struct i2c_client *i2c)
{
	const struct simple_mfd_data *simple_mfd_data;
	const struct regmap_config *regmap_config;
	struct regmap *regmap;
	int ret;

	simple_mfd_data = device_get_match_data(&i2c->dev);

	/* If no regmap_config is specified, use the default 8reg and 8val bits */
	if (!simple_mfd_data || !simple_mfd_data->regmap_config)
		regmap_config = &regmap_config_8r_8v;
	else
		regmap_config = simple_mfd_data->regmap_config;

	regmap = devm_regmap_init_i2c(i2c, regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	/* If no MFD cells are specified, register using the DT child nodes instead */
	if (!simple_mfd_data || !simple_mfd_data->mfd_cell)
		return devm_of_platform_populate(&i2c->dev);

	ret = devm_mfd_add_devices(&i2c->dev, PLATFORM_DEVID_AUTO,
				   simple_mfd_data->mfd_cell,
				   simple_mfd_data->mfd_cell_size,
				   NULL, 0, NULL);
	if (ret) {
		dev_err(&i2c->dev, "Failed to add child devices\n");
		return ret;
	}

	if (simple_mfd_data->irq_chip && i2c->irq > 0) {
		struct regmap_irq_chip_data *irq_data;

		ret = devm_regmap_add_irq_chip(&i2c->dev, regmap, i2c->irq,
					       IRQF_ONESHOT, 0,
					       simple_mfd_data->irq_chip, &irq_data);
		if (ret) {
			/* IRQ consumers are optional; keep the core cells working. */
			dev_warn(&i2c->dev, "Failed to add IRQ chip: %d\n", ret);
			return 0;
		}

		ret = devm_mfd_add_devices(&i2c->dev, PLATFORM_DEVID_AUTO,
					   simple_mfd_data->irq_mfd_cell,
					   simple_mfd_data->irq_mfd_cell_size,
					   NULL, 0, regmap_irq_get_domain(irq_data));
		if (ret)
			dev_warn(&i2c->dev, "Failed to add IRQ child devices: %d\n", ret);
	}

	return 0;
}

static const struct mfd_cell sy7636a_cells[] = {
	{ .name = "sy7636a-regulator", },
	{ .name = "sy7636a-temperature", },
};

static const struct simple_mfd_data silergy_sy7636a = {
	.mfd_cell = sy7636a_cells,
	.mfd_cell_size = ARRAY_SIZE(sy7636a_cells),
};

static const struct mfd_cell max5970_cells[] = {
	{ .name = "max5970-regulator", },
	{ .name = "max5970-iio", },
	{ .name = "max5970-led", },
};

static const struct simple_mfd_data maxim_max5970 = {
	.mfd_cell = max5970_cells,
	.mfd_cell_size = ARRAY_SIZE(max5970_cells),
};

static const struct mfd_cell max77705_sensor_cells[] = {
	{ .name = "max77705-battery" },
	{ .name = "max77705-hwmon", },
};

static const struct simple_mfd_data maxim_mon_max77705 = {
	.mfd_cell = max77705_sensor_cells,
	.mfd_cell_size = ARRAY_SIZE(max77705_sensor_cells),
};

static const struct regmap_config spacemit_p1_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static const struct mfd_cell spacemit_p1_cells[] = {
	{ .name = "spacemit-p1-regulator", },
	{ .name = "spacemit-p1-rtc", },
	/* PMIC reset/poweroff (spacemit-p1-reboot.c): nothing else resets on a plain reboot. */
	{ .name = "spacemit-p1-reboot", },
};

/* P1 (SPM8821) interrupts: status 0x91-0x97 (W1C), enable 0x98-0x9e; only the power key. */
#define P1_IRQ_STATUS_BASE	0x91
#define P1_IRQ_ENABLE_BASE	0x98

enum {
	P1_IRQ_PWRON_RISE,	/* power key released */
	P1_IRQ_PWRON_FALL,	/* power key pressed */
	P1_IRQ_PWRON_SHORT,
	P1_IRQ_PWRON_LONG,
};

static const struct regmap_irq spacemit_p1_irqs[] = {
	REGMAP_IRQ_REG(P1_IRQ_PWRON_RISE, 6, BIT(0)),
	REGMAP_IRQ_REG(P1_IRQ_PWRON_FALL, 6, BIT(1)),
	REGMAP_IRQ_REG(P1_IRQ_PWRON_SHORT, 6, BIT(2)),
	REGMAP_IRQ_REG(P1_IRQ_PWRON_LONG, 6, BIT(3)),
};

static const struct regmap_irq_chip spacemit_p1_irq_chip = {
	.name = "spacemit-p1",
	.irqs = spacemit_p1_irqs,
	.num_irqs = ARRAY_SIZE(spacemit_p1_irqs),
	.num_regs = 7,
	.status_base = P1_IRQ_STATUS_BASE,
	.unmask_base = P1_IRQ_ENABLE_BASE,
	.ack_base = P1_IRQ_STATUS_BASE,
	.init_ack_masked = true,
};

static const struct resource spacemit_p1_pwrkey_resources[] = {
	DEFINE_RES_IRQ_NAMED(P1_IRQ_PWRON_RISE, "rise"),
	DEFINE_RES_IRQ_NAMED(P1_IRQ_PWRON_FALL, "fall"),
};

static const struct mfd_cell spacemit_p1_irq_cells[] = {
	{
		.name = "spacemit-p1-pwrkey",
		.resources = spacemit_p1_pwrkey_resources,
		.num_resources = ARRAY_SIZE(spacemit_p1_pwrkey_resources),
	},
};

static const struct simple_mfd_data spacemit_p1 = {
	.regmap_config = &spacemit_p1_regmap_config,
	.mfd_cell = spacemit_p1_cells,
	.mfd_cell_size = ARRAY_SIZE(spacemit_p1_cells),
	.irq_chip = &spacemit_p1_irq_chip,
	.irq_mfd_cell = spacemit_p1_irq_cells,
	.irq_mfd_cell_size = ARRAY_SIZE(spacemit_p1_irq_cells),
};

static const struct of_device_id simple_mfd_i2c_of_match[] = {
	{ .compatible = "delta,tn48m-cpld" },
	{ .compatible = "fsl,ls1028aqds-fpga" },
	{ .compatible = "fsl,lx2160aqds-fpga" },
	{ .compatible = "fsl,lx2160ardb-fpga" },
	{ .compatible = "kontron,sl28cpld" },
	{ .compatible = "maxim,max5970", .data = &maxim_max5970 },
	{ .compatible = "maxim,max5978", .data = &maxim_max5970 },
	{ .compatible = "maxim,max77705-battery", .data = &maxim_mon_max77705 },
	{ .compatible = "silergy,sy7636a", .data = &silergy_sy7636a },
	{ .compatible = "spacemit,p1", .data = &spacemit_p1 },
	{}
};
MODULE_DEVICE_TABLE(of, simple_mfd_i2c_of_match);

static struct i2c_driver simple_mfd_i2c_driver = {
	.probe = simple_mfd_i2c_probe,
	.driver = {
		.name = "simple-mfd-i2c",
		.of_match_table = simple_mfd_i2c_of_match,
	},
};
module_i2c_driver(simple_mfd_i2c_driver);

MODULE_AUTHOR("Michael Walle <michael@walle.cc>");
MODULE_DESCRIPTION("Simple MFD - I2C driver");
MODULE_LICENSE("GPL v2");
