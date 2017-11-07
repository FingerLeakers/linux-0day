/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2016-2017 Micron Technology, Inc.
 *
 *  Authors:
 *	Peter Pan <peterpandong@micron.com>
 */
#ifndef __LINUX_MTD_SPINAND_H
#define __LINUX_MTD_SPINAND_H

#include <linux/mutex.h>
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <linux/spi/spi.h>

/**
 * Standard SPI NAND flash operations
 */

#define SPINAND_RESET_OP						\
	SPI_MEM_OP(SPI_MEM_OP_CMD(0xff, 1),				\
		   SPI_MEM_OP_NO_ADDRS,					\
		   SPI_MEM_OP_NO_DUMMY,					\
		   SPI_MEM_OP_NO_DATA)

#define SPINAND_WR_EN_DIS_OP(enable)					\
	SPI_MEM_OP(SPI_MEM_OP_CMD((enable) ? 0x06 : 0x04, 1),		\
		   SPI_MEM_OP_NO_ADDRS,					\
		   SPI_MEM_OP_NO_DUMMY,					\
		   SPI_MEM_OP_NO_DATA)

#define SPINAND_READID_OP(ndummy, buf, len)				\
	SPI_MEM_OP(SPI_MEM_OP_CMD(0x9f, 1),				\
		   SPI_MEM_OP_NO_ADDRS,					\
		   SPI_MEM_OP_DUMMY(ndummy, 1),				\
		   SPI_MEM_OP_DATA_IN(len, buf, 1))

#define SPINAND_SET_FEATURE_OP(regptr, valptr)				\
	SPI_MEM_OP(SPI_MEM_OP_CMD(0x1f, 1),				\
		   SPI_MEM_OP_ADDRS(1, regptr, 1),			\
		   SPI_MEM_OP_NO_DUMMY,					\
		   SPI_MEM_OP_DATA_OUT(1, valptr, 1))

#define SPINAND_GET_FEATURE_OP(regptr, valptr)				\
	SPI_MEM_OP(SPI_MEM_OP_CMD(0x0f, 1),				\
		   SPI_MEM_OP_ADDRS(1, regptr, 1),			\
		   SPI_MEM_OP_NO_DUMMY,					\
		   SPI_MEM_OP_DATA_IN(1, valptr, 1))

#define SPINAND_BLK_ERASE_OP(addrs)					\
	SPI_MEM_OP(SPI_MEM_OP_CMD(0xd8, 1),				\
		   SPI_MEM_OP_ADDRS(3, addrs, 1),			\
		   SPI_MEM_OP_NO_DUMMY,					\
		   SPI_MEM_OP_NO_DATA)

#define SPINAND_PAGE_READ_OP(addrs)					\
	SPI_MEM_OP(SPI_MEM_OP_CMD(0x13, 1),				\
		   SPI_MEM_OP_ADDRS(3, addrs, 1),			\
		   SPI_MEM_OP_NO_DUMMY,					\
		   SPI_MEM_OP_NO_DATA)

#define SPINAND_PAGE_READ_FROM_CACHE_OP(fast, addrs, ndummy, buf, len)	\
	SPI_MEM_OP(SPI_MEM_OP_CMD(fast ? 0x0b : 0x03, 1),		\
		   SPI_MEM_OP_ADDRS(2, addrs, 1),			\
		   SPI_MEM_OP_DUMMY(ndummy, 1),				\
		   SPI_MEM_OP_DATA_IN(len, buf, 1))

#define SPINAND_PAGE_READ_FROM_CACHE_X2_OP(addrs, ndummy, buf, len)	\
	SPI_MEM_OP(SPI_MEM_OP_CMD(0x3b, 1),				\
		   SPI_MEM_OP_ADDRS(2, addrs, 1),			\
		   SPI_MEM_OP_DUMMY(ndummy, 1),				\
		   SPI_MEM_OP_DATA_IN(len, buf, 2))

#define SPINAND_PAGE_READ_FROM_CACHE_X4_OP(addrs, ndummy, buf, len)	\
	SPI_MEM_OP(SPI_MEM_OP_CMD(0x6b, 1),				\
		   SPI_MEM_OP_ADDRS(2, addrs, 1),			\
		   SPI_MEM_OP_DUMMY(ndummy, 1),				\
		   SPI_MEM_OP_DATA_IN(len, buf, 4))

#define SPINAND_PAGE_READ_FROM_CACHE_DUALIO_OP(addrs, ndummy, buf, len)	\
	SPI_MEM_OP(SPI_MEM_OP_CMD(0xbb, 1),				\
		   SPI_MEM_OP_ADDRS(2, addrs, 2),			\
		   SPI_MEM_OP_DUMMY(ndummy, 2),				\
		   SPI_MEM_OP_DATA_IN(len, buf, 2))

