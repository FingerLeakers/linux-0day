/*
 * Copyright (c) 2017 exceet electronics GmbH
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/mtd/spinand.h>

#define SPINAND_MFR_WINBOND		0xEF

static SPINAND_OP_VARIANTS(read_cache_variants,
		SPINAND_PAGE_READ_FROM_CACHE_QUADIO_OP(NULL, 2, NULL, 0),
		SPINAND_PAGE_READ_FROM_CACHE_X4_OP(NULL, 1, NULL, 0),
		SPINAND_PAGE_READ_FROM_CACHE_DUALIO_OP(NULL, 1, NULL, 0),
		SPINAND_PAGE_READ_FROM_CACHE_X2_OP(NULL, 1, NULL, 0),
		SPINAND_PAGE_READ_FROM_CACHE_OP(true, NULL, 1, NULL, 0),
		SPINAND_PAGE_READ_FROM_CACHE_OP(false, NULL, 1, NULL, 0));

static SPINAND_OP_VARIANTS(write_cache_variants,
		SPINAND_PROG_LOAD_X4(true, NULL, NULL, 0),
		SPINAND_PROG_LOAD(true, NULL, NULL, 0));

static SPINAND_OP_VARIANTS(update_cache_variants,
		SPINAND_PROG_LOAD_X4(false, NULL, NULL, 0),
		SPINAND_PROG_LOAD(false, NULL, NULL, 0));

static const struct spinand_info winbond_spinand_table[] = {
	SPINAND_INFO("W25M02GV", 0xAB,
		     NAND_MEMORG(1, 2048, 64, 64, 1024, 1, 1, 1),
		     NAND_ECCREQ(8, 512),
		     SPINAND_INFO_OP_VARIANTS(&read_cache_variants,
					      &write_cache_variants,
					      &update_cache_variants)),
};

/**
 * winbond_spinand_detect - initialize device related part in spinand_device
 * struct if it is a Winbond device.
 * @spinand: SPI NAND device structure
 */
static int winbond_spinand_detect(struct spinand_device *spinand)
{
	u8 *id = spinand->id.data;
	int ret;

	/*
	 * Winbond SPI NAND read ID need a dummy byte,
	 * so the first byte in raw_id is dummy.
	 */
	if (id[1] != SPINAND_MFR_WINBOND)
		return 0;

	ret = spinand_match_and_init(spinand, winbond_spinand_table,
				     ARRAY_SIZE(winbond_spinand_table), id[2]);
	if (ret)
		return ret;

	return 1;
}

static const struct spinand_manufacturer_ops winbond_spinand_manuf_ops = {
	.detect = winbond_spinand_detect,
};

const struct spinand_manufacturer winbond_spinand_manufacturer = {
	.id = SPINAND_MFR_WINBOND,
	.name = "Winbond",
	.ops = &winbond_spinand_manuf_ops,
};
