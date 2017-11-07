// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2016-2017 Micron Technology, Inc.
 *
 * Authors:
 *	Peter Pan <peterpandong@micron.com>
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/mtd/spinand.h>

#define SPINAND_MFR_MICRON		0x2C

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

static const struct spinand_info micron_spinand_table[] = {
	SPINAND_INFO("MT29F2G01ABAGD", 0x24,
		     NAND_MEMORG(1, 2048, 128, 64, 2048, 2, 1, 1),
		     NAND_ECCREQ(8, 512),
		     SPINAND_INFO_OP_VARIANTS(&read_cache_variants,
					      &write_cache_variants,
					      &update_cache_variants)),
};

static int micron_spinand_detect(struct spinand_device *spinand)
{
	u8 *id = spinand->id.data;
	int ret;

	/*
	 * Micron SPI NAND read ID need a dummy byte,
	 * so the first byte in raw_id is dummy.
	 */
	if (id[1] != SPINAND_MFR_MICRON)
		return 0;

	ret = spinand_match_and_init(spinand, micron_spinand_table,
				     ARRAY_SIZE(micron_spinand_table), id[2]);
	if (ret)
		return ret;

	return 1;
}

static const struct spinand_manufacturer_ops micron_spinand_manuf_ops = {
	.detect = micron_spinand_detect,
};

const struct spinand_manufacturer micron_spinand_manufacturer = {
	.id = SPINAND_MFR_MICRON,
	.name = "Micron",
	.ops = &micron_spinand_manuf_ops,
};
