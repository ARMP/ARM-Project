/*
 * Bluetooth+WiFi Murata LBEE19QMBC rfkill power control via GPIO
 *
 * Copyright (C) 2012 LGE Inc.
 * Copyright (C) 2010 NVIDIA Corporation
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#define DEBUG	1

#include <linux/module.h>
#include <linux/init.h>
#include <linux/gpio.h>
#include <linux/rfkill.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/wakelock.h>
#include <linux/spinlock.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/lbee9qmb-rfkill.h>
#include <linux/mutex.h>

#define CHECK_HOST_WAKE_ON_RESUME	1

#define BT_HOST_WAKELOCK_TIMEOUT	(5*HZ)

struct bcm_bt_lpm {
	struct device *dev;

	struct rfkill *rfkill;
	struct rfkill *rfkill_btwake;

	int gpio_bt_wake;
	int gpio_host_wake;
	int gpio_reset;
	int reset_delay;
	int active_low;

	int irq;

	int bt_wake;
	int host_wake;
	spinlock_t slock;
	struct mutex mlock;
	struct wake_lock wake_lock;
	int (*chip_enable)(void);
	int (*chip_disable)(void);

	int blocked; /* 0: on, 1: off */
};

#ifdef CONFIG_BRCM_HOST_WAKE
static void update_host_wake_locked(void *data)
{
	struct bcm_bt_lpm *lpm = data;
	int h_wake;

	h_wake = gpio_get_value(lpm->gpio_host_wake);

	dev_dbg(lpm->dev, "%s: host_wake %d\n", __func__, h_wake);

	if (h_wake == lpm->host_wake)
		return;

	lpm->host_wake = h_wake;

	if(lpm->active_low){
		if (!h_wake) {
			dev_dbg(lpm->dev, "%s: Call wake_lock_timeout\n", __func__);
			wake_lock_timeout(&lpm->wake_lock,
					BT_HOST_WAKELOCK_TIMEOUT);
		}
	}
	else {
		if (h_wake) {
			dev_dbg(lpm->dev, "%s: Call wake_lock_timeout\n", __func__);
			wake_lock_timeout(&lpm->wake_lock,
					BT_HOST_WAKELOCK_TIMEOUT);
		}
	}
}

static irqreturn_t host_wake_isr(int irq, void *data)
{
	struct bcm_bt_lpm *lpm = data;
	unsigned long flags;
	int h_wake;

	h_wake = gpio_get_value(lpm->gpio_host_wake);
	irq_set_irq_type(irq, h_wake? IRQF_TRIGGER_LOW : IRQF_TRIGGER_HIGH);

	dev_dbg(lpm->dev, "%s\n", __func__);

	spin_lock_irqsave(&lpm->slock, flags);
	update_host_wake_locked(lpm);
	spin_unlock_irqrestore(&lpm->slock, flags);
	return IRQ_HANDLED;
}
#endif /* CONFIG_BRCM_HOST_WAKE */

static int lbee9qmb_rfkill_set_power(void *data, bool blocked)
{
	struct bcm_bt_lpm *lpm = data;

	if (!lpm)
		return -ENODEV;

	dev_dbg(lpm->dev, "%s: enabled=%d\n", __func__, !blocked);

	mutex_lock(&lpm->mlock);
	if (lpm->blocked == blocked) {
		mutex_unlock(&lpm->mlock);
		dev_dbg(lpm->dev, "%s: new setting is ignored(already set)\n",
				__func__);
		return 0;
	}

	lpm->blocked = blocked;

	if (blocked) {
		/* LGE_SJIT 11/18/2011 [mohamed.khadri@lge.com] BT UART Disable*/
		if (lpm->chip_disable) {
			if (lpm->chip_disable())
				dev_err(lpm->dev, "%s: uart disable failed\n",
						__func__);
		}
		gpio_set_value(lpm->gpio_reset, 0);
		dev_dbg(lpm->dev, "%s: reset low\n", __func__);
	}
	else {
		/* LGE_SJIT 11/18/2011 [mohamed.khadri@lge.com] BT UART Enable*/
		if (lpm->chip_enable) {
			if (lpm->chip_enable())
				dev_err(lpm->dev, "%s: uart enable failed\n",
						__func__);
		}
		gpio_set_value(lpm->gpio_reset, 0);
		dev_dbg(lpm->dev, "%s: reset low\n", __func__);
		msleep(lpm->reset_delay);
		gpio_set_value(lpm->gpio_reset, 1);
		dev_dbg(lpm->dev, "%s: reset high\n", __func__);
		msleep(lpm->reset_delay);
	}
	mutex_unlock(&lpm->mlock);

	return 0;
}