#define SPINAND_PAGE_READ_FROM_CACHE_QUADIO_OP(addrs, ndummy, buf, len)	\
	SPI_MEM_OP(SPI_MEM_OP_CMD(0xeb, 1),				\
		   SPI_MEM_OP_ADDRS(2, addrs, 4),			\
		   SPI_MEM_OP_DUMMY(ndummy, 4),				\
		   SPI_MEM_OP_DATA_IN(len, buf, 4))

#define SPINAND_PROG_EXEC_OP(addrs)					\
	SPI_MEM_OP(SPI_MEM_OP_CMD(0x10, 1),				\
		   SPI_MEM_OP_ADDRS(3, addrs, 1),			\
		   SPI_MEM_OP_NO_DUMMY,					\
		   SPI_MEM_OP_NO_DATA)

#define SPINAND_PROG_LOAD(reset, addrs, buf, len)			\
	SPI_MEM_OP(SPI_MEM_OP_CMD(reset ? 0x02 : 0x84, 1),		\
		   SPI_MEM_OP_ADDRS(2, addrs, 1),			\
		   SPI_MEM_OP_NO_DUMMY,					\
		   SPI_MEM_OP_DATA_OUT(len, buf, 1))

#define SPINAND_PROG_LOAD_X4(reset, addrs, buf, len)			\
	SPI_MEM_OP(SPI_MEM_OP_CMD(reset ? 0x32 : 0x34, 1),		\
		   SPI_MEM_OP_ADDRS(2, addrs, 1),			\
		   SPI_MEM_OP_NO_DUMMY,					\
		   SPI_MEM_OP_DATA_OUT(len, buf, 4))

/**
 * Standard SPI NAND flash commands
 */
#define SPINAND_CMD_PROG_LOAD_X4		0x32
#define SPINAND_CMD_PROG_LOAD_RDM_DATA_X4	0x34

/* feature register */
#define REG_BLOCK_LOCK		0xa0
#define REG_CFG			0xb0
#define REG_STATUS		0xc0

/* status register */
#define STATUS_OIP_MASK		BIT(0)
#define STATUS_CRBSY_MASK	BIT(7)
#define STATUS_READY		0
#define STATUS_BUSY		BIT(0)

#define STATUS_E_FAIL_MASK	BIT(2)
#define STATUS_E_FAIL		BIT(2)

#define STATUS_P_FAIL_MASK	BIT(3)
#define STATUS_P_FAIL		BIT(3)

/* configuration register */
#define CFG_ECC_MASK		BIT(4)
#define CFG_ECC_ENABLE		BIT(4)

/* block lock register */
#define BL_ALL_UNLOCKED		0X00

struct spinand_op;
struct spinand_device;

#define SPINAND_MAX_ID_LEN	4

/**
 * struct spinand_id - SPI NAND id structure
 * @data: buffer containing the id bytes. Currently 4 bytes large, but can
 *	  be extended if required.
 * @len: ID length
 *
 * struct_spinand_id->data contains all bytes returned after a READ_ID command,
 * including dummy bytes if the chip does not emit ID bytes right after the
 * READ_ID command. The responsibility to extract real ID bytes is left to
 * struct_manufacurer_ops->detect().
 */
struct spinand_id {
	u8 data[SPINAND_MAX_ID_LEN];
	int len;
};

/**
 * struct manufacurer_ops - SPI NAND manufacturer specific operations
 * @detect: detect a SPI NAND device. Every time a SPI NAND device is probed
 *	    the core calls the struct_manufacurer_ops->detect() hook of each
 *	    registered manufacturer until one of them return 1. Note that
 *	    the first thing to check in this hook is that the manufacturer ID
 *	    in struct_spinand_device->id matches the manufacturer whose
 *	    ->detect() hook has been called. Should return 1 if there's a
 *	    match, 0 if the manufacturer ID does not match and a negative
 *	    error code otherwise. When true is returned, the core assumes
 *	    that properties of the NAND chip (spinand->base.memorg and
 *	    spinand->base.eccreq) have been filled.
 * @init: initialize a SPI NAND device
 * @cleanup: cleanup a SPI NAND device
 */
struct spinand_manufacturer_ops {
	int (*detect)(struct spinand_device *spinand);
	int (*init)(struct spinand_device *spinand);
	void (*cleanup)(struct spinand_device *spinand);
};

/**
 * struct spinand_manufacturer - SPI NAND manufacturer instance
 * @id: manufacturer ID
 * @name: manufacturer name
 * @ops: manufacturer operations
 */
struct spinand_manufacturer {
	u8 id;
	char *name;
	const struct spinand_manufacturer_ops *ops;
};

extern const struct spinand_manufacturer micron_spinand_manufacturer;

