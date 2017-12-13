// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2017 Cadence Design Systems Inc.
 *
 * Author: Boris Brezillon <boris.brezillon@free-electrons.com>
 */

#include <linux/clk.h>
#include <linux/i3c/master.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define DEV_ID				0x0
#define DEV_ID_I3C_MASTER		0x5034

#define CONF_STATUS0			0x4
#define CONF_STATUS0_ECC_CHK		BIT(28)
#define CONF_STATUS0_INTEG_CHK		BIT(27)
#define CONF_STATUS0_CSR_DAP_CHK	BIT(26)
#define CONF_STATUS0_TRANS_TOUT_CHK	BIT(25)
#define CONF_STATUS0_PROT_FAULTS_CHK	BIT(24)
#define CONF_STATUS0_GPO_NUM(x)		(((x) & GENMASK(23, 16)) >> 16)
#define CONF_STATUS0_GPI_NUM(x)		(((x) & GENMASK(15, 8)) >> 8)
#define CONF_STATUS0_SUPPORTS_DDR	BIT(5)
#define CONF_STATUS0_SEC_MASTER		BIT(4)
#define CONF_STATUS0_DEVS_NUM(x)	((x) & GENMASK(3, 0))

#define CONF_STATUS1			0x8
#define CONF_STATUS1_IBI_HW_RES(x)	((((x) & GENMASK(31, 28)) >> 28) + 1)
#define CONF_STATUS1_CMD_DEPTH(x)	(4 << (((x) & GENMASK(27, 26)) >> 26))
#define CONF_STATUS1_SLVDDR_RX_DEPTH(x)	(8 << (((x) & GENMASK(25, 21)) >> 21))
#define CONF_STATUS1_SLVDDR_TX_DEPTH(x)	(8 << (((x) & GENMASK(20, 16)) >> 16))
#define CONF_STATUS1_IBI_DEPTH(x)	(2 << (((x) & GENMASK(12, 10)) >> 10))
#define CONF_STATUS1_RX_DEPTH(x)	(8 << (((x) & GENMASK(9, 5)) >> 5))
#define CONF_STATUS1_TX_DEPTH(x)	(8 << ((x) & GENMASK(4, 0)))

#define REV_ID				0xc
#define REV_ID_VID(id)			(((id) & GENMASK(31, 20)) >> 20)
#define REV_ID_PID(id)			(((id) & GENMASK(19, 8)) >> 8)
#define REV_ID_REV_MAJOR(id)		(((id) & GENMASK(7, 4)) >> 4)
#define REV_ID_REV_MINOR(id)		((id) & GENMASK(3, 0))

#define CTRL				0x10
#define CTRL_DEV_EN			BIT(31)
#define CTRL_HALT_EN			BIT(30)
#define CTRL_HJ_DISEC			BIT(8)
#define CTRL_MST_ACK			BIT(7)
#define CTRL_HJ_ACK			BIT(6)
#define CTRL_HJ_INIT			BIT(5)
#define CTRL_MST_INIT			BIT(4)
#define CTRL_AHDR_OPT			BIT(3)
#define CTRL_PURE_BUS_MODE		0
#define CTRL_MIXED_FAST_BUS_MODE	2
#define CTRL_MIXED_SLOW_BUS_MODE	3
#define CTRL_BUS_MODE_MASK		GENMASK(1, 0)

#define PRESCL_CTRL0			0x14
#define PRESCL_CTRL0_I2C(x)		((x) << 16)
#define PRESCL_CTRL0_I3C(x)		(x)
#define PRESCL_CTRL0_MAX		GENMASK(9, 0)

#define PRESCL_CTRL1			0x18
#define PRESCL_CTRL1_PP_LOW_MASK	GENMASK(15, 8)
#define PRESCL_CTRL1_PP_LOW(x)		((x) << 8)
#define PRESCL_CTRL1_OD_LOW_MASK	GENMASK(7, 0)
#define PRESCL_CTRL1_OD_LOW(x)		(x)

#define MST_IER				0x20
#define MST_IDR				0x24
#define MST_IMR				0x28
#define MST_ICR				0x2c
#define MST_ISR				0x30
#define MST_INT_M0_ERR			BIT(28)
#define MST_INT_RX_THR			BIT(24)
#define MST_INT_TX_THR			BIT(23)
#define MST_INT_IBI_THR			BIT(22)
#define MST_INT_CMD_THR			BIT(21)
#define MST_INT_RX_UNF			BIT(20)
#define MST_INT_TX_OVF			BIT(19)
#define MST_INT_IBI_UNF			BIT(18)
#define MST_INT_CMD_OVF			BIT(17)
#define MST_INT_CMD_EMPTY		BIT(16)
#define MST_INT_MR_DONE			BIT(11)
#define MST_INT_IBI_FAIL		BIT(10)
#define MST_INT_SDR_FAIL		BIT(9)
#define MST_INT_DDR_FAIL		BIT(8)
#define MST_INT_HJ_REQ			BIT(7)
#define MST_INT_MR_REQ			BIT(6)
#define MST_INT_IBI_REQ			BIT(5)
#define MST_INT_BUS_DISCR		BIT(4)
#define MST_INT_INVALID_DA		BIT(3)
#define MST_INT_RD_ABORT		BIT(2)
#define MST_INT_NACK			BIT(1)
#define MST_INT_COMP			BIT(0)

#define MST_INT_XFER_STATUS		(MST_INT_M0_ERR |	\
					 MST_INT_SDR_FAIL |	\
					 MST_INT_DDR_FAIL | 	\
					 MST_INT_INVALID_DA |	\
					 MST_INT_RD_ABORT | 	\
					 MST_INT_NACK |		\
					 MST_INT_COMP)

#define MST_STATUS0			0x34
#define MST_STATUS0_IDLE		BIT(31)
#define MST_STATUS0_HALTED		BIT(30)
#define MST_STATUS0_MASTER_MODE		BIT(29)
#define MST_STATUS0_IMM_COMP		BIT(28)
#define MST_STATUS0_DDR_ERR_ID(s)	((s) & GENMASK(27, 25) >> 25)
#define MST_STATUS0_DAA_COMP		BIT(24)
#define MST_STATUS0_IBI_FIFO_FULL	BIT(23)
#define MST_STATUS0_RX_FIFO_FULL	BIT(22)
#define MST_STATUS0_XFER_BYTES(s)	((s) & GENMASK(21, 10) >> 10)
#define MST_STATUS0_DEV_ADDR(s)		((s) & GENMASK(9, 0))

#define SIR_STATUS			0x3c
#define SIR_STATUS_DEV(d)		BIT(d)

#define SLV_IER				0x40
#define SLV_IDR				0x44
#define SLV_IMR				0x48
#define SLV_ICR				0x4c
#define SLV_ISR				0x50
#define SLV_INT_TM			BIT(20)
#define SLV_INT_ERROR			BIT(19)
#define SLV_INT_EVENT_UP		BIT(18)
#define SLV_INT_HJ_DONE			BIT(17)
#define SLV_INT_MR_DONE			BIT(16)
#define SLV_INT_SDR_FAIL		BIT(14)
#define SLV_INT_DDR_FAIL		BIT(13)
#define SLV_INT_M_RD_ABORT		BIT(12)
#define SLV_INT_DDR_RX_THR		BIT(11)
#define SLV_INT_DDR_TX_THR		BIT(10)
#define SLV_INT_SDR_RX_THR		BIT(9)
#define SLV_INT_SDR_TX_THR		BIT(8)
#define SLV_INT_DDR_RX_UNF		BIT(7)
#define SLV_INT_DDR_TX_OVF		BIT(6)
#define SLV_INT_SDR_RX_UNF		BIT(5)
#define SLV_INT_SDR_TX_OVF		BIT(4)
#define SLV_INT_DDR_RD_COMP		BIT(3)
#define SLV_INT_DDR_WR_COMP		BIT(2)
#define SLV_INT_SDR_RD_COMP		BIT(1)
#define SLV_INT_SDR_WR_COMP		BIT(0)

#define SLV_STATUS0			0x54
#define SLV_STATUS0_REG_ADDR(s)		(((s) & GENMASK(23, 16)) >> 16)
#define SLV_STATUS0_XFRD_BYTES(s)	((s) & GENMASK(15, 0))

#define SLV_STATUS1			0x58
#define SLV_STATUS1_AS(s)		(((s) & GENMASK(21, 20)) >> 20)
#define SLV_STATUS1_VEN_TM		BIT(19)
#define SLV_STATUS1_HJ_DIS		BIT(18)
#define SLV_STATUS1_MR_DIS		BIT(17)
#define SLV_STATUS1_PROT_ERR		BIT(16)
#define SLV_STATUS1_DDR_RX_FULL		BIT(7)
#define SLV_STATUS1_DDR_TX_FULL		BIT(6)
#define SLV_STATUS1_DDR_RX_EMPTY	BIT(5)
#define SLV_STATUS1_DDR_TX_EMPTY	BIT(4)
#define SLV_STATUS1_SDR_RX_FULL		BIT(3)
#define SLV_STATUS1_SDR_TX_FULL		BIT(2)
#define SLV_STATUS1_SDR_RX_EMPTY	BIT(1)
#define SLV_STATUS1_SDR_TX_EMPTY	BIT(0)

