// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2016-2017 Micron Technology, Inc.
 *
 * Authors:
 *	Peter Pan <peterpandong@micron.com>
 */

#define pr_fmt(fmt)	"spi-nand: " fmt

#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mtd/spinand.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>

static void spinand_cache_op_adjust_colum(struct spinand_device *spinand,
					  const struct nand_page_io_req *req,
					  u16 *column)
{
	struct nand_device *nand = spinand_to_nand(spinand);
	unsigned int shift;

	if (nand->memorg.planes_per_lun < 2)
		return;

	/* The plane number is passed in MSB just above the column address */
	shift = fls(nand->memorg.pagesize);
	*column |= req->pos.plane << shift;
}

static int spinand_read_reg_op(struct spinand_device *spinand, u8 reg, u8 *val)
{
	struct spi_mem_op op = SPINAND_GET_FEATURE_OP(&reg, val);

	return spi_mem_exec_op(spinand->spimem, &op);
}

static int spinand_write_reg_op(struct spinand_device *spinand, u8 reg, u8 val)
{
	struct spi_mem_op op = SPINAND_SET_FEATURE_OP(&reg, &val);

	return spi_mem_exec_op(spinand->spimem, &op);
}

static int spinand_read_status(struct spinand_device *spinand, u8 *status)
{
	return spinand_read_reg_op(spinand, REG_STATUS, status);
}

static int spinand_get_cfg(struct spinand_device *spinand, u8 *cfg)
{
	return spinand_read_reg_op(spinand, REG_CFG, cfg);
}

static int spinand_set_cfg(struct spinand_device *spinand, u8 cfg)
{
	return spinand_write_reg_op(spinand, REG_CFG, cfg);
}

static void spinand_disable_ecc(struct spinand_device *spinand)
{
	u8 cfg = 0;

	spinand_get_cfg(spinand, &cfg);

	if ((cfg & CFG_ECC_MASK) == CFG_ECC_ENABLE) {
		cfg &= ~CFG_ECC_ENABLE;
		spinand_set_cfg(spinand, cfg);
	}
}

static int spinand_write_enable_op(struct spinand_device *spinand)
{
	struct spi_mem_op op = SPINAND_WR_EN_DIS_OP(true);

	return spi_mem_exec_op(spinand->spimem, &op);
}

static int spinand_load_page_op(struct spinand_device *spinand,
				const struct nand_page_io_req *req)
{
	struct nand_device *nand = spinand_to_nand(spinand);
	unsigned int row = nanddev_pos_to_row(nand, &req->pos);
	u8 addrs[3] = { row >> 16, row >> 8, row };
	struct spi_mem_op op = SPINAND_PAGE_READ_OP(addrs);

	return spi_mem_exec_op(spinand->spimem, &op);
}

static int spinand_read_from_cache_op(struct spinand_device *spinand,
				      const struct nand_page_io_req *req)
{
	struct spi_mem_op op = *spinand->op_templates.read_cache;
	struct nand_device *nand = spinand_to_nand(spinand);
	struct nand_page_io_req adjreq = *req;
	unsigned int nbytes = 0;
	void *buf = NULL;
	u16 column = 0;
	u8 addrs[2];
	int ret;

	if (req->datalen) {
		adjreq.datalen = nanddev_page_size(nand);
		adjreq.dataoffs = 0;
		adjreq.databuf.in = spinand->buf;
		buf = spinand->buf;
		nbytes = adjreq.datalen;
	}

	if (req->ooblen) {
		adjreq.ooblen = nanddev_per_page_oobsize(nand);
		adjreq.ooboffs = 0;
		adjreq.oobbuf.in = spinand->oobbuf;
		nbytes += nanddev_per_page_oobsize(nand);
		if (!buf) {
			buf = spinand->oobbuf;
			column = nanddev_page_size(nand);
		}
	}

	spinand_cache_op_adjust_colum(spinand, &adjreq, &column);
	op.addr.buf = addrs;

	/*
	 * Some controllers are limited in term of max RX data size. In this
	 * case, just repeat the READ_CACHE operation after updating the
	 * column.
	 */
	while (nbytes) {
		addrs[0] = column >> 8;
		addrs[1] = column;

		op.data.buf.in = buf;
		op.data.nbytes = nbytes;
		ret = spi_mem_adjust_op_size(spinand->spimem, &op);
		if (ret)
			return ret;

		ret = spi_mem_exec_op(spinand->spimem, &op);
		if (ret)
			return ret;

		buf += op.data.nbytes;
		nbytes -= op.data.nbytes;
		column += op.data.nbytes;
	}

	if (req->datalen)
		memcpy(req->databuf.in, spinand->buf + req->dataoffs,
		       req->datalen);

	if (req->ooblen)
		memcpy(req->oobbuf.in, spinand->oobbuf + req->ooboffs,
		       req->ooblen);

	return 0;
}

