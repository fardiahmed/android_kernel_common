/******************************************************************************
 *
 * Copyright(c) 2013 - 2017 Realtek Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 *****************************************************************************/
#include <drv_types.h>

/*
 * K1 / Banana Pi F3: the module supply (GPIO67) and WL_REG_ON (GPIO116) are
 * sequenced by the MMC core through the mmc-pwrseq-simple + vmmc regulator in
 * the device tree, and the card is enumerated when sdhci1 probes. There is no
 * out-of-band wake IRQ wired, so the in-band SDIO interrupt is used.
 */
void platform_wifi_get_oob_irq(int *oob_irq)
{
	*oob_irq = 0;
}

void platform_wifi_mac_addr(u8 *mac_addr)
{
}

int platform_wifi_power_on(void)
{
	return 0;
}

void platform_wifi_power_off(void)
{
}