#define CMD0_FIFO			0x60
#define CMD0_FIFO_IS_DDR		BIT(31)
#define CMD0_FIFO_IS_CCC		BIT(30)
#define CMD0_FIFO_BCH			BIT(29)
#define XMIT_BURST_STATIC_SUBADDR	0
#define XMIT_SINGLE_INC_SUBADDR		1
#define XMIT_SINGLE_STATIC_SUBADDR	2
#define XMIT_BURST_WITHOUT_SUBADDR	3
#define CMD0_FIFO_PRIV_XMIT_MODE(m)	((m) << 27)
#define CMD0_FIFO_SBCA			BIT(26)
#define CMD0_FIFO_RSBC			BIT(25)
#define CMD0_FIFO_IS_10B		BIT(24)
#define CMD0_FIFO_PL_LEN(l)		((l) << 12)
#define CMD0_FIFO_PL_LEN_MAX		4095
#define CMD0_FIFO_DEV_ADDR(a)		((a) << 1)
#define CMD0_FIFO_RNW			BIT(0)

#define CMD1_FIFO			0x64
#define CMD1_FIFO_CSRADDR(a)		(a)
#define CMD1_FIFO_CCC(id)		(id)

#define TX_FIFO				0x68

#define IMD_CMD0			0x70
#define IMD_CMD0_PL_LEN(l)		((l) << 12)
#define IMD_CMD0_DEV_ADDR(a)		((a) << 1)
#define IMD_CMD0_RNW			BIT(0)

#define IMD_CMD1			0x74
#define IMD_CMD1_CCC(id)		(id)

#define IMD_DATA			0x78
#define RX_FIFO				0x80
#define IBI_DATA_FIFO			0x84
#define SLV_DDR_TX_FIFO			0x88
#define SLV_DDR_RX_FIFO			0x8c

#define CMD_IBI_THR_CTRL		0x90
#define IBI_THR(t)			((t) << 8)
#define CMD_THR(t)			(t)

#define TX_RX_THR_CTRL			0x94
#define RX_THR(t)			((t) << 16)
#define TX_THR(t)			(t)

#define SLV_DDR_TX_RX_THR_CTRL		0x98
#define SLV_DDR_RX_THR(t)		((t) << 16)
#define SLV_DDR_TX_THR(t)		(t)

#define FLUSH_CTRL			0x9c
#define FLUSH_SLV_DDR_RX_FIFO		BIT(22)
#define FLUSH_SLV_DDR_TX_FIFO		BIT(21)
#define FLUSH_IMM_FIFO			BIT(20)
#define FLUSH_IBI_FIFO			BIT(19)
#define FLUSH_RX_FIFO			BIT(18)
#define FLUSH_TX_FIFO			BIT(17)
#define FLUSH_CMD_FIFO			BIT(16)

#define TTO_PRESCL_CTRL0		0xb0
#define TTO_PRESCL_CTRL0_DIVB(x)	((x) << 16)
#define TTO_PRESCL_CTRL0_DIVA(x)	(x)

#define TTO_PRESCL_CTRL1		0xb4
#define TTO_PRESCL_CTRL1_DIVB(x)	((x) << 16)
#define TTO_PRESCL_CTRL1_DIVA(x)	(x)

#define DEVS_CTRL			0xb8
#define DEVS_CTRL_DEV_CLR_ALL		GENMASK(31, 16)
#define DEVS_CTRL_DEV_CLR(dev)		BIT(16 + (dev))
#define DEVS_CTRL_DEV_ACTIVE(dev)	BIT(dev)

#define DEV_ID_RR0(d)			(0xc0 + ((d) * 0x10))
#define DEV_ID_RR0_LVR_EXT_ADDR		BIT(11)
#define DEV_ID_RR0_HDR_CAP		BIT(10)
#define DEV_ID_RR0_IS_I3C		BIT(9)
#define DEV_ID_RR0_SET_DEV_ADDR(a)	(((a) & GENMASK(6, 0)) |	\
					 (((a) & GENMASK(9, 7)) << 6))
#define DEV_ID_RR0_GET_DEV_ADDR(x)	((((x) >> 1) & GENMASK(6, 0)) |	\
					 (((x) >> 6) & GENMASK(9, 7)))

#define DEV_ID_RR1(d)			(0xc4 + ((d) * 0x10))
#define DEV_ID_RR1_PID_MSB(pid)		(pid)

#define DEV_ID_RR2(d)			(0xc8 + ((d) * 0x10))
#define DEV_ID_RR2_PID_LSB(pid)		((pid) << 16)
#define DEV_ID_RR2_BCR(bcr)		((bcr) << 8)
#define DEV_ID_RR2_DCR(dcr)		(dcr)
#define DEV_ID_RR2_LVR(lvr)		(lvr)

#define SIR_MAP(x)			(0x180 + ((x) * 4))
#define SIR_MAP_DEV_REG(d)		SIR_MAP((d) / 2)
#define SIR_MAP_DEV_SHIFT(d, fs)	((fs) + (((d) % 2) ? 16 : 0))
#define SIR_MAP_DEV_CONF_MASK(d)	(GENMASK(15, 0) << (((d) % 2) ? 16 : 0))
#define SIR_MAP_DEV_CONF(d, c)		((c) << (((d) % 2) ? 16 : 0))
#define DEV_ROLE_SLAVE			0
#define DEV_ROLE_MASTER			1
#define SIR_MAP_DEV_ROLE(role)		((role) << 14)
#define SIR_MAP_DEV_SLOW		BIT(13)
#define SIR_MAP_DEV_PL(l)		((l) << 8)
#define SIR_MAP_PL_MAX			GENMASK(4, 0)
#define SIR_MAP_DEV_DA(a)		((a) << 1)
#define SIR_MAP_DEV_ACK			BIT(0)

#define GPIR_WORD(x)			(0x200 + ((x) * 4))
#define GPI_REG(val, id)		\
	(((val) >> (((id) % 4) * 8)) & GENMASK(7, 0))

#define GPOR_WORD(x)			(0x220 + ((x) * 4))
#define GPO_REG(val, id)		\
	(((val) >> (((id) % 4) * 8)) & GENMASK(7, 0))

#define ASF_INT_STATUS			0x300
#define ASF_INT_RAW_STATUS		0x304
#define ASF_INT_MASK			0x308
#define ASF_INT_TEST			0x30c
#define ASF_INT_FATAL_SELECT		0x310
#define ASF_INTEGRITY_ERR		BIT(6)
#define ASF_PROTOCOL_ERR		BIT(5)
#define ASF_TRANS_TIMEOUT_ERR		BIT(4)
#define ASF_CSR_ERR			BIT(3)
#define ASF_DAP_ERR			BIT(2)
#define ASF_SRAM_UNCORR_ERR		BIT(1)
#define ASF_SRAM_CORR_ERR		BIT(0)

#define ASF_SRAM_CORR_FAULT_STATUS	0x320
#define ASF_SRAM_UNCORR_FAULT_STATUS	0x324
#define ASF_SRAM_CORR_FAULT_INSTANCE(x)	((x) >> 24)
#define ASF_SRAM_CORR_FAULT_ADDR(x)	((x) & GENMASK(23, 0))

#define ASF_SRAM_FAULT_STATS		0x328
#define ASF_SRAM_FAULT_UNCORR_STATS(x)	((x) >> 16)
#define ASF_SRAM_FAULT_CORR_STATS(x)	((x) & GENMASK(15, 0))

#define ASF_TRANS_TOUT_CTRL		0x330
#define ASF_TRANS_TOUT_EN		BIT(31)
#define ASF_TRANS_TOUT_VAL(x)	(x)

#define ASF_TRANS_TOUT_FAULT_MASK	0x334
#define ASF_TRANS_TOUT_FAULT_STATUS	0x338
#define ASF_TRANS_TOUT_FAULT_APB	BIT(3)
#define ASF_TRANS_TOUT_FAULT_SCL_LOW	BIT(2)
#define ASF_TRANS_TOUT_FAULT_SCL_HIGH	BIT(1)
#define ASF_TRANS_TOUT_FAULT_FSCL_HIGH	BIT(0)

#define ASF_PROTO_FAULT_MASK		0x340
#define ASF_PROTO_FAULT_STATUS		0x344
#define ASF_PROTO_FAULT_SLVSDR_RD_ABORT	BIT(31)
#define ASF_PROTO_FAULT_SLVDDR_FAIL	BIT(30)
#define ASF_PROTO_FAULT_S(x)		BIT(16 + (x))
#define ASF_PROTO_FAULT_MSTSDR_RD_ABORT	BIT(15)
#define ASF_PROTO_FAULT_MSTDDR_FAIL	BIT(14)
#define ASF_PROTO_FAULT_M(x)		BIT(x)