#define SPINAND_CAP_RD_X1	BIT(0)
#define SPINAND_CAP_RD_X2	BIT(1)
#define SPINAND_CAP_RD_X4	BIT(2)
#define SPINAND_CAP_RD_DUAL	BIT(3)
#define SPINAND_CAP_RD_QUAD	BIT(4)
#define SPINAND_CAP_WR_X1	BIT(5)
#define SPINAND_CAP_WR_X2	BIT(6)
#define SPINAND_CAP_WR_X4	BIT(7)
#define SPINAND_CAP_WR_DUAL	BIT(8)
#define SPINAND_CAP_WR_QUAD	BIT(9)

struct spinand_op_variants {
	const struct spi_mem_op *ops;
	unsigned int nops;
};

#define SPINAND_OP_VARIANTS(name, ...)						\
	const struct spinand_op_variants name = {				\
		.ops = (struct spi_mem_op[]) { __VA_ARGS__ },			\
		.nops = sizeof((struct spi_mem_op[]){ __VA_ARGS__ }) /		\
		        sizeof(struct spi_mem_op),				\
	}

struct spinand_info {
	const char *model;
	u8 devid;
	struct nand_memory_organization memorg;
	struct nand_ecc_req eccreq;
	struct {
		const struct spinand_op_variants *read_cache;
		const struct spinand_op_variants *write_cache;
		const struct spinand_op_variants *update_cache;
	} op_variants;
};

#define SPINAND_INFO_OP_VARIANTS(__read, __write, __update)		\
	{								\
		.read_cache = __read,					\
		.write_cache = __write,					\
		.update_cache = __update,				\
	}

#define SPINAND_INFO(__model, __id, __memorg, __eccreq, __op_variants)	\
	{								\
		.model = __model,					\
		.devid = __id,						\
		.memorg = __memorg,					\
		.eccreq = __eccreq,					\
		.op_variants = __op_variants,				\
	}

/**
 * struct spinand_device - SPI NAND device instance
 * @base: NAND device instance
 * @lock: lock used to serialize accesses to the NAND
 * @id: NAND ID as returned by READ_ID
 * @read_cache_op: opcode for the "read from cache" operation
 * @write_cache_op: opcode for the "write to cache" operation
 * @buf: bounce buffer for data
 * @oobbuf: bounce buffer for OOB data
 * @rw_modes: supported read/write mode (combination of SPINAND_CAP_XXX flags)
 * @manufacturer: SPI NAND manufacturer information
 */
struct spinand_device {
	struct nand_device base;
	struct spi_mem *spimem;
	struct mutex lock;
	struct spinand_id id;

	struct {
		const struct spi_mem_op *read_cache;
		const struct spi_mem_op *write_cache;
		const struct spi_mem_op *update_cache;
	} op_templates;

	u8 *buf;
	u8 *oobbuf;
	u32 rw_modes;
	struct {
		const struct spinand_manufacturer *manu;
		void *priv;
	} manufacturer;
};

/**
 * mtd_to_spinand() - Get the SPI NAND device attached to an MTD instance
 * @mtd: MTD instance
 *
 * Return: the SPI NAND device attached to @mtd.
 */
static inline struct spinand_device *mtd_to_spinand(struct mtd_info *mtd)
{
	return container_of(mtd_to_nanddev(mtd), struct spinand_device, base);
}

/**
 * spinand_to_mtd() - Get the MTD device embedded in a SPI NAND device
 * @spinand: SPI NAND device
 *
 * Return: the MTD device embedded in @spinand.
 */
static inline struct mtd_info *spinand_to_mtd(struct spinand_device *spinand)
{
	return nanddev_to_mtd(&spinand->base);
}

/**
 * nand_to_spinand() - Get the SPI NAND device embedding an NAND object
 * @nand: NAND object
 *
 * Return: the SPI NAND device embedding @nand.
 */
static inline struct spinand_device *nand_to_spinand(struct nand_device *nand)
{
	return container_of(nand, struct spinand_device, base);
}

/**
 * spinand_to_nand() - Get the NAND device embedded in a SPI NAND object
 * @spinand: SPI NAND device
 *
 * Return: the NAND device embedded in @spinand.
 */
static inline struct nand_device *
spinand_to_nand(struct spinand_device *spinand)
{
	return &spinand->base;
}

/**
 * spinand_set_of_node - Attach a DT node to a SPI NAND device
 * @spinand: SPI NAND device
 * @np: DT node
 *
 * Attach a DT node to a SPI NAND device.
 */
static inline void spinand_set_of_node(struct spinand_device *spinand,
				       struct device_node *np)
{
	nanddev_set_of_node(&spinand->base, np);
}

int spinand_match_and_init(struct spinand_device *dev,
			   const struct spinand_info *table,
			   unsigned int table_size, u8 devid);

#endif /* __LINUX_MTD_SPINAND_H */
