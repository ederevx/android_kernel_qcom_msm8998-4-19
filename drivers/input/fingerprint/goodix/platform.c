/*
 * platform indepent driver interface
 *
 * Coypritht (c) 2017 Goodix
 */
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/timer.h>
#include <linux/err.h>

#include "gf_spi.h"

#if defined(USE_SPI_BUS)
#include <linux/spi/spi.h>
#include <linux/spi/spidev.h>
#elif defined(USE_PLATFORM_BUS)
#include <linux/platform_device.h>
#endif
int gf_pinctrl_init(struct gf_dev* gf_dev)
{
	int ret = 0;
	struct device *dev = &gf_dev->spi->dev;

	gf_dev->gf_pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR_OR_NULL(gf_dev->gf_pinctrl)) {
		dev_err(dev, "Target does not use pinctrl\n");
		ret = PTR_ERR(gf_dev->gf_pinctrl);
		goto err;
	}

	gf_dev->gpio_state_enable =
		pinctrl_lookup_state(gf_dev->gf_pinctrl, "fp_en_init");
	if (IS_ERR_OR_NULL(gf_dev->gpio_state_enable)) {
		dev_err(dev, "Cannot get active pinstate\n");
		ret = PTR_ERR(gf_dev->gpio_state_enable);
		goto err;
	}

	gf_dev->gpio_state_disable =
		pinctrl_lookup_state(gf_dev->gf_pinctrl, "fp_dis_init");
	if (IS_ERR_OR_NULL(gf_dev->gpio_state_disable)) {
		dev_err(dev, "Cannot get active pinstate\n");
		ret = PTR_ERR(gf_dev->gpio_state_disable);
		goto err;
	}

	return 0;
err:
	gf_dev->gf_pinctrl = NULL;
	gf_dev->gpio_state_enable = NULL;
	gf_dev->gpio_state_disable = NULL;
	return ret;
}

int gf_parse_dts(struct gf_dev* gf_dev)
{
	struct device *dev = &gf_dev->spi->dev;
	bool is_dumpling;
	int rc;

	is_dumpling = of_property_read_bool(dev->of_node, "oem,dumpling");
	gf_dev->project_version = is_dumpling ? 0x02 : 0x01;

	/*get reset resource*/
	gf_dev->reset_gpio =
		of_get_named_gpio(dev->of_node, "fp-gpio-reset", 0);
	if (!gpio_is_valid(gf_dev->reset_gpio)) {
		rc = gf_dev->reset_gpio;
		if (rc >= 0)
			rc = -EINVAL;
		dev_err(dev, "RESET GPIO is invalid: %d\n", rc);
		return rc;
	}

	rc = gpio_request(gf_dev->reset_gpio, "goodix_reset");
	if (rc) {
		dev_err(dev, "Failed RESET GPIO. rc = %d\n", rc);
		gf_dev->reset_gpio = -EINVAL;
		return rc;
	}

	rc = gpio_direction_output(gf_dev->reset_gpio, 1);
	if (rc) {
		dev_err(dev, "Failed to configure RESET GPIO. rc = %d\n", rc);
		goto err_reset;
	}

	/*get irq resourece*/
	gf_dev->irq_gpio =
		of_get_named_gpio(dev->of_node, "fp-gpio-irq", 0);
	pr_info("gf::irq_gpio:%d\n", gf_dev->irq_gpio);
	if (!gpio_is_valid(gf_dev->irq_gpio)) {
		rc = gf_dev->irq_gpio;
		if (rc >= 0)
			rc = -EINVAL;
		dev_err(dev, "IRQ GPIO is invalid: %d\n", rc);
		goto err_reset;
	}

	rc = gpio_request(gf_dev->irq_gpio, "goodix_irq");
	if (rc) {
		dev_err(dev, "Failed IRQ GPIO. rc = %d\n", rc);
		gf_dev->irq_gpio = -EINVAL;
		goto err_reset;
	}

	rc = gpio_direction_input(gf_dev->irq_gpio);
	if (rc) {
		dev_err(dev, "Failed to configure IRQ GPIO. rc = %d\n", rc);
		goto err_irq;
	}

	if (is_dumpling) {
		gf_dev->enable_gpio =
			of_get_named_gpio(dev->of_node, "fp-gpio-enable", 0);
		if (!gpio_is_valid(gf_dev->enable_gpio)) {
			rc = gf_dev->enable_gpio;
			if (rc >= 0)
				rc = -EINVAL;
			dev_err(dev, "ENABLE GPIO is invalid: %d\n", rc);
			gf_dev->enable_gpio = -EINVAL;
			goto err_irq;
		}

		rc = devm_gpio_request(dev, gf_dev->enable_gpio, "goodix_en");
		if (rc) {
			dev_err(dev,
				"failed to request enable gpio, rc = %d\n",
				rc);
			gf_dev->enable_gpio = -EINVAL;
			goto err_irq;
		}
	}

	return 0;

err_irq:
	gpio_free(gf_dev->irq_gpio);
	gf_dev->irq_gpio = -EINVAL;
err_reset:
	gpio_free(gf_dev->reset_gpio);
	gf_dev->reset_gpio = -EINVAL;
	return rc;
}

void gf_cleanup(struct gf_dev	*gf_dev)
{
	pr_info("[info] %s\n",__func__);
	if (gpio_is_valid(gf_dev->irq_gpio))
	{
		gpio_free(gf_dev->irq_gpio);
		gf_dev->irq_gpio = -EINVAL;
		pr_info("remove irq_gpio success\n");
	}
	if (gpio_is_valid(gf_dev->reset_gpio))
	{
		gpio_free(gf_dev->reset_gpio);
		gf_dev->reset_gpio = -EINVAL;
		pr_info("remove reset_gpio success\n");
	}
}

int gf_power_on(struct gf_dev* gf_dev)
{
	int rc = 0;

	msleep(10);
	pr_info("---- power on ok ----\n");

	return rc;
}

int gf_power_off(struct gf_dev* gf_dev)
{
	int rc = 0;

	pr_info("---- power off ----\n");
	return rc;
}

int gf_hw_reset(struct gf_dev *gf_dev, unsigned int delay_ms)
{
	if(gf_dev == NULL) {
		pr_info("Input buff is NULL.\n");
		return -1;
	}
	gpio_direction_output(gf_dev->reset_gpio, 1);
	gpio_set_value(gf_dev->reset_gpio, 0);
	mdelay(3);
	gpio_set_value(gf_dev->reset_gpio, 1);
	mdelay(delay_ms);
	return 0;
}

int gf_irq_num(struct gf_dev *gf_dev)
{
	if(gf_dev == NULL) {
		pr_info("Input buff is NULL.\n");
		return -1;
	} else {
		return gpio_to_irq(gf_dev->irq_gpio);
	}
}