struct cdns_i3c_master_caps {
	u32 cmdfifodepth;
	u32 txfifodepth;
	u32 rxfifodepth;
};

struct cdns_i3c_cmd {
	u32 cmd0;
	u32 cmd1;
	u32 tx_len;
	const void *tx_buf;
	u32 rx_len;
	void *rx_buf;
};

struct cdns_i3c_xfer {
	struct list_head node;
	struct completion comp;
	u32 isr;
	unsigned int ncmds;
	struct cdns_i3c_cmd cmds[0];
};

struct cdns_i3c_master {
	struct work_struct hj_work;
	struct i3c_master_controller base;
	unsigned long free_dev_slots;
	struct {
		unsigned int num_slots;
		struct i3c_device **slots;
		spinlock_t lock;
	} ibi;
	struct {
		struct list_head list;
		struct cdns_i3c_xfer *cur;
		spinlock_t lock;
	} xferqueue;
	void __iomem *regs;
	struct clk *sysclk;
	struct clk *pclk;
	struct cdns_i3c_master_caps caps;
	unsigned long i3c_scl_lim;
};

static inline struct cdns_i3c_master *
to_cdns_i3c_master(struct i3c_master_controller *master)
{
	return container_of(master, struct cdns_i3c_master, base);
}

static void cdns_i3c_master_wr_to_tx_fifo(struct cdns_i3c_master *master,
					  const u8 *bytes, int nbytes)
{
	int i, j;

	for (i = 0; i < nbytes; i += 4) {
		u32 data = 0;

		for (j = 0; j < 4 && (i + j) < nbytes; j++)
			data |= (u32)bytes[i + j] << (j * 8);

		writel(data, master->regs + TX_FIFO);
	}
}

static void cdns_i3c_master_drain_rx_fifo(struct cdns_i3c_master *master)
{
	int i;

	for (i = 0; i < master->caps.rxfifodepth; i++) {
		readl(master->regs + RX_FIFO);
		if (readl(master->regs + MST_ISR) & MST_INT_RX_UNF) {
			writel(MST_INT_RX_UNF, master->regs + MST_ICR);
			break;
		}
	}
}

static void cdns_i3c_master_rd_from_rx_fifo(struct cdns_i3c_master *master,
					    u8 *bytes, int nbytes)
{
	u32 status0;
	int i, j;

	status0 = readl(master->regs + MST_STATUS0);

	if (nbytes > MST_STATUS0_XFER_BYTES(status0))
		nbytes = MST_STATUS0_XFER_BYTES(status0);

	for (i = 0; i < nbytes; i += 4) {
		u32 data;

		data = readl(master->regs + RX_FIFO);

		for (j = 0; j < 4 && (i + j) < nbytes; j++)
			bytes[i + j] = data >> (j * 8);
	}
}

static bool cdns_i3c_master_supports_ccc_cmd(struct i3c_master_controller *m,
					     const struct i3c_ccc_cmd *cmd)
{
	if (cmd->ndests > 1)
		return false;

	switch (cmd->id) {
	case I3C_CCC_ENEC(true):
	case I3C_CCC_ENEC(false):
	case I3C_CCC_DISEC(true):
	case I3C_CCC_DISEC(false):
	case I3C_CCC_ENTAS(0, true):
	case I3C_CCC_ENTAS(0, false):
	case I3C_CCC_RSTDAA(true):
	case I3C_CCC_RSTDAA(false):
	case I3C_CCC_ENTDAA:
	case I3C_CCC_SETMWL(true):
	case I3C_CCC_SETMWL(false):
	case I3C_CCC_SETMRL(true):
	case I3C_CCC_SETMRL(false):
	case I3C_CCC_DEFSLVS:
	case I3C_CCC_ENTHDR(0):
	case I3C_CCC_SETDASA:
	case I3C_CCC_SETNEWDA:
	case I3C_CCC_GETMWL:
	case I3C_CCC_GETMRL:
	case I3C_CCC_GETPID:
	case I3C_CCC_GETBCR:
	case I3C_CCC_GETDCR:
	case I3C_CCC_GETSTATUS:
	case I3C_CCC_GETACCMST:
	case I3C_CCC_GETMXDS:
	case I3C_CCC_GETHDRCAP:
		return true;
	default:
		break;
	}

	return false;
}

static int cdns_i3c_master_disable(struct cdns_i3c_master *master)
{
	u32 status;

	writel(readl(master->regs + CTRL) & ~CTRL_DEV_EN, master->regs + CTRL);

	return readl_poll_timeout(master->regs + MST_STATUS0, status,
				  status & MST_STATUS0_IDLE, 10, 1000000);
}

static void cdns_i3c_master_enable(struct cdns_i3c_master *master)
{
	writel(readl(master->regs + CTRL) | CTRL_DEV_EN, master->regs + CTRL);
}

static struct cdns_i3c_xfer *
cdns_i3c_master_alloc_xfer(struct cdns_i3c_master *master, unsigned int ncmds)
{
	struct cdns_i3c_xfer *xfer;

	xfer = kzalloc(sizeof(*xfer) + (ncmds * sizeof(*xfer->cmds)),
		       GFP_KERNEL);
	if (!xfer)
		return NULL;

	INIT_LIST_HEAD(&xfer->node);
	xfer->ncmds = ncmds;

	return xfer;
}

static void cdns_i3c_master_free_xfer(struct cdns_i3c_xfer *xfer)
{
	kfree(xfer);
}

static void cdns_i3c_master_start_xfer_locked(struct cdns_i3c_master *master)
{
	struct cdns_i3c_xfer *xfer = master->xferqueue.cur;
	unsigned int i;

	if (!xfer)
		return;

	writel(MST_INT_XFER_STATUS | MST_INT_CMD_EMPTY,
	       master->regs + MST_ICR);
	for (i = 0; i < xfer->ncmds; i++) {
		struct cdns_i3c_cmd *cmd = &xfer->cmds[i];

		cdns_i3c_master_wr_to_tx_fifo(master, cmd->tx_buf,
					      cmd->tx_len);
	}

	for (i = 0; i < xfer->ncmds; i++) {
		struct cdns_i3c_cmd *cmd = &xfer->cmds[i];

		writel(cmd->cmd1, master->regs + CMD1_FIFO);
		writel(cmd->cmd0, master->regs + CMD0_FIFO);
	}

	writel(MST_INT_CMD_EMPTY | MST_INT_RD_ABORT, master->regs + MST_IER);
}

static void cdns_i3c_master_end_xfer_locked(struct cdns_i3c_master *master,
					    u32 isr)
{
	struct cdns_i3c_xfer *xfer = master->xferqueue.cur;

	if (!xfer)
		return;

	isr &= MST_INT_XFER_STATUS | MST_INT_CMD_EMPTY;
	if (!isr)
		return;


	writel(MST_INT_CMD_EMPTY | MST_INT_RD_ABORT, master->regs + MST_IDR);
	xfer->isr = isr & ~MST_INT_CMD_EMPTY;

	if (xfer->isr & MST_INT_RD_ABORT) {
		writel(FLUSH_RX_FIFO | FLUSH_TX_FIFO | FLUSH_CMD_FIFO,
		       master->regs + FLUSH_CTRL);
		writel(readl(master->regs + CTRL) | CTRL_DEV_EN,
		       master->regs + CTRL);
	} else if (xfer->isr != MST_INT_COMP) {
		cdns_i3c_master_drain_rx_fifo(master);
	} else {
		unsigned int i;

		for (i = 0; i < xfer->ncmds; i++) {
			struct cdns_i3c_cmd *cmd = &xfer->cmds[i];

			cdns_i3c_master_rd_from_rx_fifo(master, cmd->rx_buf,
							cmd->rx_len);
		}
	}

	complete(&xfer->comp);

	xfer = list_first_entry_or_null(&master->xferqueue.list,
					struct cdns_i3c_xfer, node);
	if (xfer)
		list_del_init(&xfer->node);

	master->xferqueue.cur = xfer;
	cdns_i3c_master_start_xfer_locked(master);
}

static void cdns_i3c_master_queue_xfer(struct cdns_i3c_master *master,
				       struct cdns_i3c_xfer *xfer)
{
	unsigned long flags;

	init_completion(&xfer->comp);
	spin_lock_irqsave(&master->xferqueue.lock, flags);
	if (master->xferqueue.cur) {
		list_add_tail(&xfer->node, &master->xferqueue.list);
	} else {
		master->xferqueue.cur = xfer;
		cdns_i3c_master_start_xfer_locked(master);
	}
	spin_unlock_irqrestore(&master->xferqueue.lock, flags);
}

static void cdns_i3c_master_unqueue_xfer(struct cdns_i3c_master *master,
					 struct cdns_i3c_xfer *xfer)
{
	unsigned long flags;