static int spinand_write_to_cache_op(struct spinand_device *spinand,
				     const struct nand_page_io_req *req)
{
	struct spi_mem_op op =* spinand->op_templates.write_cache;
	struct nand_device *nand = spinand_to_nand(spinand);
	struct nand_page_io_req adjreq = *req;
	unsigned int nbytes = 0;
	void *buf = NULL;
	u16 column = 0;
	u8 addrs[2];
	int ret;

	memset(spinand->buf, 0xff,
	       nanddev_page_size(nand) +
	       nanddev_per_page_oobsize(nand));

	if (req->datalen) {
		memcpy(spinand->buf + req->dataoffs, req->databuf.out,
		       req->datalen);
		adjreq.dataoffs = 0;
		adjreq.datalen = nanddev_page_size(nand);
		adjreq.databuf.out = spinand->buf;
		nbytes = adjreq.datalen;
		buf = spinand->buf;
	}

	if (req->ooblen) {
		memcpy(spinand->oobbuf + req->ooboffs, req->oobbuf.out,
		       req->ooblen);
		adjreq.ooblen = nanddev_per_page_oobsize(nand);
		adjreq.ooboffs = 0;
		nbytes += nanddev_per_page_oobsize(nand);
		if (!buf) {
			buf = spinand->oobbuf;
			column = nanddev_page_size(nand);
		}
	}

	spinand_cache_op_adjust_colum(spinand, &adjreq, &column);

	op = *spinand->op_templates.write_cache;
	op.addr.buf = addrs;

	/*
	 * Some controllers are limited in term of max TX data size. In this
	 * case, split the operation into one LOAD CACHE and one or more
	 * LOAD RANDOM CACHE.
	 */
	while (nbytes) {
		addrs[0] = column >> 8;
		addrs[1] = column;
		op.data.buf.in = buf;
		op.data.nbytes = nbytes;

		ret = spi_mem_adjust_op_size(spinand->spimem, &op);
		if (ret)
			return ret;

		ret = spi_mem_exec_op(spinand->spimem, &op);
		if (ret)
			return ret;

		buf += op.data.nbytes;
		nbytes -= op.data.nbytes;
		column += op.data.nbytes;

		/*
		 * We need to use the RANDOM LOAD CACHE operation if there's
		 * more than one iteration, because the LOAD operation resets
		 * the cache to 0xff.
		 */
		if (nbytes) {
			op = *spinand->op_templates.update_cache;
			op.addr.buf = addrs;
		}
	}

	return 0;
}

static int spinand_program_op(struct spinand_device *spinand,
			      const struct nand_page_io_req *req)
{
	struct nand_device *nand = spinand_to_nand(spinand);
	unsigned int row = nanddev_pos_to_row(nand, &req->pos);
	u8 addrs[3] = { row >> 16, row >> 8, row };
	struct spi_mem_op op = SPINAND_PROG_EXEC_OP(addrs);

	return spi_mem_exec_op(spinand->spimem, &op);
}

static int spinand_erase_op(struct spinand_device *spinand,
			    const struct nand_pos *pos)
{
	struct nand_device *nand = &spinand->base;
	unsigned int row = nanddev_pos_to_row(nand, pos);
	u8 addrs[3] = { row >> 16, row >> 8, row };
	struct spi_mem_op op = SPINAND_BLK_ERASE_OP(addrs);

	return spi_mem_exec_op(spinand->spimem, &op);
}