static struct rfkill_ops lbee9qmb_rfkill_ops = {
	.set_block = lbee9qmb_rfkill_set_power,
};

#ifdef CONFIG_BRCM_BT_WAKE
static int lbee9qmb_rfkill_set_btwake(void *data, bool wake)
{
	struct bcm_bt_lpm *lpm = data;
	int b_wake, h_wake;

	if (!lpm)
		return -ENODEV;

	if(lpm->bt_wake == wake)
		return 0;

	b_wake = gpio_get_value(lpm->gpio_bt_wake);
#ifdef CONFIG_BRCM_HOST_WAKE
	h_wake = gpio_get_value(lpm->gpio_host_wake);
#endif

	dev_dbg(lpm->dev, "%s: wake %d\n", __func__, wake);
	dev_dbg(lpm->dev, "%s: bt_wake %d\n", __func__, b_wake);
#ifdef CONFIG_BRCM_HOST_WAKE
	dev_dbg(lpm->dev, "%s: host_wake %d\n", __func__, h_wake);
#endif

	lpm->bt_wake = wake;

	if (wake) {
		gpio_set_value(lpm->gpio_bt_wake, 1);
		
		if (lpm->active_low)
			wake_unlock(&lpm->wake_lock);
		else
			wake_lock(&lpm->wake_lock);
	}
	else {
		gpio_set_value(lpm->gpio_bt_wake, 0);
		
		if (lpm->active_low)		
			wake_lock(&lpm->wake_lock);
		else
			wake_unlock(&lpm->wake_lock);
	}

	b_wake = gpio_get_value(lpm->gpio_bt_wake);
	h_wake = gpio_get_value(lpm->gpio_host_wake);

	dev_dbg(lpm->dev, "%s: bt_wake %d\n", __func__, b_wake);
#ifdef CONFIG_BRCM_HOST_WAKE
	dev_dbg(lpm->dev, "%s: host_wake %d\n", __func__, h_wake);
#endif
	return 0;
}

static struct rfkill_ops lbee9qmb_rfkill_btwake_ops = {
	.set_block = lbee9qmb_rfkill_set_btwake,
};
#endif /* CONFIG_BRCM_BT_WAKE */

