/*
 * drivers/staging/xgifb/vb_util.c
 *
 * Copyright (c) 2016 Allwinnertech Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */
#include <linux/io.h>
#include <linux/types.h>

#include "vb_def.h"
#include "vgatypes.h"
#include "vb_struct.h"

#include "XGIfb.h"

#include "vb_util.h"

void xgifb_reg_set(unsigned long port, u8 index, u8 data)
{
	outb(index, port);
	outb(data, port + 1);
}

u8 xgifb_reg_get(unsigned long port, u8 index)
{
	u8 data;

	outb(index, port);
	data = inb(port + 1);
	return data;
}

void xgifb_reg_and_or(unsigned long port, u8 index,
		unsigned data_and, unsigned data_or)
{
	u8 temp;

	temp = xgifb_reg_get(port, index); /* XGINew_Part1Port index 02 */
	temp = (temp & data_and) | data_or;
	xgifb_reg_set(port, index, temp);
}

void xgifb_reg_and(unsigned long port, u8 index, unsigned data_and)
{
	u8 temp;

	temp = xgifb_reg_get(port, index); /* XGINew_Part1Port index 02 */
	temp &= data_and;
	xgifb_reg_set(port, index, temp);
}

void xgifb_reg_or(unsigned long port, u8 index, unsigned data_or)
{
	u8 temp;

	temp = xgifb_reg_get(port, index); /* XGINew_Part1Port index 02 */
	temp |= data_or;
	xgifb_reg_set(port, index, temp);
}