static int spinand_wait(struct spinand_device *spinand, u8 *s)
{
	unsigned long timeo =  jiffies + msecs_to_jiffies(400);
	u8 status;

	do {
		spinand_read_status(spinand, &status);
		if ((status & STATUS_OIP_MASK) == STATUS_READY)
			goto out;
	} while (time_before(jiffies, timeo));

	/*
	 * Extra read, just in case the STATUS_READY bit has changed
	 * since our last check
	 */
	spinand_read_status(spinand, &status);
out:
	if (s)
		*s = status;

	return (status & STATUS_OIP_MASK) == STATUS_READY ? 0 :	-ETIMEDOUT;
}

static int spinand_read_id_op(struct spinand_device *spinand, u8 *buf)
{
	struct spi_mem_op op = SPINAND_READID_OP(0, buf, SPINAND_MAX_ID_LEN);

	return spi_mem_exec_op(spinand->spimem, &op);
}

static int spinand_reset_op(struct spinand_device *spinand)
{
	struct spi_mem_op op = SPINAND_RESET_OP;
	int ret;

	ret = spi_mem_exec_op(spinand->spimem, &op);
	if (ret)
		return ret;

	return spinand_wait(spinand, NULL);
}

static int spinand_lock_block(struct spinand_device *spinand, u8 lock)
{
	return spinand_write_reg_op(spinand, REG_BLOCK_LOCK, lock);
}

static int spinand_read_page(struct spinand_device *spinand,
			     const struct nand_page_io_req *req)
{
	struct nand_device *nand = spinand_to_nand(spinand);
	int ret;

	spinand_load_page_op(spinand, req);

	ret = spinand_wait(spinand, NULL);
	if (ret < 0) {
		pr_err("failed to load page @%llx (err = %d)\n",
		       nanddev_pos_to_offs(nand, &req->pos), ret);
		return ret;
	}

	spinand_read_from_cache_op(spinand, req);

	return 0;
}

static int spinand_write_page(struct spinand_device *spinand,
			      const struct nand_page_io_req *req)
{
	struct nand_device *nand = spinand_to_nand(spinand);
	u8 status;
	int ret = 0;

	spinand_write_enable_op(spinand);
	spinand_write_to_cache_op(spinand, req);
	spinand_program_op(spinand, req);

	ret = spinand_wait(spinand, &status);
	if (!ret && (status & STATUS_P_FAIL_MASK) == STATUS_P_FAIL)
		ret = -EIO;

	if (ret < 0)
		pr_err("failed to program page @%llx (err = %d)\n",
		       nanddev_pos_to_offs(nand, &req->pos), ret);

	return ret;
}

static int spinand_mtd_read(struct mtd_info *mtd, loff_t from,
			    struct mtd_oob_ops *ops)
{
	struct spinand_device *spinand = mtd_to_spinand(mtd);
	struct nand_device *nand = mtd_to_nanddev(mtd);
	struct nand_io_iter iter;
	int ret;

	mutex_lock(&spinand->lock);
	nanddev_io_for_each_page(nand, from, ops, &iter) {
		ret = spinand_read_page(spinand, &iter.req);
		if (ret)
			break;

		ops->retlen += iter.req.datalen;
		ops->oobretlen += iter.req.ooblen;
	}
	mutex_unlock(&spinand->lock);

	return ret;
}

static int spinand_mtd_write(struct mtd_info *mtd, loff_t to,
			     struct mtd_oob_ops *ops)
{
	struct spinand_device *spinand = mtd_to_spinand(mtd);
	struct nand_device *nand = mtd_to_nanddev(mtd);
	struct nand_io_iter iter;
	int ret = 0;

	mutex_lock(&spinand->lock);
	nanddev_io_for_each_page(nand, to, ops, &iter) {
		ret = spinand_write_page(spinand, &iter.req);
		if (ret)
			break;

		ops->retlen += iter.req.datalen;
		ops->oobretlen += iter.req.ooblen;
	}
	mutex_unlock(&spinand->lock);

	return ret;
}