static int lbee9qmb_rfkill_probe(struct platform_device *pdev)
{
	struct lbee9qmb_platform_data *pdata = pdev->dev.platform_data;
	struct bcm_bt_lpm *lpm;

	int rc;
	
	dev_dbg(&pdev->dev, "%s\n", __func__);
	
	if (!pdata) {
		dev_err(&pdev->dev, "no platform data\n");
		return -ENOSYS;
	}

	lpm = kzalloc(sizeof(struct bcm_bt_lpm), GFP_KERNEL);
	if (!lpm) {
		dev_err(&pdev->dev, "could not allocate memory\n");
		rc = -ENOMEM;
		goto err_kzalloc;
	}

	lpm->gpio_reset = pdata->gpio_reset;
#ifdef CONFIG_BRCM_BT_WAKE
	lpm->gpio_bt_wake = pdata->gpio_btwake;
	lpm->active_low = pdata->active_low;
#endif
#ifdef CONFIG_BRCM_HOST_WAKE
	lpm->gpio_host_wake = pdata->gpio_hostwake;
#endif
	lpm->reset_delay = pdata->delay;
	lpm->blocked = -1; /* will be off */

	if (pdata->chip_enable)
		lpm->chip_enable = pdata->chip_enable;
	if (pdata->chip_disable)
		lpm->chip_disable = pdata->chip_disable;
	lpm->dev = &pdev->dev;

	/* request gpios */
	rc = gpio_request(pdata->gpio_reset, "bt_reset");
	if (rc < 0) {
		dev_err(&pdev->dev, "gpio_request(bt_reset) failed\n");
		goto err_gpio_bt_reset;
	}
#ifdef CONFIG_BRCM_BT_WAKE
	rc = gpio_request(pdata->gpio_btwake, "bt_wake");
	if (rc < 0) {
		dev_err(&pdev->dev, "gpio_request(bt_wake) failed\n");
		goto err_gpio_bt_wake;
	}
#endif
#ifdef CONFIG_BRCM_HOST_WAKE
	rc = gpio_request(pdata->gpio_hostwake, "host_wake");
	if (rc < 0) {
		dev_err(&pdev->dev, "gpio_request(bt_hostwake) failed\n");
		goto err_gpio_host_wake;
	}
#endif

	/* mutex */
	mutex_init(&lpm->mlock);

	/* wakelock init */
	wake_lock_init(&lpm->wake_lock, WAKE_LOCK_SUSPEND, "BTLowPower");

	lpm->rfkill = rfkill_alloc("lbee9qmb-rfkill", &pdev->dev,
			RFKILL_TYPE_BLUETOOTH, &lbee9qmb_rfkill_ops, lpm);
	if (!lpm->rfkill) {
		rc = -ENOMEM;
		dev_err(&pdev->dev, "%s: rfkill_alloc failed\n", __func__);
		goto err_rfkill;
	}

	rc = rfkill_register(lpm->rfkill);
	if (rc < 0) {
		dev_err(&pdev->dev, "%s: rfkill_register failed\n", __func__);
		goto err_rfkill_register;
	}

#ifdef CONFIG_BRCM_BT_WAKE
	lpm->rfkill_btwake = rfkill_alloc("lbee9qmb-rfkill_btwake", &pdev->dev,
			RFKILL_TYPE_BLUETOOTH, &lbee9qmb_rfkill_btwake_ops, lpm);
	if (!lpm->rfkill_btwake) {
		rc = -ENOMEM;
		dev_err(&pdev->dev, "%s: rfkill_alloc(btwake) failed\n", __func__);
		goto err_rfkill_btwake;
	}

	rc = rfkill_register(lpm->rfkill_btwake);
	if (rc < 0) {
		dev_err(&pdev->dev, "%s: rfkill_register(btwake) failed\n", __func__);
		goto err_rfkill_btwake_register;
	}
#endif
	platform_set_drvdata(pdev, lpm);

	gpio_direction_output(pdata->gpio_reset, 0);
#ifdef CONFIG_BRCM_BT_WAKE
	if (lpm->active_low)
		gpio_direction_output(pdata->gpio_btwake, 1);
	else
		gpio_direction_output(pdata->gpio_btwake, 0);
#endif
#ifdef CONFIG_BRCM_HOST_WAKE
	spin_lock_init(&lpm->slock);

	gpio_direction_input(pdata->gpio_hostwake);
	lpm->irq = gpio_to_irq(pdata->gpio_hostwake);

	rc = request_irq(lpm->irq, host_wake_isr,
			lpm->active_low ? IRQF_TRIGGER_LOW : IRQF_TRIGGER_HIGH,
			"bt host_wake", lpm);
	if (rc < 0) {
		dev_err(&pdev->dev, "%s: request_irq failed\n", __func__);
		goto err_request_irq;
	}
	enable_irq_wake(lpm->irq);
#endif

	/* bt chip off on booting */
	lbee9qmb_rfkill_set_power(lpm, 1);
	/* sync with internal rfkill state */
	rfkill_set_states(lpm->rfkill, 1, 0);

	dev_info(&pdev->dev, "%s: probed\n", __func__);
	return 0;

#ifdef CONFIG_BRCM_HOST_WAKE
err_request_irq:
#endif
#ifdef CONFIG_BRCM_BT_WAKE
	rfkill_unregister(lpm->rfkill_btwake);
err_rfkill_btwake_register:
	rfkill_destroy(lpm->rfkill_btwake);
err_rfkill_btwake:
#endif
	rfkill_unregister(lpm->rfkill);
err_rfkill_register:
	rfkill_destroy(lpm->rfkill);
err_rfkill:
	wake_lock_destroy(&lpm->wake_lock);
#ifdef CONFIG_BRCM_HOST_WAKE
	gpio_free(pdata->gpio_hostwake);
err_gpio_host_wake:
#endif
#ifdef CONFIG_BRCM_BT_WAKE
	gpio_free(pdata->gpio_btwake);
err_gpio_bt_wake:
#endif
	gpio_free(pdata->gpio_reset);
err_gpio_bt_reset:
	kfree(lpm);
err_kzalloc:
	return rc;
}