	spin_lock_irqsave(&master->xferqueue.lock, flags);
	if (master->xferqueue.cur == xfer) {
		u32 status;

		writel(readl(master->regs + CTRL) & ~CTRL_DEV_EN,
		       master->regs + CTRL);
		readl_poll_timeout_atomic(master->regs + MST_STATUS0, status,
					  status & MST_STATUS0_IDLE, 10,
					  1000000);
		master->xferqueue.cur = NULL;
		writel(FLUSH_RX_FIFO | FLUSH_TX_FIFO | FLUSH_CMD_FIFO,
		       master->regs + FLUSH_CTRL);
		writel(MST_INT_CMD_EMPTY | MST_INT_RD_ABORT,
		       master->regs + MST_IDR);
		writel(readl(master->regs + CTRL) | CTRL_DEV_EN,
		       master->regs + CTRL);
	} else {
		list_del_init(&xfer->node);
	}
	spin_unlock_irqrestore(&master->xferqueue.lock, flags);
}

static int cdns_i3c_master_send_ccc_cmd(struct i3c_master_controller *m,
					struct i3c_ccc_cmd *cmd)
{
	struct cdns_i3c_master *master = to_cdns_i3c_master(m);
	struct cdns_i3c_xfer *xfer;
	struct cdns_i3c_cmd *ccmd;
	int ret;

	xfer = cdns_i3c_master_alloc_xfer(master, 1);
	if (!xfer)
		return -ENOMEM;

	ccmd = xfer->cmds;
	ccmd->cmd1 = CMD1_FIFO_CCC(cmd->id);
	ccmd->cmd0 = CMD0_FIFO_IS_CCC |
		     CMD0_FIFO_PL_LEN(cmd->dests[0].payload.len);

	if (cmd->id & I3C_CCC_DIRECT)
		ccmd->cmd0 |= CMD0_FIFO_DEV_ADDR(cmd->dests[0].addr);

	if (cmd->rnw) {
		ccmd->cmd0 |= CMD0_FIFO_RNW;
		ccmd->rx_buf = cmd->dests[0].payload.data;
		ccmd->rx_len = cmd->dests[0].payload.len;
	} else {
		ccmd->tx_buf = cmd->dests[0].payload.data;
		ccmd->tx_len = cmd->dests[0].payload.len;
	}

	cdns_i3c_master_queue_xfer(master, xfer);
	if (!wait_for_completion_timeout(&xfer->comp, msecs_to_jiffies(1000)))
		cdns_i3c_master_unqueue_xfer(master, xfer);

	/*
	 * MST_INT_NACK is not an error when doing DAA, it just means "no i3c
	 * devices on the bus".
	 */
	if (xfer->isr == MST_INT_COMP)
		ret = 0;
	else if (xfer->isr)
		ret = -EIO;
	else
		ret = -ETIMEDOUT;

	cdns_i3c_master_free_xfer(xfer);

	return ret;
}

static int cdns_i3c_master_contig_priv_xfers(struct cdns_i3c_master *master,
					     const struct i3c_priv_xfer *xfers,
					     int nxfers)
{
	struct cdns_i3c_xfer *xfer;
	unsigned int i;
	int ret;

	xfer = cdns_i3c_master_alloc_xfer(master, nxfers);
	if (!xfer)
		return -ENOMEM;

	for (i = 0; i < nxfers; i++) {
		struct cdns_i3c_cmd *ccmd = &xfer->cmds[i];
		u32 pl_len = xfers[i].len;

		ccmd->cmd0 = CMD0_FIFO_DEV_ADDR(xfers[i].addr) |
			CMD0_FIFO_PRIV_XMIT_MODE(XMIT_BURST_WITHOUT_SUBADDR);

		if (xfers[i].flags & I3C_PRIV_XFER_READ) {
			ccmd->cmd0 |= CMD0_FIFO_RNW;
			ccmd->rx_buf = xfers[i].data.in;
			ccmd->rx_len = xfers[i].len;
			pl_len++;
		} else {
			ccmd->tx_buf = xfers[i].data.out;
			ccmd->tx_len = xfers[i].len;
		}

		ccmd->cmd0 |= CMD0_FIFO_PL_LEN(pl_len);

		if (i < nxfers - 1)
			ccmd->cmd0 |= CMD0_FIFO_RSBC;

		if (!i)
			ccmd->cmd0 |= CMD0_FIFO_BCH;
	}

	cdns_i3c_master_queue_xfer(master, xfer);
	if (!wait_for_completion_timeout(&xfer->comp, msecs_to_jiffies(1000)))
		cdns_i3c_master_unqueue_xfer(master, xfer);

	if (!xfer->isr)
		ret = -ETIMEDOUT;
	else if (xfer->isr &
		 (MST_INT_NACK | MST_INT_RD_ABORT | MST_INT_INVALID_DA))
		ret = -EIO;
	else
		ret = 0;

	cdns_i3c_master_free_xfer(xfer);

	return ret;
}

static int cdns_i3c_master_priv_xfers(struct i3c_master_controller *m,
				      const struct i3c_priv_xfer *xfers,
				      int nxfers)
{
	struct cdns_i3c_master *master = to_cdns_i3c_master(m);
	int tnxfers = 0, tntx = 0, tnrx = 0, j = 0, i, ret = 0;

	for (i = 0; i < nxfers; i++) {
		if (xfers[i].len > CMD0_FIFO_PL_LEN_MAX)
			return -ENOTSUPP;
	}

	if (!nxfers)
		return 0;

	/*
	 * First make sure that all transactions (block of transfers separated
	 * by a STOP marker) fit in the FIFOs.
	 */
	for (i = 0; i < nxfers; i++) {
		tnxfers++;

		if (xfers[i].flags & I3C_PRIV_XFER_READ)
			tnrx += DIV_ROUND_UP(xfers[i].len, 4);
		else
			tntx += DIV_ROUND_UP(xfers[i].len, 4);

		if (!(xfers[i].flags & I3C_PRIV_XFER_STOP) &&
		    i < nxfers - 1)
			continue;

		if (tnxfers > master->caps.cmdfifodepth ||
		    tnrx > master->caps.rxfifodepth ||
		    tntx > master->caps.txfifodepth)
			return -ENOTSUPP;

		tnxfers = 0;
		tntx = 0;
		tnrx = 0;
	}

	for (i = 0, j = 0; i < nxfers; i++) {
		if (!(xfers[i].flags & I3C_PRIV_XFER_STOP) && i < nxfers - 1)
			continue;

		ret = cdns_i3c_master_contig_priv_xfers(master, xfers + j,
							i + 1 - j);
		if (ret)
			return ret;

		j = i;
	}

	return 0;
}

#define I3C_DDR_FIRST_DATA_WORD_PREAMBLE	0x2
#define I3C_DDR_DATA_WORD_PREAMBLE		0x3

#define I3C_DDR_PREAMBLE(p)			((p) << 18)

static u32 prepare_ddr_word(u16 payload)
{
	u32 ret;
	u16 pb;

	ret = (u32)payload << 2;

	/* Calculate parity. */
	pb = (payload >> 15) ^ (payload >> 13) ^ (payload >> 11) ^
	     (payload >> 9) ^ (payload >> 7) ^ (payload >> 5) ^
	     (payload >> 3) ^ (payload >> 1);
	ret |= (pb & 1) << 1;
	pb = (payload >> 14) ^ (payload >> 12) ^ (payload >> 10) ^
	     (payload >> 8) ^ (payload >> 6) ^ (payload >> 4) ^
	     (payload >> 2) ^ payload ^ 1;
	ret |= (pb & 1);

	return ret;
}

static u32 prepare_ddr_data_word(u16 data, bool first)
{
	return prepare_ddr_word(data) | I3C_DDR_PREAMBLE(first ? 2 : 3);
}

#define I3C_DDR_READ_CMD	BIT(15)

static u32 prepare_ddr_cmd_word(u16 cmd)
{
	return prepare_ddr_word(cmd) | I3C_DDR_PREAMBLE(1);
}

static u32 prepare_ddr_crc_word(u8 crc5)
{
	return (((u32)crc5 & 0x1f) << 9) | (0xc << 14) |
	       I3C_DDR_PREAMBLE(1);
}

static u8 update_crc5(u8 crc5, u16 word)
{
	u8 crc0;
	int i;

	/*
	 * crc0 = next_data_bit ^ crc[4]
	 *                1         2            3       4
	 * crc[4:0] = { crc[3:2], crc[1]^crc0, crc[0], crc0 }
	 */
	for (i = 0; i < 16; ++i) {
		crc0 = ((word >> (15 - i)) ^ (crc5 >> 4)) & 0x1;
		crc5 = ((crc5 << 1) & (0x18 | 0x2)) |
		       (((crc5 >> 1) ^ crc0) << 2) | crc0;
	}

	return crc5 & 0x1F;
}