static bool spinand_isbad(struct nand_device *nand, const struct nand_pos *pos)
{
	struct spinand_device *spinand = nand_to_spinand(nand);
	struct nand_page_io_req req = {
		.pos = *pos,
		.ooblen = 2,
		.ooboffs = 0,
		.oobbuf.in = spinand->oobbuf,
	};

	memset(spinand->oobbuf, 0, 2);
	spinand_read_page(spinand, &req);
	if (spinand->oobbuf[0] != 0xff || spinand->oobbuf[1] != 0xff)
		return true;

	return false;
}

static int spinand_mtd_block_isbad(struct mtd_info *mtd, loff_t offs)
{
	struct nand_device *nand = mtd_to_nanddev(mtd);
	struct spinand_device *spinand = nand_to_spinand(nand);
	struct nand_pos pos;
	int ret;

	nanddev_offs_to_pos(nand, offs, &pos);
	mutex_lock(&spinand->lock);
	ret = nanddev_isbad(nand, &pos);
	mutex_unlock(&spinand->lock);

	return ret;
}

static int spinand_markbad(struct nand_device *nand, const struct nand_pos *pos)
{
	struct spinand_device *spinand = nand_to_spinand(nand);
	struct nand_page_io_req req = {
		.pos = *pos,
		.ooboffs = 0,
		.ooblen = 2,
		.oobbuf.out = spinand->oobbuf,
	};

	/* Erase block before marking it bad. */
	spinand_write_enable_op(spinand);
	spinand_erase_op(spinand, pos);

	memset(spinand->oobbuf, 0, 2);
	return spinand_write_page(spinand, &req);
}

static int spinand_mtd_block_markbad(struct mtd_info *mtd, loff_t offs)
{
	struct nand_device *nand = mtd_to_nanddev(mtd);
	struct spinand_device *spinand = nand_to_spinand(nand);
	struct nand_pos pos;
	int ret;

	nanddev_offs_to_pos(nand, offs, &pos);
	mutex_lock(&spinand->lock);
	ret = nanddev_markbad(nand, &pos);
	mutex_unlock(&spinand->lock);

	return ret;
}

static int spinand_erase(struct nand_device *nand, const struct nand_pos *pos)
{
	struct spinand_device *spinand = nand_to_spinand(nand);
	u8 status;
	int ret;

	spinand_write_enable_op(spinand);
	spinand_erase_op(spinand, pos);

	ret = spinand_wait(spinand, &status);

	if (!ret && (status & STATUS_E_FAIL_MASK) == STATUS_E_FAIL)
		ret = -EIO;

	if (ret)
		pr_err("failed to erase block %d (err = %d)\n",
		       pos->eraseblock, ret);

	return ret;
}

static int spinand_mtd_erase(struct mtd_info *mtd,
			     struct erase_info *einfo)
{
	struct spinand_device *spinand = mtd_to_spinand(mtd);
	int ret;

	mutex_lock(&spinand->lock);
	ret = nanddev_mtd_erase(mtd, einfo);
	mutex_unlock(&spinand->lock);

	if (!ret)
		mtd_erase_callback(einfo);

	return ret;
}

static int spinand_mtd_block_isreserved(struct mtd_info *mtd, loff_t offs)
{
	struct spinand_device *spinand = mtd_to_spinand(mtd);
	struct nand_device *nand = mtd_to_nanddev(mtd);
	struct nand_pos pos;
	int ret;

	nanddev_offs_to_pos(nand, offs, &pos);
	mutex_lock(&spinand->lock);
	ret = nanddev_isreserved(nand, &pos);
	mutex_unlock(&spinand->lock);

	return ret;
}

const struct spi_mem_op *
spinand_find_supported_op(struct spinand_device *spinand,
			  const struct spi_mem_op *ops,
			  unsigned int nops)
{
	unsigned int i;

	for (i = 0; i < nops; i++) {
		if (spi_mem_supports_op(spinand->spimem, &ops[i]))
			return &ops[i];
	}

	return NULL;
}

static const struct nand_ops spinand_ops = {
	.erase = spinand_erase,
	.markbad = spinand_markbad,
	.isbad = spinand_isbad,
};