static int lbee9qmb_rfkill_remove(struct platform_device *pdev)
{
	struct bcm_bt_lpm *lpm = platform_get_drvdata(pdev);

	dev_dbg(&pdev->dev, "%s\n", __func__);

#ifdef CONFIG_BRCM_HOST_WAKE
	free_irq(lpm->irq, lpm);
#endif
#ifdef CONFIG_BRCM_BT_WAKE
	rfkill_unregister(lpm->rfkill_btwake);
	rfkill_destroy(lpm->rfkill_btwake);
#endif
	rfkill_unregister(lpm->rfkill);
	rfkill_destroy(lpm->rfkill);
	wake_lock_destroy(&lpm->wake_lock);
#ifdef CONFIG_BRCM_HOST_WAKE
	gpio_free(lpm->gpio_host_wake);
#endif
#ifdef CONFIG_BRCM_BT_WAKE
	gpio_free(lpm->gpio_bt_wake);
#endif
	gpio_free(lpm->gpio_reset);

	kfree(lpm);

	return 0;
}

static int lbee9qmb_rfkill_suspend(struct platform_device *pdev,
		pm_message_t state)
{
	struct bcm_bt_lpm *lpm = platform_get_drvdata(pdev);
	int v;
	int wake;
	int ret = 0;

	if (!lpm->blocked) {
#ifdef CONFIG_BRCM_HOST_WAKE
		disable_irq(lpm->irq);
		v = gpio_get_value(lpm->gpio_host_wake);

		if (lpm->active_low)
			wake = !v;
		else
			wake = v;

		if (wake) {
			dev_warn(&pdev->dev, "host waked\n");
			ret = -EBUSY;
			goto failed;
		}
#endif

		/* disable uart to sleep */
		if (lpm->chip_disable) {
			ret = lpm->chip_disable();
			if (ret < 0) {
				dev_warn(&pdev->dev, "failed uart disabe\n");
				goto failed;
			}
		}
	}
	return 0;

failed:
#ifdef CONFIG_BRCM_HOST_WAKE
	enable_irq(lpm->irq);
#endif
	return ret;
}

static int lbee9qmb_rfkill_resume(struct platform_device *pdev)
{
	struct bcm_bt_lpm *lpm = platform_get_drvdata(pdev);
	unsigned long flags;


	if (!lpm->blocked) {
		/* re-enable uart */
		if (lpm->chip_enable) {
			if (lpm->chip_enable()) {
				dev_err(&pdev->dev, "failed uart enable\n");
			}
		}
#ifdef CONFIG_BRCM_HOST_WAKE
#ifdef CHECK_HOST_WAKE_ON_RESUME
		spin_lock_irqsave(&lpm->slock, flags);
		update_host_wake_locked(lpm);
		spin_unlock_irqrestore(&lpm->slock, flags);
#endif

		enable_irq(lpm->irq);
#endif
	}
	return 0;
}

static struct platform_driver lbee9qmb_rfkill_driver = {
	.probe = lbee9qmb_rfkill_probe,
	.remove = lbee9qmb_rfkill_remove,
	.suspend = lbee9qmb_rfkill_suspend,
	.resume = lbee9qmb_rfkill_resume,
	.driver = {
		.name = "lbee9qmb-rfkill",
		.owner = THIS_MODULE,
	},
};

static int __init lbee9qmb_rfkill_init(void)
{
	return platform_driver_register(&lbee9qmb_rfkill_driver);
}

static void __exit lbee9qmb_rfkill_exit(void)
{
	platform_driver_unregister(&lbee9qmb_rfkill_driver);
}

module_init(lbee9qmb_rfkill_init);
module_exit(lbee9qmb_rfkill_exit);

MODULE_DESCRIPTION("Murata LBEE9QMBC rfkill");
MODULE_AUTHOR("NVIDIA/LGE");
MODULE_LICENSE("GPL");