static int cdns_i3c_master_send_hdr_cmd(struct i3c_master_controller *m,
					const struct i3c_hdr_cmd *cmds,
					int ncmds)
{
	struct cdns_i3c_master *master = to_cdns_i3c_master(m);
	int ret, i, ntxwords = 1, nrxwords = 0;
	struct cdns_i3c_xfer *xfer;
	struct cdns_i3c_cmd *ccmd;
	u16 cmdword, datain;
	u32 checkword, word;
	u32 *buf = NULL;
	u8 crc5;

	if (ncmds < 1)
		return 0;

	if (ncmds > 1 || cmds[0].ndatawords > CMD0_FIFO_PL_LEN_MAX)
		return -ENOTSUPP;

	if (cmds[0].mode != I3C_HDR_DDR)
		return -ENOTSUPP;

	cmdword = ((u16)cmds[0].code << 8) | (cmds[0].addr << 1);
	if (cmdword & I3C_DDR_READ_CMD)
		nrxwords += cmds[0].ndatawords + 1;
	else
		ntxwords += cmds[0].ndatawords + 1;

	if (ntxwords > master->caps.txfifodepth ||
	    nrxwords > master->caps.rxfifodepth)
		return -ENOTSUPP;

	buf = kzalloc((nrxwords + ntxwords) * sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	xfer = cdns_i3c_master_alloc_xfer(master, 2);
	if (!xfer) {
		ret = -ENOMEM;
		goto out_free_buf;
	}

	ccmd = &xfer->cmds[0];
	ccmd->cmd1 = CMD1_FIFO_CCC(I3C_CCC_ENTHDR(0));
	ccmd->cmd0 = CMD0_FIFO_IS_CCC;

	ccmd = &xfer->cmds[1];

	if (cmdword & I3C_DDR_READ_CMD) {
		u16 pb;

		pb = (cmdword >> 14) ^ (cmdword >> 12) ^ (cmdword >> 10) ^
		     (cmdword >> 8) ^ (cmdword >> 6) ^ (cmdword >> 4) ^
		     (cmdword >> 2);

		if (pb & 1)
			cmdword |= BIT(0);
	}

	ccmd->tx_len = ntxwords * sizeof(u32);
	ccmd->tx_buf = buf;
	ccmd->rx_len = nrxwords * sizeof(u32);
	ccmd->rx_buf = buf + ntxwords;

	buf[0] = prepare_ddr_cmd_word(cmdword);
	crc5 = update_crc5(0x1f, cmdword);
	for (i = 0; i < ntxwords - 2; i++) {
		crc5 = update_crc5(crc5, cmds[0].data.out[i]);
		buf[i + 1] = prepare_ddr_data_word(cmds[0].data.out[i], !i);
	}

	ccmd->cmd0 = CMD0_FIFO_IS_DDR | CMD0_FIFO_PL_LEN(ntxwords);

	cdns_i3c_master_queue_xfer(master, xfer);
	if (!wait_for_completion_timeout(&xfer->comp, msecs_to_jiffies(1000)))
		cdns_i3c_master_unqueue_xfer(master, xfer);

	if (xfer->isr == MST_INT_COMP) {
		ret = 0;
		for (i = 0; i < nrxwords; i++) {
			word = ((u32 *)ccmd->rx_buf)[i];
			datain = (word >> 2) & GENMASK(15, 0);
			if (i < nrxwords - 1) {
				checkword = prepare_ddr_data_word(datain, !i);
				word &= GENMASK(19, 0);
			} else {
				checkword = prepare_ddr_crc_word(crc5);
				word &= GENMASK(19, 7);
			}

			if (checkword != word) {
				ret = -EIO;
				break;
			}

			if (i < nrxwords - 1) {
				crc5 = update_crc5(crc5, datain);
				cmds[0].data.in[i] = datain;
			}
		}
	} else if (!xfer->isr) {
		ret = -ETIMEDOUT;
	} else {
		ret = -EIO;
	}

	cdns_i3c_master_free_xfer(xfer);

out_free_buf:
	kfree(buf);

	return ret;
}

static int cdns_i3c_master_i2c_xfers(struct i3c_master_controller *m,
				     const struct i2c_msg *xfers, int nxfers)
{
	struct cdns_i3c_master *master = to_cdns_i3c_master(m);
	unsigned int nrxwords = 0, ntxwords = 0;
	struct cdns_i3c_xfer *xfer;
	int i, ret = 0;

	if (nxfers > master->caps.cmdfifodepth)
		return -ENOTSUPP;

	for (i = 0; i < nxfers; i++) {
		if (xfers[i].len > CMD0_FIFO_PL_LEN_MAX)
			return -ENOTSUPP;

		if (xfers[i].flags & I2C_M_RD)
			nrxwords += DIV_ROUND_UP(xfers[i].len, 4);
		else
			ntxwords += DIV_ROUND_UP(xfers[i].len, 4);
	}

	if (ntxwords > master->caps.txfifodepth ||
	    nrxwords > master->caps.rxfifodepth)
		return -ENOTSUPP;

	xfer = cdns_i3c_master_alloc_xfer(master, nxfers);
	if (!xfer)
		return -ENOMEM;

	for (i = 0; i < nxfers; i++) {
		struct cdns_i3c_cmd *ccmd = &xfer->cmds[0];

		ccmd->cmd0 = CMD0_FIFO_DEV_ADDR(xfers[i].addr) |
			CMD0_FIFO_PL_LEN(xfers[i].len) |
			CMD0_FIFO_PRIV_XMIT_MODE(XMIT_BURST_WITHOUT_SUBADDR);

		if (xfers[i].flags & I2C_M_TEN)
			ccmd->cmd0 |= CMD0_FIFO_IS_10B;

		if (xfers[i].flags & I2C_M_RD) {
			ccmd->cmd0 |= CMD0_FIFO_RNW;
			ccmd->rx_buf = xfers[i].buf;
			ccmd->rx_len = xfers[i].len;
		} else {
			ccmd->tx_buf = xfers[i].buf;
			ccmd->tx_len = xfers[i].len;
		}
	}

	cdns_i3c_master_queue_xfer(master, xfer);
	if (!wait_for_completion_timeout(&xfer->comp, msecs_to_jiffies(1000)))
		cdns_i3c_master_unqueue_xfer(master, xfer);

	if (xfer->isr & MST_INT_NACK)
		ret = -EIO;
	else if (!(xfer->isr & MST_INT_COMP))
		ret = -ETIMEDOUT;

	cdns_i3c_master_free_xfer(xfer);

	return ret;
}

struct cdns_i3c_i2c_dev_data {
	u16 id;
	s16 ibi;
	struct i3c_generic_ibi_pool *ibi_pool;
};

static u32 prepare_rr0_dev_address(u32 addr)
{
	u32 ret = (addr << 1) & 0xff;

	/* RR0[7:1] = addr[6:0] */
	ret |= (addr & GENMASK(6, 0)) << 1;

	/* RR0[15:13] = addr[9:7] */
	ret |= (addr & GENMASK(9, 7)) << 6;

	/* RR0[0] = ~XOR(addr[6:0]) */
	if (!(hweight8(addr & 0x7f) & 1))
		ret |= 1;

	return ret;
}

static int cdns_i3c_master_attach_i3c_dev(struct cdns_i3c_master *master,
					  struct i3c_device *dev)
{
	struct cdns_i3c_i2c_dev_data *data;
	u32 val;

	if (!master->free_dev_slots)
		return -ENOMEM;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->id = ffs(master->free_dev_slots) - 1;
	clear_bit(data->id, &master->free_dev_slots);
	i3c_device_set_master_data(dev, data);

	if (dev->info.dyn_addr)
		val = prepare_rr0_dev_address(dev->info.dyn_addr) |
		      DEV_ID_RR0_IS_I3C;
	else
		val = prepare_rr0_dev_address(dev->info.static_addr);

	if (dev->info.dcr & I3C_BCR_HDR_CAP)
		val |= DEV_ID_RR0_HDR_CAP;

	writel(val, master->regs + DEV_ID_RR0(data->id));
	writel(DEV_ID_RR1_PID_MSB(dev->info.pid),
	       master->regs + DEV_ID_RR1(data->id));
	writel(DEV_ID_RR2_DCR(dev->info.dcr) | DEV_ID_RR2_BCR(dev->info.bcr) |
	       DEV_ID_RR2_PID_LSB(dev->info.pid),
	       master->regs + DEV_ID_RR2(data->id));
	writel(readl(master->regs + DEVS_CTRL) |
	       DEVS_CTRL_DEV_ACTIVE(data->id), master->regs + DEVS_CTRL);

	return 0;
}

static void cdns_i3c_master_detach_i3c_dev(struct cdns_i3c_master *master,
					   struct i3c_device *dev)
{
	struct cdns_i3c_i2c_dev_data *data = i3c_device_get_master_data(dev);

	if (!data)
		return;

	set_bit(data->id, &master->free_dev_slots);
	writel(readl(master->regs + DEVS_CTRL) |
	       DEVS_CTRL_DEV_CLR(data->id),
	       master->regs + DEVS_CTRL);

	i3c_device_set_master_data(dev, NULL);
	kfree(data);
}

static int cdns_i3c_master_attach_i2c_dev(struct cdns_i3c_master *master,
					  struct i2c_device *dev)
{
	struct cdns_i3c_i2c_dev_data *data;

	if (!master->free_dev_slots)
		return -ENOMEM;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->id = ffs(master->free_dev_slots) - 1;
	clear_bit(data->id, &master->free_dev_slots);
	i2c_device_set_master_data(dev, data);

	writel(prepare_rr0_dev_address(dev->info.addr) |
	       (dev->info.flags & I2C_CLIENT_TEN ? DEV_ID_RR0_LVR_EXT_ADDR : 0),
	       master->regs + DEV_ID_RR0(data->id));
	writel(dev->lvr, master->regs + DEV_ID_RR2(data->id));
	writel(readl(master->regs + DEVS_CTRL) |
	       DEVS_CTRL_DEV_ACTIVE(data->id),
	       master->regs + DEVS_CTRL);

	return 0;
}

static void cdns_i3c_master_detach_i2c_dev(struct cdns_i3c_master *master,
					   struct i2c_device *dev)
{
	struct cdns_i3c_i2c_dev_data *data = i2c_device_get_master_data(dev);

	if (!data)
		return;

	set_bit(data->id, &master->free_dev_slots);
	writel(readl(master->regs + DEVS_CTRL) |
	       DEVS_CTRL_DEV_CLR(data->id),
	       master->regs + DEVS_CTRL);

	i2c_device_set_master_data(dev, NULL);
	kfree(data);
}

static void cdns_i3c_master_bus_cleanup(struct i3c_master_controller *m)
{
	struct cdns_i3c_master *master = to_cdns_i3c_master(m);
	struct i2c_device *i2cdev;
	struct i3c_device *i3cdev;

	cdns_i3c_master_disable(master);

	i3c_bus_for_each_i2cdev(m->bus, i2cdev)
		cdns_i3c_master_detach_i2c_dev(master, i2cdev);

	i3c_bus_for_each_i3cdev(m->bus, i3cdev)
		cdns_i3c_master_detach_i3c_dev(master, i3cdev);
}

static void cdns_i3c_master_dev_rr_to_info(struct cdns_i3c_master *master,
					   unsigned int slot,
					   struct i3c_device_info *info)
{
	u32 rr;

	memset(info, 0, sizeof(*info));
	rr = readl(master->regs + DEV_ID_RR0(slot));
	info->dyn_addr = DEV_ID_RR0_GET_DEV_ADDR(rr);
	rr = readl(master->regs + DEV_ID_RR2(slot));
	info->dcr = rr;
	info->bcr = rr >> 8;
	info->pid = rr >> 16;
	info->pid |= (u64)readl(master->regs + DEV_ID_RR1(slot)) << 16;
}

static int cdns_i3c_master_do_daa_locked(struct cdns_i3c_master *master)
{
	unsigned long i3c_lim_period, pres_step, i3c_scl_lim;
	struct i3c_device_info devinfo;
	struct i3c_device *i3cdev;
	u32 prescl1, ctrl, devs;
	int ret, slot, ncycles;

	ret = i3c_master_entdaa_locked(&master->base);
	if (ret)
		return ret;

	/* Now, add discovered devices to the bus. */
	i3c_scl_lim = master->i3c_scl_lim;
	devs = readl(master->regs + DEVS_CTRL);
	for (slot = find_next_bit(&master->free_dev_slots, BITS_PER_LONG, 1);
	     slot < BITS_PER_LONG;
	     slot = find_next_bit(&master->free_dev_slots,
				  BITS_PER_LONG, slot + 1)) {
		struct cdns_i3c_i2c_dev_data *data;
		u32 rr, max_fscl = 0;
		u8 addr;

		if (!(devs & DEVS_CTRL_DEV_ACTIVE(slot)))
			continue;

		data = kzalloc(sizeof(*data), GFP_KERNEL);
		if (!data)
			return -ENOMEM;

		data->ibi = -1;
		data->id = slot;
		rr = readl(master->regs + DEV_ID_RR0(slot));
		addr = DEV_ID_RR0_GET_DEV_ADDR(rr);
		i3cdev = i3c_master_add_i3c_dev_locked(&master->base, addr);
		if (IS_ERR(i3cdev))
			return PTR_ERR(i3cdev);

		i3c_device_get_info(i3cdev, &devinfo);
		clear_bit(data->id, &master->free_dev_slots);
		i3c_device_set_master_data(i3cdev, data);

		max_fscl = max(I3C_CCC_MAX_SDR_FSCL(devinfo.max_read_ds),
			       I3C_CCC_MAX_SDR_FSCL(devinfo.max_write_ds));
		switch (max_fscl) {
		case I3C_SDR_DR_FSCL_8MHZ:
			max_fscl = 8000000;
			break;
		case I3C_SDR_DR_FSCL_6MHZ:
			max_fscl = 6000000;
			break;
		case I3C_SDR_DR_FSCL_4MHZ:
			max_fscl = 4000000;
			break;
		case I3C_SDR_DR_FSCL_2MHZ:
			max_fscl = 2000000;
			break;
		case I3C_SDR_DR_FSCL_MAX:
		default:
			max_fscl = 0;
			break;
		}

		if (max_fscl && (max_fscl < i3c_scl_lim || !i3c_scl_lim))
			i3c_scl_lim = max_fscl;
	}

	i3c_master_defslvs_locked(&master->base);

	pres_step = 1000000000 / (master->base.bus->scl_rate.i3c * 4);

	/* No bus limitation to apply, bail out. */
	if (!i3c_scl_lim ||
	    (master->i3c_scl_lim && master->i3c_scl_lim <= i3c_scl_lim))
		return 0;

	/* Configure PP_LOW to meet I3C slave limitations. */
	prescl1 = readl(master->regs + PRESCL_CTRL1) &
		  ~PRESCL_CTRL1_PP_LOW_MASK;
	ctrl = readl(master->regs + CTRL) & ~CTRL_DEV_EN;

	i3c_lim_period = DIV_ROUND_UP(1000000000, i3c_scl_lim);
	ncycles = DIV_ROUND_UP(i3c_lim_period, pres_step) - 4;
	if (ncycles < 0)
		ncycles = 0;
	prescl1 |= PRESCL_CTRL1_PP_LOW(ncycles);

	/* Disable I3C master before updating PRESCL_CTRL1. */
	ret = cdns_i3c_master_disable(master);
	if (!ret) {
		writel(prescl1, master->regs + PRESCL_CTRL1);
		master->i3c_scl_lim = i3c_scl_lim;
	}
	cdns_i3c_master_enable(master);

	return ret;
}

static int cdns_i3c_master_bus_init(struct i3c_master_controller *m)
{
	struct cdns_i3c_master *master = to_cdns_i3c_master(m);
	unsigned long pres_step, sysclk_rate, max_i2cfreq;
	u32 ctrl, prescl0, prescl1, pres, low;
	struct i3c_device_info info = { };
	struct i3c_ccc_events events;
	struct i2c_device *i2cdev;
	struct i3c_device *i3cdev;
	int ret, slot, ncycles;
	u8 last_addr = 0;

	switch (m->bus->mode) {
	case I3C_BUS_MODE_PURE:
		ctrl = CTRL_PURE_BUS_MODE;
		break;

	case I3C_BUS_MODE_MIXED_FAST:
		ctrl = CTRL_MIXED_FAST_BUS_MODE;
		break;

	case I3C_BUS_MODE_MIXED_SLOW:
		ctrl = CTRL_MIXED_SLOW_BUS_MODE;
		break;

	default:
		return -EINVAL;
	}

	sysclk_rate = clk_get_rate(master->sysclk);
	if (!sysclk_rate)
		return -EINVAL;

	pres = DIV_ROUND_UP(sysclk_rate, (m->bus->scl_rate.i3c * 4)) - 1;
	if (pres > PRESCL_CTRL0_MAX)
		return -ERANGE;

	m->bus->scl_rate.i3c = sysclk_rate / ((pres + 1) * 4);

	prescl0 = PRESCL_CTRL0_I3C(pres);

	low = ((I3C_BUS_TLOW_OD_MIN_NS * sysclk_rate) / (pres + 1)) - 2;
	prescl1 = PRESCL_CTRL1_OD_LOW(low);

	max_i2cfreq = m->bus->scl_rate.i2c;

	pres = (sysclk_rate / (max_i2cfreq * 5)) - 1;
	if (pres > PRESCL_CTRL0_MAX)
		return -ERANGE;

	m->bus->scl_rate.i2c = sysclk_rate / ((pres + 1) * 5);

	prescl0 |= PRESCL_CTRL0_I2C(pres);

	writel(DEVS_CTRL_DEV_CLR_ALL, master->regs + DEVS_CTRL);

	i3c_bus_for_each_i2cdev(m->bus, i2cdev) {
		ret = cdns_i3c_master_attach_i2c_dev(master, i2cdev);
		if (ret)
			goto err_detach_devs;
	}

	writel(prescl0, master->regs + PRESCL_CTRL0);

	/* Calculate OD and PP low. */
	pres_step = 1000000000 / (m->bus->scl_rate.i3c * 4);
	ncycles = DIV_ROUND_UP(I3C_BUS_TLOW_OD_MIN_NS, pres_step) - 2;
	if (ncycles < 0)
		ncycles = 0;
	prescl1 = PRESCL_CTRL1_OD_LOW(ncycles);
	writel(prescl1, master->regs + PRESCL_CTRL1);

	i3c_bus_for_each_i3cdev(m->bus, i3cdev) {
		ret = cdns_i3c_master_attach_i3c_dev(master, i3cdev);
		if (ret)
			goto err_detach_devs;
	}

	/* Get an address for the master. */
	ret = i3c_master_get_free_addr(m, 0);
	if (ret < 0)
		goto err_detach_devs;

	writel(prepare_rr0_dev_address(ret) | DEV_ID_RR0_IS_I3C,
	       master->regs + DEV_ID_RR0(0));

	cdns_i3c_master_dev_rr_to_info(master, 0, &info);
	if (info.bcr & I3C_BCR_HDR_CAP)
		info.hdr_cap = I3C_CCC_HDR_MODE(I3C_HDR_DDR);

	ret = i3c_master_set_info(&master->base, &info);
	if (ret)
		goto err_detach_devs;

	/* Prepare RR slots before lauching DAA. */
	for (slot = find_next_bit(&master->free_dev_slots, BITS_PER_LONG, 1);
	     slot < BITS_PER_LONG;
	     slot = find_next_bit(&master->free_dev_slots,
				  BITS_PER_LONG, slot + 1)) {
		ret = i3c_master_get_free_addr(m, last_addr + 1);
		if (ret < 0)
			goto err_disable_master;

		last_addr = ret;
		writel(prepare_rr0_dev_address(last_addr) | DEV_ID_RR0_IS_I3C,
		       master->regs + DEV_ID_RR0(slot));
		writel(0, master->regs + DEV_ID_RR1(slot));
		writel(0, master->regs + DEV_ID_RR2(slot));
	}

	/*
	 * Enable Hot-Join and when a Hot-Join request happen, disable all
	 * events coming from this device.
	 *
	 * We will issue ENTDAA afterwards from the threaded IRQ handler.
	 */
	ctrl |= CTRL_HJ_ACK | CTRL_HJ_DISEC | CTRL_HALT_EN;
	writel(ctrl, master->regs + CTRL);

	cdns_i3c_master_enable(master);

	/*
	 * Reset all dynamic addresses on the bus, because we don't know what
	 * happened before this point (the bootloader may have assigned dynamic
	 * addresses that we're not aware of).
	 */
	ret = i3c_master_rstdaa_locked(m, I3C_BROADCAST_ADDR);
	if (ret)
		goto err_disable_master;

	/* Disable all slave events (interrupts) before starting DAA. */
	events.events = I3C_CCC_EVENT_SIR | I3C_CCC_EVENT_MR |
			I3C_CCC_EVENT_HJ;
	ret = i3c_master_disec_locked(m, I3C_BROADCAST_ADDR, &events);
	if (ret)
		goto err_disable_master;

	ret = cdns_i3c_master_do_daa_locked(master);
	if (ret < 0)
		goto err_disable_master;

	/* Unmask Hot-Join and Marstership request interrupts. */
	events.events = I3C_CCC_EVENT_HJ | I3C_CCC_EVENT_MR;
	ret = i3c_master_enec_locked(m, I3C_BROADCAST_ADDR, &events);
	if (ret)
		pr_info("Failed to re-enable H-J");

	writel(MST_INT_HJ_REQ, master->regs + MST_IER);
	return 0;

err_disable_master:
	cdns_i3c_master_disable(master);

err_detach_devs:
	cdns_i3c_master_bus_cleanup(m);

	return ret;
}

static void cnds_i3c_master_demux_ibis(struct cdns_i3c_master *master)
{
	unsigned int i, sirstatus;

	writel(MST_INT_IBI_REQ, master->regs + MST_ICR);
	sirstatus = readl(master->regs + SIR_STATUS);

	/*
	 * The IBI logic is broken, and if more than one device sent an
	 * IBI, there's simply no way we can determine which one came in first.
	 * In this case, drop all IBIs and flush the FIFO.
	 */
	if (hweight32(sirstatus) > 1) {
		writel(readl(master->regs + CTRL) & ~CTRL_DEV_EN, master->regs + CTRL);
		writel(FLUSH_IBI_FIFO, master->regs + FLUSH_CTRL);
		writel(sirstatus, master->regs + SIR_STATUS);
		writel(readl(master->regs + CTRL) | CTRL_DEV_EN, master->regs + CTRL);
		return;
	}

	spin_lock(&master->ibi.lock);
	for (i = 0; i < master->ibi.num_slots; i++) {
		struct i3c_device *dev = master->ibi.slots[i];
		struct cdns_i3c_i2c_dev_data *data;
		struct i3c_ibi_slot *slot;
		unsigned int len, j;
		u8 *buf;
		u32 tmp;

		if (!(sirstatus & BIT(i)) || !dev)
			continue;

		data = i3c_device_get_master_data(dev);
		slot = i3c_generic_ibi_get_free_slot(data->ibi_pool);
		if (!slot)
			continue;

		buf = slot->data;
		for (len = 0; len < dev->ibi->max_payload_len;) {
			tmp = readl(master->regs + IBI_DATA_FIFO);
			if (readl(master->regs + MST_ISR) & MST_INT_IBI_UNF) {
				writel(MST_INT_IBI_UNF, master->regs + MST_ICR);
				break;
			}

			for (j = 0; j < 4 && len < dev->ibi->max_payload_len;
			     j++, len++)
				buf[len] = tmp >> (j * 8);
		}

		slot->len = len;
		i3c_device_queue_ibi(dev, slot);
	}

	spin_unlock(&master->ibi.lock);

	writel(FLUSH_IBI_FIFO, master->regs + FLUSH_CTRL);
	writel(sirstatus, master->regs + SIR_STATUS);
}

static irqreturn_t cdns_i3c_master_interrupt(int irq, void *data)
{
	struct cdns_i3c_master *master = data;
	u32 status;

	status = readl(master->regs + MST_ISR);
	if (!(status & readl(master->regs + MST_IMR)))
		return IRQ_NONE;

	spin_lock(&master->xferqueue.lock);
	cdns_i3c_master_end_xfer_locked(master, status);
	spin_unlock(&master->xferqueue.lock);

	if (status & MST_INT_HJ_REQ) {
		writel(MST_INT_HJ_REQ, master->regs + MST_IDR);
		queue_work(master->base.wq, &master->hj_work);
	}

	if (status & MST_INT_IBI_REQ)
		cnds_i3c_master_demux_ibis(master);

	return IRQ_HANDLED;
}

int cdns_i3c_master_disable_ibi(struct i3c_master_controller *m,
				struct i3c_device *dev)
{
	struct i3c_ccc_events events = { .events = I3C_CCC_EVENT_SIR };
	struct cdns_i3c_master *master = to_cdns_i3c_master(m);
	struct cdns_i3c_i2c_dev_data *data = i3c_device_get_master_data(dev);
	struct i3c_device_info devinfo;
	unsigned long flags;
	u32 sirmap;
	int ret;

	i3c_bus_normaluse_lock(m->bus);
	i3c_device_get_info(dev, &devinfo);
	ret = i3c_master_disec_locked(m, devinfo.dyn_addr, &events);
	if (ret)
		goto out;

	spin_lock_irqsave(&master->ibi.lock, flags);
	sirmap = readl(master->regs + SIR_MAP_DEV_REG(data->ibi));
	sirmap &= ~SIR_MAP_DEV_CONF_MASK(data->ibi);
	sirmap |= SIR_MAP_DEV_CONF(data->ibi,
				   SIR_MAP_DEV_DA(I3C_BROADCAST_ADDR));
	writel(sirmap, master->regs + SIR_MAP_DEV_REG(data->ibi));
	spin_unlock_irqrestore(&master->ibi.lock, flags);

out:
	i3c_bus_normaluse_unlock(m->bus);

	return ret;
}

int cdns_i3c_master_enable_ibi(struct i3c_master_controller *m,
			       struct i3c_device *dev)
{
	struct i3c_ccc_events events = { .events = I3C_CCC_EVENT_SIR };
	struct cdns_i3c_master *master = to_cdns_i3c_master(m);
	struct cdns_i3c_i2c_dev_data *data = i3c_device_get_master_data(dev);
	struct i3c_device_info devinfo;
	unsigned long flags;
	u32 sircfg, sirmap;
	int ret;

	i3c_bus_normaluse_lock(m->bus);
	i3c_device_get_info(dev, &devinfo);
	spin_lock_irqsave(&master->ibi.lock, flags);
	sirmap = readl(master->regs + SIR_MAP_DEV_REG(data->ibi));
	sirmap &= ~SIR_MAP_DEV_CONF_MASK(data->ibi);
	sircfg = SIR_MAP_DEV_ROLE(devinfo.bcr >> 6) |
		 SIR_MAP_DEV_DA(devinfo.dyn_addr) |
		 SIR_MAP_DEV_PL(devinfo.max_ibi_len) |
		 SIR_MAP_DEV_ACK;
	if (devinfo.bcr & I3C_BCR_MAX_DATA_SPEED_LIM)
		sircfg |= SIR_MAP_DEV_SLOW;
	sirmap |= SIR_MAP_DEV_CONF(data->ibi, sircfg);
	writel(sirmap, master->regs + SIR_MAP_DEV_REG(data->ibi));
	spin_unlock_irqrestore(&master->ibi.lock, flags);

	ret = i3c_master_enec_locked(m, devinfo.dyn_addr, &events);
	if (ret)
		goto err_reset_sircfg;
	i3c_bus_normaluse_unlock(m->bus);

	return 0;

err_reset_sircfg:
	spin_lock_irqsave(&master->ibi.lock, flags);
	sirmap = readl(master->regs + SIR_MAP_DEV_REG(data->ibi));
	sirmap &= ~SIR_MAP_DEV_CONF_MASK(data->ibi);
	sirmap |= SIR_MAP_DEV_CONF(data->ibi,
				   SIR_MAP_DEV_DA(I3C_BROADCAST_ADDR));
	writel(sirmap, master->regs + SIR_MAP_DEV_REG(data->ibi));
	spin_unlock_irqrestore(&master->ibi.lock, flags);
	i3c_bus_normaluse_unlock(m->bus);

	return ret;
}

int cdns_i3c_master_request_ibi(struct i3c_master_controller *m,
				struct i3c_device *dev,
				const struct i3c_ibi_setup *req)
{
	struct cdns_i3c_master *master = to_cdns_i3c_master(m);
	struct cdns_i3c_i2c_dev_data *data = i3c_device_get_master_data(dev);
	unsigned long flags;
	unsigned int i;

	data->ibi_pool = i3c_generic_ibi_alloc_pool(dev, req);
	if (IS_ERR(data->ibi_pool))
		return PTR_ERR(data->ibi_pool);

	spin_lock_irqsave(&master->ibi.lock, flags);
	for (i = 0; i < master->ibi.num_slots; i++) {
		if (!master->ibi.slots[i]) {
			data->ibi = i;
			master->ibi.slots[i] = dev;
			break;
		}
	}
	spin_unlock_irqrestore(&master->ibi.lock, flags);

	if (i < master->ibi.num_slots)
		return 0;

	i3c_generic_ibi_free_pool(data->ibi_pool);
	data->ibi_pool = NULL;

	return -ENOSPC;
}

static void cdns_i3c_master_free_ibi(struct i3c_master_controller *m,
				     struct i3c_device *dev)
{
	struct cdns_i3c_master *master = to_cdns_i3c_master(m);
	struct cdns_i3c_i2c_dev_data *data = i3c_device_get_master_data(dev);
	unsigned long flags;

	spin_lock_irqsave(&master->ibi.lock, flags);
	master->ibi.slots[data->ibi] = NULL;
	data->ibi = -1;
	spin_unlock_irqrestore(&master->ibi.lock, flags);

	i3c_generic_ibi_free_pool(data->ibi_pool);
}


static void cdns_i3c_master_recycle_ibi_slot(struct i3c_master_controller *m,
					     struct i3c_device *dev,
					     struct i3c_ibi_slot *slot)
{
	struct cdns_i3c_i2c_dev_data *data = i3c_device_get_master_data(dev);

	i3c_generic_ibi_recycle_slot(data->ibi_pool, slot);
}

static const struct i3c_master_controller_ops cdns_i3c_master_ops = {
	.bus_init = cdns_i3c_master_bus_init,
	.bus_cleanup = cdns_i3c_master_bus_cleanup,
	.supports_ccc_cmd = cdns_i3c_master_supports_ccc_cmd,
	.send_ccc_cmd = cdns_i3c_master_send_ccc_cmd,
	.send_hdr_cmds = cdns_i3c_master_send_hdr_cmd,
	.priv_xfers = cdns_i3c_master_priv_xfers,
	.i2c_xfers = cdns_i3c_master_i2c_xfers,
	.enable_ibi = cdns_i3c_master_enable_ibi,
	.disable_ibi = cdns_i3c_master_disable_ibi,
	.request_ibi = cdns_i3c_master_request_ibi,
	.free_ibi = cdns_i3c_master_free_ibi,
	.recycle_ibi_slot = cdns_i3c_master_recycle_ibi_slot,
};

static void cdns_i3c_master_hj(struct work_struct *work)
{
	struct cdns_i3c_master *master = container_of(work,
						      struct cdns_i3c_master,
						      hj_work);
	struct i3c_bus *bus = i3c_master_get_bus(&master->base);
	u32 status;

	status = readl(master->regs + MST_ISR);
	if (!(status & MST_INT_HJ_REQ))
		return;

	i3c_bus_maintenance_lock(bus);
	writel(MST_INT_HJ_REQ, master->regs + MST_ICR);
	cdns_i3c_master_do_daa_locked(master);
	i3c_master_register_new_i3c_devs(&master->base);
	i3c_bus_maintenance_unlock(bus);
}

static int cdns_i3c_master_probe(struct platform_device *pdev)
{
	struct cdns_i3c_master *master;
	struct resource *res;
	int ret, irq;
	u32 val;

	master = devm_kzalloc(&pdev->dev, sizeof(*master), GFP_KERNEL);
	if (!master)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	master->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(master->regs))
		return PTR_ERR(master->regs);

	master->pclk = devm_clk_get(&pdev->dev, "pclk");
	if (IS_ERR(master->pclk))
		return PTR_ERR(master->pclk);

	master->sysclk = devm_clk_get(&pdev->dev, "sysclk");
	if (IS_ERR(master->pclk))
		return PTR_ERR(master->pclk);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = clk_prepare_enable(master->pclk);
	if (ret)
		return ret;

	ret = clk_prepare_enable(master->sysclk);
	if (ret)
		goto err_disable_pclk;

	if (readl(master->regs + DEV_ID) != DEV_ID_I3C_MASTER) {
		ret = -EINVAL;
		goto err_disable_sysclk;
	}

	spin_lock_init(&master->xferqueue.lock);
	INIT_LIST_HEAD(&master->xferqueue.list);

	INIT_WORK(&master->hj_work, cdns_i3c_master_hj);
	writel(0xffffffff, master->regs + MST_IDR);
	writel(0xffffffff, master->regs + SLV_IDR);
	ret = devm_request_irq(&pdev->dev, irq, cdns_i3c_master_interrupt, 0,
			       dev_name(&pdev->dev), master);
	if (ret)
		goto err_disable_sysclk;

	platform_set_drvdata(pdev, master);

	val = readl(master->regs + CONF_STATUS0);

	/* Device ID0 is reserved to describe this master. */
	master->free_dev_slots = GENMASK(CONF_STATUS0_DEVS_NUM(val), 1);

	val = readl(master->regs + CONF_STATUS1);
	master->caps.cmdfifodepth = CONF_STATUS1_CMD_DEPTH(val);
	master->caps.rxfifodepth = CONF_STATUS1_RX_DEPTH(val);
	master->caps.txfifodepth = CONF_STATUS1_TX_DEPTH(val);

	spin_lock_init(&master->ibi.lock);
	master->ibi.num_slots = CONF_STATUS1_IBI_HW_RES(val);
	master->ibi.slots = devm_kzalloc(&pdev->dev,
					 sizeof(*master->ibi.slots) *
					 master->ibi.num_slots,
					 GFP_KERNEL);
	if (!master->ibi.slots)
		goto err_disable_sysclk;

	writel(MST_INT_IBI_REQ, master->regs + MST_IER);

	ret = i3c_master_register(&master->base, &pdev->dev,
				  &cdns_i3c_master_ops, false);
	if (ret)
		goto err_disable_sysclk;

	return 0;

err_disable_sysclk:
	clk_disable_unprepare(master->sysclk);

err_disable_pclk:
	clk_disable_unprepare(master->pclk);

	return ret;
}

static int cdns_i3c_master_remove(struct platform_device *pdev)
{
	struct cdns_i3c_master *master = platform_get_drvdata(pdev);
	int ret;

	ret = i3c_master_unregister(&master->base);
	if (ret)
		return ret;

	clk_disable_unprepare(master->sysclk);
	clk_disable_unprepare(master->pclk);

	return 0;
}

static const struct of_device_id cdns_i3c_master_of_ids[] = {
	{ .compatible = "cdns,i3c-master" },
	{ /* sentinel */ },
};

static struct platform_driver cdns_i3c_master = {
	.probe = cdns_i3c_master_probe,
	.remove = cdns_i3c_master_remove,
	.driver = {
		.name = "cdns-i3c-master",
		.of_match_table = cdns_i3c_master_of_ids,
	},
};
module_platform_driver(cdns_i3c_master);

MODULE_AUTHOR("Boris Brezillon <boris.brezillon@free-electrons.com>");
MODULE_DESCRIPTION("Cadence I3C master driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:cdns-i3c-master");
