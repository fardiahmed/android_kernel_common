// SPDX-License-Identifier: GPL-2.0-only
/* SpacemiT P1 (SPM8821) PMIC power key, ported from the vendor kernel. */

#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeirq.h>

/* On a quick tap both bits are set and rise is dispatched first: track the state. */
struct p1_pwrkey {
	struct input_dev *input;
	bool down;
	bool release_pending;
};

static void p1_pwrkey_report(struct p1_pwrkey *key, bool down)
{
	input_report_key(key->input, KEY_POWER, down);
	input_sync(key->input);
	key->down = down;
}

static irqreturn_t p1_pwrkey_fall_irq(int irq, void *data)
{
	struct p1_pwrkey *key = data;

	p1_pwrkey_report(key, true);
	if (key->release_pending) {
		key->release_pending = false;
		p1_pwrkey_report(key, false);
	}
	pm_wakeup_event(key->input->dev.parent, 0);

	return IRQ_HANDLED;
}

static irqreturn_t p1_pwrkey_rise_irq(int irq, void *data)
{
	struct p1_pwrkey *key = data;

	if (key->down)
		p1_pwrkey_report(key, false);
	else
		key->release_pending = true;
	pm_wakeup_event(key->input->dev.parent, 0);

	return IRQ_HANDLED;
}

static int p1_pwrkey_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct p1_pwrkey *key;
	struct input_dev *pwr;
	int rise_irq, fall_irq, err;

	rise_irq = platform_get_irq_byname(pdev, "rise");
	if (rise_irq < 0)
		return rise_irq;

	fall_irq = platform_get_irq_byname(pdev, "fall");
	if (fall_irq < 0)
		return fall_irq;

	key = devm_kzalloc(dev, sizeof(*key), GFP_KERNEL);
	if (!key)
		return -ENOMEM;

	pwr = devm_input_allocate_device(dev);
	if (!pwr)
		return -ENOMEM;
	key->input = pwr;

	pwr->name = "spacemit-p1-pwrkey";
	pwr->phys = "spacemit-p1-pwrkey/input0";
	pwr->id.bustype = BUS_HOST;
	input_set_capability(pwr, EV_KEY, KEY_POWER);

	err = devm_request_threaded_irq(dev, fall_irq, NULL, p1_pwrkey_fall_irq,
					IRQF_ONESHOT, "p1_pwrkey_fall", key);
	if (err)
		return dev_err_probe(dev, err, "failed to request fall irq\n");

	err = devm_request_threaded_irq(dev, rise_irq, NULL, p1_pwrkey_rise_irq,
					IRQF_ONESHOT, "p1_pwrkey_rise", key);
	if (err)
		return dev_err_probe(dev, err, "failed to request rise irq\n");

	err = input_register_device(pwr);
	if (err)
		return dev_err_probe(dev, err, "failed to register input device\n");

	device_init_wakeup(dev, true);
	dev_pm_set_wake_irq(dev, fall_irq);

	return 0;
}

static void p1_pwrkey_remove(struct platform_device *pdev)
{
	dev_pm_clear_wake_irq(&pdev->dev);
	device_init_wakeup(&pdev->dev, false);
}

static const struct platform_device_id p1_pwrkey_id[] = {
	{ "spacemit-p1-pwrkey" },
	{ }
};
MODULE_DEVICE_TABLE(platform, p1_pwrkey_id);

static struct platform_driver p1_pwrkey_driver = {
	.probe = p1_pwrkey_probe,
	.remove = p1_pwrkey_remove,
	.id_table = p1_pwrkey_id,
	.driver = {
		.name = "spacemit-p1-pwrkey",
	},
};
module_platform_driver(p1_pwrkey_driver);

MODULE_DESCRIPTION("SpacemiT P1 PMIC power key driver");
MODULE_LICENSE("GPL");