static const struct spinand_manufacturer *spinand_manufacturers[] = {
	&micron_spinand_manufacturer,
	&winbond_spinand_manufacturer,
};

static int spinand_manufacturer_detect(struct spinand_device *spinand)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(spinand_manufacturers); i++) {
		ret = spinand_manufacturers[i]->ops->detect(spinand);
		if (ret > 0) {
			spinand->manufacturer.manu = spinand_manufacturers[i];
			return 0;
		} else if (ret < 0) {
			return ret;
		}
	}

	return -ENOTSUPP;
}

static int spinand_manufacturer_init(struct spinand_device *spinand)
{
	if (spinand->manufacturer.manu->ops->init)
		return spinand->manufacturer.manu->ops->init(spinand);

	return 0;
}

static void spinand_manufacturer_cleanup(struct spinand_device *spinand)
{
	/* Release manufacturer private data */
	if (spinand->manufacturer.manu->ops->cleanup)
		return spinand->manufacturer.manu->ops->cleanup(spinand);
}

static const struct spi_mem_op *
spinand_select_op_variant(struct spinand_device *spinand,
			  const struct spinand_op_variants *variants)
{
	struct nand_device *nand = spinand_to_nand(spinand);
	unsigned int i;

	for (i = 0; i < variants->nops; i++) {
		struct spi_mem_op op = variants->ops[i];
		unsigned int nbytes;
		int ret;

		nbytes = nanddev_per_page_oobsize(nand) +
			 nanddev_page_size(nand);

		while (nbytes) {
			op.data.nbytes = nbytes;
			ret = spi_mem_adjust_op_size(spinand->spimem, &op);
			if (ret)
				break;

			if (spi_mem_supports_op(spinand->spimem, &op))
				return &variants->ops[i];
		}
	}

	return NULL;
}

int spinand_match_and_init(struct spinand_device *spinand,
			   const struct spinand_info *table,
			   unsigned int table_size, u8 devid)
{
	struct nand_device *nand = spinand_to_nand(spinand);
	unsigned int i;

	for (i = 0; i < table_size; i++) {
		const struct spinand_info *info = &table[i];
		const struct spi_mem_op *op;

		if (devid != info->devid)
			continue;

		nand->memorg = table[i].memorg;
		nand->eccreq = table[i].eccreq;

		op = spinand_select_op_variant(spinand,
					       info->op_variants.read_cache);
		if (!op)
			return -ENOTSUPP;

		spinand->op_templates.read_cache = op;

		op = spinand_select_op_variant(spinand,
					       info->op_variants.write_cache);
		if (!op)
			return -ENOTSUPP;

		spinand->op_templates.write_cache = op;

		op = spinand_select_op_variant(spinand,
					       info->op_variants.update_cache);
		spinand->op_templates.update_cache = op;

		return 0;
	}

	return -ENOTSUPP;
}


static int spinand_detect(struct spinand_device *spinand)
{
	struct nand_device *nand = &spinand->base;
	int ret;

	ret = spinand_reset_op(spinand);
	if (ret)
		return ret;

	ret = spinand_read_id_op(spinand, spinand->id.data);
	if (ret)
		return ret;

	spinand->id.len = SPINAND_MAX_ID_LEN;

	ret = spinand_manufacturer_detect(spinand);
	if (ret) {
		pr_err("unknown raw ID %*phN\n",
		       SPINAND_MAX_ID_LEN, spinand->id.data);
		return ret;
	}

	pr_info("%s SPI NAND was found.\n", spinand->manufacturer.manu->name);
	pr_info("%llu MiB, block size: %zu KiB, page size: %zu, OOB size: %u\n",
		nanddev_size(nand) >> 20, nanddev_eraseblock_size(nand) >> 10,
		nanddev_page_size(nand), nanddev_per_page_oobsize(nand));

	return 0;
}

