// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2020 Nuvoton Technology Corp.
 *
 */

#include <common.h>
#include <clk.h>
#include <dm.h>
#include <log.h>
#include <malloc.h>
#include <usb.h>
#include <asm/io.h>

#include <dt-bindings/clock/ma35d1-clk.h>
#include <syscon.h>
#include <regmap.h>
#include <ma35d1-sys.h>

#include "ohci.h"

/* Declare global data pointer */
DECLARE_GLOBAL_DATA_PTR;

struct ma35d1_ohci_priv {
	int	id;
	struct regmap	*sysreg;
	void __iomem	*reg;
	struct clk  clk;
};

static int ohci_ma35d1_probe(struct udevice *dev)
{
	struct ma35d1_ohci_priv *ma35d1_ohci = dev_get_priv(dev);
	struct ohci_regs *regs = (struct ohci_regs *)devfdt_get_addr(dev);
	struct ofnode_phandle_args args;
	int	ret;

	/*
	 *  enable clock
	 */
	ret = clk_get_by_index(dev, 0, &ma35d1_ohci->clk);
	if (ret < 0) {
		dev_err(dev, "%s failed: %d\n", __func__, ret);
		return ret;
	}

	ret = clk_enable(&ma35d1_ohci->clk);
	if (ret != 0) {
		dev_err(dev, "%s clk_enable() failed: %d\n", __func__, ret);
		return ret;
	}

	/* get system register map */
	ret = dev_read_phandle_with_args(dev, "nuvoton,sys", NULL,
					 0, 0, &args);
	if (ret) {
		dev_err(dev, "Failed to get syscon: %d\n", ret);
		return ret;
	}

	ma35d1_ohci->sysreg = syscon_node_to_regmap(args.node);
	if (IS_ERR(ma35d1_ohci->sysreg)) {
		ret = PTR_ERR(ma35d1_ohci->sysreg);
		dev_err(dev, "can't get syscon: %d\n", ret);
		return ret;
	}

	return ohci_register(dev, regs);
}

static const struct udevice_id ohci_ma35d1_ids[] = {
	{ .compatible = "nuvoton,ma35d1-ohci0" },
	{ .compatible = "nuvoton,ma35d1-ohci1" },
	{ .compatible = "nuvoton,ma35d1-ohci2" },
	{ }
};

U_BOOT_DRIVER(ohci_generic) = {
	.name	= "ohci_ma35d1",
	.id	= UCLASS_USB,
	.of_match = ohci_ma35d1_ids,
	.probe = ohci_ma35d1_probe,
	.remove = ohci_deregister,
	.ops	= &ohci_usb_ops,
	.platdata_auto_alloc_size = sizeof(struct usb_platdata),
	.priv_auto_alloc_size = sizeof(struct ma35d1_ohci_priv),
	.flags	= DM_FLAG_ALLOC_PRIV_DMA,
};