static int spinand_init(struct spinand_device *spinand)
{
	struct mtd_info *mtd = spinand_to_mtd(spinand);
	struct nand_device *nand = mtd_to_nanddev(mtd);
	int ret;

	ret = spinand_detect(spinand);
	if (ret) {
		pr_err("Failed to detect a SPI NAND (err = %d).\n", ret);
		return ret;
	}

	ret = nanddev_init(nand, &spinand_ops, THIS_MODULE);
	if (ret)
		return ret;

	/*
	 * Use kzalloc() instead of devm_kzalloc() here, because some drivers
	 * may use this buffer for DMA access.
	 * Memory allocated by devm_ does not guarantee DMA-safe alignment.
	 */
	spinand->buf = kzalloc(nanddev_page_size(nand) +
			       nanddev_per_page_oobsize(nand),
			       GFP_KERNEL);
	if (!spinand->buf)
		return -ENOMEM;

	spinand->oobbuf = spinand->buf + nanddev_page_size(nand);

	ret = spinand_manufacturer_init(spinand);
	if (ret) {
		pr_err("Init of SPI NAND failed (err = %d)\n", ret);
		goto err_free_buf;
	}

	/*
	 * Right now, we don't support ECC, so let the whole oob
	 * area is available for user.
	 */
	mtd->_read_oob = spinand_mtd_read;
	mtd->_write_oob = spinand_mtd_write;
	mtd->_block_isbad = spinand_mtd_block_isbad;
	mtd->_block_markbad = spinand_mtd_block_markbad;
	mtd->_block_isreserved = spinand_mtd_block_isreserved;
	mtd->_erase = spinand_mtd_erase;

	/* After power up, all blocks are locked, so unlock it here. */
	spinand_lock_block(spinand, BL_ALL_UNLOCKED);
	/* Right now, we don't support ECC, so disable on-die ECC */
	spinand_disable_ecc(spinand);

	return 0;

err_free_buf:
	kfree(spinand->buf);
	return ret;
}

static void spinand_cleanup(struct spinand_device *spinand)
{
	struct nand_device *nand = &spinand->base;

	spinand_manufacturer_cleanup(spinand);
	kfree(spinand->buf);
	nanddev_cleanup(nand);
}

static int spinand_probe(struct spi_mem *mem)
{
	struct spinand_device *spinand;
	struct mtd_info *mtd;
	int ret;

	spinand = devm_kzalloc(&mem->spi->dev, sizeof(*spinand),
			       GFP_KERNEL);
	if (!spinand)
		return -ENOMEM;

	spinand->spimem = mem;
	spi_mem_set_drvdata(mem, spinand);
	spinand_set_of_node(spinand, mem->spi->dev.of_node);
	mutex_init(&spinand->lock);
	mtd = spinand_to_mtd(spinand);
	mtd->dev.parent = &mem->spi->dev;

	ret = spinand_init(spinand);
	if (ret)
		return ret;

	ret = mtd_device_register(mtd, NULL, 0);
	if (ret)
		goto err_spinand_cleanup;

	return 0;

err_spinand_cleanup:
	spinand_cleanup(spinand);

	return ret;
}

static int spinand_remove(struct spi_mem *mem)
{
	struct spinand_device *spinand;
	struct mtd_info *mtd;
	int ret;

	spinand = spi_mem_get_drvdata(mem);
	mtd = spinand_to_mtd(spinand);

	ret = mtd_device_unregister(mtd);
	if (ret)
		return ret;

	spinand_cleanup(spinand);

	return 0;
}

static const struct spi_device_id spinand_ids[] = {
	{ .name = "spi-nand" },
	{ /* sentinel */ },
};

#ifdef CONFIG_OF
static const struct of_device_id spinand_of_ids[] = {
	{ .compatible = "spi-nand" },
	{ /* sentinel */ },
};
#endif

static struct spi_mem_driver spinand_drv = {
	.spidrv = {
		.id_table = spinand_ids,
		.driver = {
			.name = "spi-nand",
			.of_match_table = of_match_ptr(spinand_of_ids),
		},
	},
	.probe = spinand_probe,
	.remove = spinand_remove,
};
module_spi_mem_driver(spinand_drv);

MODULE_DESCRIPTION("SPI NAND framework");
MODULE_AUTHOR("Peter Pan<peterpandong@micron.com>");
MODULE_LICENSE("GPL v2");
