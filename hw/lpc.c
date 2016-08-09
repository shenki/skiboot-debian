/* Copyright 2013-2014 IBM Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define pr_fmt(fmt)	"LPC: " fmt

#include <skiboot.h>
#include <xscom.h>
#include <io.h>
#include <lock.h>
#include <chip.h>
#include <lpc.h>
#include <timebase.h>
#include <errorlog.h>
#include <opal-api.h>
#include <psi.h>

//#define DBG_IRQ(fmt...) prerror(fmt)
#define DBG_IRQ(fmt...) do { } while(0)

DEFINE_LOG_ENTRY(OPAL_RC_LPC_READ, OPAL_PLATFORM_ERR_EVT, OPAL_LPC,
		 OPAL_MISC_SUBSYSTEM, OPAL_PREDICTIVE_ERR_GENERAL,
		 OPAL_NA);

DEFINE_LOG_ENTRY(OPAL_RC_LPC_WRITE, OPAL_PLATFORM_ERR_EVT, OPAL_LPC,
		 OPAL_MISC_SUBSYSTEM, OPAL_PREDICTIVE_ERR_GENERAL,
		 OPAL_NA);

DEFINE_LOG_ENTRY(OPAL_RC_LPC_SYNC, OPAL_PLATFORM_ERR_EVT, OPAL_LPC,
		 OPAL_MISC_SUBSYSTEM, OPAL_PREDICTIVE_ERR_GENERAL,
		 OPAL_NA);

#define ECCB_CTL	0 /* b0020 -> b00200 */
#define ECCB_STAT	2 /* b0022 -> b00210 */
#define ECCB_DATA	3 /* b0023 -> b00218 */

#define ECCB_CTL_MAGIC		0xd000000000000000ul
#define ECCB_CTL_DATASZ		PPC_BITMASK(4,7)
#define ECCB_CTL_READ		PPC_BIT(15)
#define ECCB_CTL_ADDRLEN	PPC_BITMASK(23,25)
#define 	ECCB_ADDRLEN_4B	0x4
#define ECCB_CTL_ADDR		PPC_BITMASK(32,63)

#define ECCB_STAT_PIB_ERR	PPC_BITMASK(0,5)
#define ECCB_STAT_RD_DATA	PPC_BITMASK(6,37)
#define ECCB_STAT_BUSY		PPC_BIT(44)
#define ECCB_STAT_ERRORS1	PPC_BITMASK(45,51)
#define ECCB_STAT_OP_DONE	PPC_BIT(52)
#define ECCB_STAT_ERRORS2	PPC_BITMASK(53,55)

#define ECCB_STAT_ERR_MASK	(ECCB_STAT_PIB_ERR | \
				 ECCB_STAT_ERRORS1 | \
				 ECCB_STAT_ERRORS2)

#define ECCB_TIMEOUT	1000000

/* OPB Master LS registers */
#define OPB_MASTER_LS_IRQ_STAT	0x50
#define OPB_MASTER_LS_IRQ_MASK	0x54
#define OPB_MASTER_LS_IRQ_POL	0x58
#define   OPB_MASTER_IRQ_LPC	       	0x00000800

/* LPC HC registers */
#define LPC_HC_FW_SEG_IDSEL	0x24
#define LPC_HC_FW_RD_ACC_SIZE	0x28
#define   LPC_HC_FW_RD_1B		0x00000000
#define   LPC_HC_FW_RD_2B		0x01000000
#define   LPC_HC_FW_RD_4B		0x02000000
#define   LPC_HC_FW_RD_16B		0x04000000
#define   LPC_HC_FW_RD_128B		0x07000000
#define LPC_HC_IRQSER_CTRL	0x30
#define   LPC_HC_IRQSER_EN		0x80000000
#define   LPC_HC_IRQSER_QMODE		0x40000000
#define   LPC_HC_IRQSER_START_MASK	0x03000000
#define   LPC_HC_IRQSER_START_4CLK	0x00000000
#define   LPC_HC_IRQSER_START_6CLK	0x01000000
#define   LPC_HC_IRQSER_START_8CLK	0x02000000
#define LPC_HC_IRQMASK		0x34	/* same bit defs as LPC_HC_IRQSTAT */
#define LPC_HC_IRQSTAT		0x38
#define   LPC_HC_IRQ_SERIRQ0		0x80000000 /* all bits down to ... */
#define   LPC_HC_IRQ_SERIRQ16		0x00008000 /* IRQ16=IOCHK#, IRQ2=SMI# */
#define   LPC_HC_IRQ_SERIRQ_ALL		0xffff8000
#define   LPC_HC_IRQ_LRESET		0x00000400
#define   LPC_HC_IRQ_SYNC_ABNORM_ERR	0x00000080
#define   LPC_HC_IRQ_SYNC_NORESP_ERR	0x00000040
#define   LPC_HC_IRQ_SYNC_NORM_ERR	0x00000020
#define   LPC_HC_IRQ_SYNC_TIMEOUT_ERR	0x00000010
#define   LPC_HC_IRQ_TARG_TAR_ERR	0x00000008
#define   LPC_HC_IRQ_BM_TAR_ERR		0x00000004
#define   LPC_HC_IRQ_BM0_REQ		0x00000002
#define   LPC_HC_IRQ_BM1_REQ		0x00000001
#define   LPC_HC_IRQ_BASE_IRQS		(		     \
	LPC_HC_IRQ_LRESET |				     \
	LPC_HC_IRQ_SYNC_ABNORM_ERR |			     \
	LPC_HC_IRQ_SYNC_NORESP_ERR |			     \
	LPC_HC_IRQ_SYNC_NORM_ERR |			     \
	LPC_HC_IRQ_SYNC_TIMEOUT_ERR |			     \
	LPC_HC_IRQ_TARG_TAR_ERR |			     \
	LPC_HC_IRQ_BM_TAR_ERR)
#define LPC_HC_ERROR_ADDRESS	0x40

#define LPC_NUM_SERIRQ		17

struct lpcm {
	uint32_t		chip_id;
	uint32_t		xbase;
	void			*mbase;
	struct lock		lock;
	uint8_t			fw_idsel;
	uint8_t			fw_rdsz;
	struct list_head	clients;
	bool			has_serirq;
	uint8_t			sirq_routes[LPC_NUM_SERIRQ];
};

struct lpc_client_entry {
	struct list_node node;
	const struct lpc_client *clt;
};

/* Default LPC bus */
static int32_t lpc_default_chip_id = -1;
static bool lpc_irqs_ready;

/*
 * These are expected to be the same on all chips and should probably
 * be read (or configured) dynamically. This is how things are configured
 * today on Tuletta.
 */
static uint32_t lpc_io_opb_base		= 0xd0010000;
static uint32_t lpc_mem_opb_base 	= 0xe0000000;
static uint32_t lpc_fw_opb_base 	= 0xf0000000;
static uint32_t lpc_reg_opb_base 	= 0xc0012000;
static uint32_t opb_master_reg_base 	= 0xc0010000;

static int64_t opb_mmio_write(struct lpcm *lpc, uint32_t addr, uint32_t data,
			      uint32_t sz)
{
	switch (sz) {
	case 1:
		out_8(lpc->mbase + addr, data);
		return OPAL_SUCCESS;
	case 2:
		out_be16(lpc->mbase + addr, data);
		return OPAL_SUCCESS;
	case 4:
		out_be32(lpc->mbase + addr, data);
		return OPAL_SUCCESS;
	}
	prerror("LPC: Invalid data size %d\n", sz);
	return OPAL_PARAMETER;
}

static int64_t opb_write(struct lpcm *lpc, uint32_t addr, uint32_t data,
			 uint32_t sz)
{
	uint64_t ctl = ECCB_CTL_MAGIC, stat;
	int64_t rc, tout;
	uint64_t data_reg;

	if (lpc->mbase)
		return opb_mmio_write(lpc, addr, data, sz);

	switch(sz) {
	case 1:
		data_reg = ((uint64_t)data) << 56;
		break;
	case 2:
		data_reg = ((uint64_t)data) << 48;
		break;
	case 4:
		data_reg = ((uint64_t)data) << 32;
		break;
	default:
		prerror("Invalid data size %d\n", sz);
		return OPAL_PARAMETER;
	}

	rc = xscom_write(lpc->chip_id, lpc->xbase + ECCB_DATA, data_reg);
	if (rc) {
		log_simple_error(&e_info(OPAL_RC_LPC_WRITE),
			"LPC: XSCOM write to ECCB DATA error %lld\n", rc);
		return rc;
	}

	ctl = SETFIELD(ECCB_CTL_DATASZ, ctl, sz);
	ctl = SETFIELD(ECCB_CTL_ADDRLEN, ctl, ECCB_ADDRLEN_4B);
	ctl = SETFIELD(ECCB_CTL_ADDR, ctl, addr);
	rc = xscom_write(lpc->chip_id, lpc->xbase + ECCB_CTL, ctl);
	if (rc) {
		log_simple_error(&e_info(OPAL_RC_LPC_WRITE),
			"LPC: XSCOM write to ECCB CTL error %lld\n", rc);
		return rc;
	}

	for (tout = 0; tout < ECCB_TIMEOUT; tout++) {
		rc = xscom_read(lpc->chip_id, lpc->xbase + ECCB_STAT,
				&stat);
		if (rc) {
			log_simple_error(&e_info(OPAL_RC_LPC_WRITE),
				"LPC: XSCOM read from ECCB STAT err %lld\n",
									rc);
			return rc;
		}
		if (stat & ECCB_STAT_OP_DONE) {
			if (stat & ECCB_STAT_ERR_MASK) {
				log_simple_error(&e_info(OPAL_RC_LPC_WRITE),
					"LPC: Error status: 0x%llx\n", stat);
				return OPAL_HARDWARE;
			}
			return OPAL_SUCCESS;
		}
		time_wait_nopoll(100);
	}
	log_simple_error(&e_info(OPAL_RC_LPC_WRITE), "LPC: Write timeout !\n");
	return OPAL_HARDWARE;
}

static int64_t opb_mmio_read(struct lpcm *lpc, uint32_t addr, uint32_t *data,
			     uint32_t sz)
{
	switch (sz) {
	case 1:
		*data = in_8(lpc->mbase + addr);
		return OPAL_SUCCESS;
	case 2:
		*data = in_be16(lpc->mbase + addr);
		return OPAL_SUCCESS;
	case 4:
		*data = in_be32(lpc->mbase + addr);
		return OPAL_SUCCESS;
	}
	prerror("LPC: Invalid data size %d\n", sz);
	return OPAL_PARAMETER;
}

static int64_t opb_read(struct lpcm *lpc, uint32_t addr, uint32_t *data,
		        uint32_t sz)
{
	uint64_t ctl = ECCB_CTL_MAGIC | ECCB_CTL_READ, stat;
	int64_t rc, tout;

	if (lpc->mbase)
		return opb_mmio_read(lpc, addr, data, sz);

	if (sz != 1 && sz != 2 && sz != 4) {
		prerror("Invalid data size %d\n", sz);
		return OPAL_PARAMETER;
	}

	ctl = SETFIELD(ECCB_CTL_DATASZ, ctl, sz);
	ctl = SETFIELD(ECCB_CTL_ADDRLEN, ctl, ECCB_ADDRLEN_4B);
	ctl = SETFIELD(ECCB_CTL_ADDR, ctl, addr);
	rc = xscom_write(lpc->chip_id, lpc->xbase + ECCB_CTL, ctl);
	if (rc) {
		log_simple_error(&e_info(OPAL_RC_LPC_READ),
			"LPC: XSCOM write to ECCB CTL error %lld\n", rc);
		return rc;
	}

	for (tout = 0; tout < ECCB_TIMEOUT; tout++) {
		rc = xscom_read(lpc->chip_id, lpc->xbase + ECCB_STAT,
				&stat);
		if (rc) {
			log_simple_error(&e_info(OPAL_RC_LPC_READ),
				"LPC: XSCOM read from ECCB STAT err %lld\n",
									rc);
			return rc;
		}
		if (stat & ECCB_STAT_OP_DONE) {
			uint32_t rdata = GETFIELD(ECCB_STAT_RD_DATA, stat);
			if (stat & ECCB_STAT_ERR_MASK) {
				log_simple_error(&e_info(OPAL_RC_LPC_READ),
					"LPC: Error status: 0x%llx\n", stat);
				return OPAL_HARDWARE;
			}
			switch(sz) {
			case 1:
				*data = rdata >> 24;
				break;
			case 2:
				*data = rdata >> 16;
				break;
			default:
				*data = rdata;
				break;
			}
			return 0;
		}
		time_wait_nopoll(100);
	}
	log_simple_error(&e_info(OPAL_RC_LPC_READ), "LPC: Read timeout !\n");
	return OPAL_HARDWARE;
}

static int64_t lpc_set_fw_idsel(struct lpcm *lpc, uint8_t idsel)
{
	uint32_t val;
	int64_t rc;

	if (idsel == lpc->fw_idsel)
		return OPAL_SUCCESS;
	if (idsel > 0xf)
		return OPAL_PARAMETER;

	rc = opb_read(lpc, lpc_reg_opb_base + LPC_HC_FW_SEG_IDSEL,
		      &val, 4);
	if (rc) {
		prerror("Failed to read HC_FW_SEG_IDSEL register !\n");
		return rc;
	}
	val = (val & 0xfffffff0) | idsel;
	rc = opb_write(lpc, lpc_reg_opb_base + LPC_HC_FW_SEG_IDSEL,
		       val, 4);
	if (rc) {
		prerror("Failed to write HC_FW_SEG_IDSEL register !\n");
		return rc;
	}
	lpc->fw_idsel = idsel;
	return OPAL_SUCCESS;
}

static int64_t lpc_set_fw_rdsz(struct lpcm *lpc, uint8_t rdsz)
{
	uint32_t val;
	int64_t rc;

	if (rdsz == lpc->fw_rdsz)
		return OPAL_SUCCESS;
	switch(rdsz) {
	case 1:
		val = LPC_HC_FW_RD_1B;
		break;
	case 2:
		val = LPC_HC_FW_RD_2B;
		break;
	case 4:
		val = LPC_HC_FW_RD_4B;
		break;
	default:
		/*
		 * The HW supports 16 and 128 via a buffer/cache
		 * but I have never exprimented with it and am not
		 * sure it works the way we expect so let's leave it
		 * at that for now
		 */
		return OPAL_PARAMETER;
	}
	rc = opb_write(lpc, lpc_reg_opb_base + LPC_HC_FW_RD_ACC_SIZE,
		       val, 4);
	if (rc) {
		prerror("Failed to write LPC_HC_FW_RD_ACC_SIZE !\n");
		return rc;
	}
	lpc->fw_rdsz = rdsz;
	return OPAL_SUCCESS;
}

static int64_t lpc_opb_prepare(struct lpcm *lpc,
			       enum OpalLPCAddressType addr_type,
			       uint32_t addr, uint32_t sz,
			       uint32_t *opb_base, bool is_write)
{
	uint32_t top = addr + sz;
	uint8_t fw_idsel;
	int64_t rc;

	/* Address wraparound */
	if (top < addr)
		return OPAL_PARAMETER;

	/*
	 * Bound check access and get the OPB base address for
	 * the window corresponding to the access type
	 */
	switch(addr_type) {
	case OPAL_LPC_IO:
		/* IO space is 64K */
		if (top > 0x10000)
			return OPAL_PARAMETER;
		/* And only supports byte accesses */
		if (sz != 1)
			return OPAL_PARAMETER;
		*opb_base = lpc_io_opb_base;
		break;
	case OPAL_LPC_MEM:
		/* MEM space is 256M */
		if (top > 0x10000000)
			return OPAL_PARAMETER;
		/* And only supports byte accesses */
		if (sz != 1)
			return OPAL_PARAMETER;
		*opb_base = lpc_mem_opb_base;
		break;
	case OPAL_LPC_FW:
		/*
		 * FW space is in segments of 256M controlled
		 * by IDSEL, make sure we don't cross segments
		 */
		*opb_base = lpc_fw_opb_base;
		fw_idsel = (addr >> 28);
		if (((top - 1) >> 28) != fw_idsel)
			return OPAL_PARAMETER;

		/* Set segment */
		rc = lpc_set_fw_idsel(lpc, fw_idsel);
		if (rc)
			return rc;
		/* Set read access size */
		if (!is_write) {
			rc = lpc_set_fw_rdsz(lpc, sz);
			if (rc)
				return rc;
		}
		break;
	default:
		return OPAL_PARAMETER;
	}
	return OPAL_SUCCESS;
}

static int64_t __lpc_write(struct lpcm *lpc, enum OpalLPCAddressType addr_type,
			   uint32_t addr, uint32_t data, uint32_t sz)
{
	uint32_t opb_base;
	int64_t rc;

	lock(&lpc->lock);

	/*
	 * Convert to an OPB access and handle LPC HC configuration
	 * for FW accesses (IDSEL)
	 */
	rc = lpc_opb_prepare(lpc, addr_type, addr, sz, &opb_base, true);
	if (rc)
		goto bail;

	/* Perform OPB access */
	rc = opb_write(lpc, opb_base + addr, data, sz);

	/* XXX Add LPC error handling/recovery */
 bail:
	unlock(&lpc->lock);
	return rc;
}

int64_t lpc_write(enum OpalLPCAddressType addr_type, uint32_t addr,
		  uint32_t data, uint32_t sz)
{
	struct proc_chip *chip;

	if (lpc_default_chip_id < 0)
		return OPAL_PARAMETER;
	chip = get_chip(lpc_default_chip_id);
	if (!chip || !chip->lpc)
		return OPAL_PARAMETER;
	return __lpc_write(chip->lpc, addr_type, addr, data, sz);
}

/*
 * The "OPAL" variant add the emulation of 2 and 4 byte accesses using
 * byte accesses for IO and MEM space in order to be compatible with
 * existing Linux expectations
 */
static int64_t opal_lpc_write(uint32_t chip_id, enum OpalLPCAddressType addr_type,
			      uint32_t addr, uint32_t data, uint32_t sz)
{
	struct proc_chip *chip;
	int64_t rc;

	chip = get_chip(chip_id);
	if (!chip || !chip->lpc)
		return OPAL_PARAMETER;

	if (addr_type == OPAL_LPC_FW || sz == 1)
		return __lpc_write(chip->lpc, addr_type, addr, data, sz);
	while(sz--) {
		rc = __lpc_write(chip->lpc, addr_type, addr, data & 0xff, 1);
		if (rc)
			return rc;
		addr++;
		data >>= 8;
	}
	return OPAL_SUCCESS;
}

static int64_t __lpc_read(struct lpcm *lpc, enum OpalLPCAddressType addr_type,
			  uint32_t addr, uint32_t *data, uint32_t sz)
{
	uint32_t opb_base;
	int64_t rc;

	lock(&lpc->lock);

	/*
	 * Convert to an OPB access and handle LPC HC configuration
	 * for FW accesses (IDSEL and read size)
	 */
	rc = lpc_opb_prepare(lpc, addr_type, addr, sz, &opb_base, false);
	if (rc)
		goto bail;

	/* Perform OPB access */
	rc = opb_read(lpc, opb_base + addr, data, sz);

	/* XXX Add LPC error handling/recovery */
 bail:
	unlock(&lpc->lock);
	return rc;
}

int64_t lpc_read(enum OpalLPCAddressType addr_type, uint32_t addr,
		 uint32_t *data, uint32_t sz)
{
	struct proc_chip *chip;

	if (lpc_default_chip_id < 0)
		return OPAL_PARAMETER;
	chip = get_chip(lpc_default_chip_id);
	if (!chip || !chip->lpc)
		return OPAL_PARAMETER;
	return __lpc_read(chip->lpc, addr_type, addr, data, sz);
}

/*
 * The "OPAL" variant add the emulation of 2 and 4 byte accesses using
 * byte accesses for IO and MEM space in order to be compatible with
 * existing Linux expectations
 */
static int64_t opal_lpc_read(uint32_t chip_id, enum OpalLPCAddressType addr_type,
			     uint32_t addr, uint32_t *data, uint32_t sz)
{
	struct proc_chip *chip;
	int64_t rc;

	chip = get_chip(chip_id);
	if (!chip || !chip->lpc)
		return OPAL_PARAMETER;

	if (addr_type == OPAL_LPC_FW || sz == 1)
		return __lpc_read(chip->lpc, addr_type, addr, data, sz);
	*data = 0;
	while(sz--) {
		uint32_t byte;

		rc = __lpc_read(chip->lpc, addr_type, addr, &byte, 1);
		if (rc)
			return rc;
		*data = *data | (byte << (8 * sz));
		addr++;
	}
	return OPAL_SUCCESS;
}

bool lpc_present(void)
{
	return lpc_default_chip_id >= 0;
}

/* Called with LPC lock held */
static void lpc_setup_serirq(struct lpcm *lpc)
{
	struct lpc_client_entry *ent;
	uint32_t mask = LPC_HC_IRQ_BASE_IRQS;
	int rc;

	if (!lpc_irqs_ready)
		return;

	/* Collect serirq enable bits */
	list_for_each(&lpc->clients, ent, node)
		mask |= ent->clt->interrupts & LPC_HC_IRQ_SERIRQ_ALL;

	rc = opb_write(lpc, lpc_reg_opb_base + LPC_HC_IRQMASK, mask, 4);
	if (rc) {
		prerror("Failed to update irq mask\n");
		return;
	}
	DBG_IRQ("LPC: IRQ mask set to 0x%08x\n", mask);

	/* Enable the LPC interrupt in the OPB Master */
	opb_write(lpc, opb_master_reg_base + OPB_MASTER_LS_IRQ_POL, 0, 4);
	rc = opb_write(lpc, opb_master_reg_base + OPB_MASTER_LS_IRQ_MASK,
		       OPB_MASTER_IRQ_LPC, 4);
	if (rc)
		prerror("Failed to enable IRQs in OPB\n");

	/* Check whether we should enable serirq */
	if (mask & LPC_HC_IRQ_SERIRQ_ALL) {
		rc = opb_write(lpc, lpc_reg_opb_base + LPC_HC_IRQSER_CTRL,
			       LPC_HC_IRQSER_EN | LPC_HC_IRQSER_START_4CLK, 4);
		DBG_IRQ("LPC: SerIRQ enabled\n");
	} else {
		rc = opb_write(lpc, lpc_reg_opb_base + LPC_HC_IRQSER_CTRL,
			       0, 4);
		DBG_IRQ("LPC: SerIRQ disabled\n");
	}
	if (rc)
		prerror("Failed to configure SerIRQ\n");
	{
		u32 val;
		rc = opb_read(lpc, lpc_reg_opb_base + LPC_HC_IRQMASK, &val, 4);
		if (rc)
			prerror("Failed to readback mask");
		else
			DBG_IRQ("LPC: MASK READBACK=%x\n", val);

		rc = opb_read(lpc, lpc_reg_opb_base + LPC_HC_IRQSER_CTRL,
			      &val, 4);
		if (rc)
			prerror("Failed to readback ctrl");
		else
			DBG_IRQ("LPC: CTRL READBACK=%x\n", val);
	}
}

static void __lpc_route_serirq(struct lpcm *lpc, uint32_t sirq,
			       uint32_t psi_idx)
{
	uint32_t reg, shift, val;
	int64_t rc;

	lpc->sirq_routes[sirq] = psi_idx;

	/* We may not be ready yet ... */
	if (!lpc->has_serirq)
		return;

	if (sirq < 14) {
		reg = 0xc;
		shift = 4 + (sirq << 1);
	} else {
		reg = 0x8;
		shift = 8 + ((sirq - 14) << 1);
	}
	shift = 30-shift;
	rc = opb_read(lpc, opb_master_reg_base + reg, &val, 4);
	if (rc)
		return;
	val = val & ~(3 << shift);
	val |= (psi_idx & 3) << shift;
	opb_write(lpc, opb_master_reg_base + reg, val, 4);
}

void lpc_route_serirq(uint32_t chip_id, uint32_t sirq, uint32_t psi_idx)
{
	struct proc_chip *chip;
	struct lpcm *lpc;

	if (sirq >= LPC_NUM_SERIRQ) {
		prerror("LPC[%03x]: Routing request for invalid SerIRQ %d\n",
			chip_id, sirq);
		return;
	}

	chip = get_chip(chip_id);
	if (!chip || !chip->lpc)
		return;
	lpc = chip->lpc;
	lock(&lpc->lock);
	__lpc_route_serirq(lpc, sirq, psi_idx);
	unlock(&lpc->lock);
}

static void lpc_init_interrupts_one(struct proc_chip *chip)
{
	struct lpcm *lpc = chip->lpc;
	int i, rc;

	lock(&lpc->lock);

	/* First mask them all */
	rc = opb_write(lpc, lpc_reg_opb_base + LPC_HC_IRQMASK, 0, 4);
	if (rc) {
		prerror("LPC: Failed to init interrutps\n");
		goto bail;
	}

	switch(chip->type) {
	case PROC_CHIP_P8_MURANO:
	case PROC_CHIP_P8_VENICE:
		/* On Murano/Venice, there is no SerIRQ, only enable error
		 * interrupts
		 */
		rc = opb_write(lpc, lpc_reg_opb_base + LPC_HC_IRQMASK,
			       LPC_HC_IRQ_BASE_IRQS, 4);
		if (rc) {
			prerror("LPC: Failed to set interrupt mask\n");
			goto bail;
		}
		opb_write(lpc, lpc_reg_opb_base + LPC_HC_IRQSER_CTRL, 0, 4);
		break;
	case PROC_CHIP_P8_NAPLES:
		/* On Naples, we support LPC interrupts, enable them based
		 * on what clients requests. This will setup the mask and
		 * enable processing
		 */
		lpc->has_serirq = true;
		lpc_setup_serirq(lpc);
		break;
	case PROC_CHIP_P9_NIMBUS:
	case PROC_CHIP_P9_CUMULUS:
		/* On P9, we additionall setup the routing */
		lpc->has_serirq = true;
		for (i = 0; i < LPC_NUM_SERIRQ; i++)
			__lpc_route_serirq(lpc, i, lpc->sirq_routes[i]);
		lpc_setup_serirq(lpc);
		break;
	default:
		;
	}
 bail:
	unlock(&lpc->lock);
}

void lpc_init_interrupts(void)
{
	struct proc_chip *chip;

	lpc_irqs_ready = true;

	for_each_chip(chip) {
		if (chip->lpc)
			lpc_init_interrupts_one(chip);
	}
}

static void lpc_dispatch_reset(struct lpcm *lpc)
{
	struct lpc_client_entry *ent;

	/* XXX We are going to hit this repeatedly while reset is
	 * asserted which might be sub-optimal. We should instead
	 * detect assertion and start a poller that will wait for
	 * de-assertion. We could notify clients of LPC being
	 * on/off rather than just reset
	 */

	prerror("LPC: Got LPC reset on chip 0x%x !\n", lpc->chip_id);

	/* Collect serirq enable bits */
	list_for_each(&lpc->clients, ent, node) {
		if (!ent->clt->reset)
			continue;
		unlock(&lpc->lock);
		ent->clt->reset(lpc->chip_id);
		lock(&lpc->lock);
	}

	/* Reconfigure serial interrupts */
	if (lpc->has_serirq)
		lpc_setup_serirq(lpc);
}

static void lpc_dispatch_err_irqs(struct lpcm *lpc, uint32_t irqs)
{
	const char *sync_err = "Unknown LPC error";
	uint32_t err_addr;
	int rc;

	/* Write back to clear error interrupts, we clear SerIRQ later
	 * as they are handled as level interrupts
	 */
	rc = opb_write(lpc, lpc_reg_opb_base + LPC_HC_IRQSTAT,
		       LPC_HC_IRQ_BASE_IRQS, 4);
	if (rc)
		prerror("Failed to clear IRQ error latches !\n");

	if (irqs & LPC_HC_IRQ_LRESET)
		lpc_dispatch_reset(lpc);
	if (irqs & LPC_HC_IRQ_SYNC_ABNORM_ERR)
		sync_err = "Got SYNC abnormal error.";
	if (irqs & LPC_HC_IRQ_SYNC_NORESP_ERR)
		sync_err = "Got SYNC no-response error.";
	if (irqs & LPC_HC_IRQ_SYNC_NORM_ERR)
		sync_err = "Got SYNC normal error.";
	if (irqs & LPC_HC_IRQ_SYNC_TIMEOUT_ERR)
		sync_err = "Got SYNC timeout error.";
	if (irqs & LPC_HC_IRQ_TARG_TAR_ERR)
		sync_err = "Got abnormal TAR error.";
	if (irqs & LPC_HC_IRQ_BM_TAR_ERR)
		sync_err = "Got bus master TAR error.";

	rc = opb_read(lpc, lpc_reg_opb_base + LPC_HC_ERROR_ADDRESS,
		      &err_addr, 4);
	if (rc)
		log_simple_error(&e_info(OPAL_RC_LPC_SYNC), "LPC[%03x]: %s "
				 "Error reading error address register\n",
				 lpc->chip_id, sync_err);
	else
		log_simple_error(&e_info(OPAL_RC_LPC_SYNC), "LPC[%03x]: %s "
			"Error address reg: 0x%08x\n",
				 lpc->chip_id, sync_err, err_addr);
}

static void lpc_dispatch_ser_irqs(struct lpcm *lpc, uint32_t irqs,
				  bool clear_latch)
{
	struct lpc_client_entry *ent;
	uint32_t cirqs;
	int rc;

	irqs &= LPC_HC_IRQ_SERIRQ_ALL;

	/* Collect serirq enable bits */
	list_for_each(&lpc->clients, ent, node) {
		if (!ent->clt->interrupt)
			continue;
		cirqs = ent->clt->interrupts & irqs;
		if (cirqs) {
			unlock(&lpc->lock);
			ent->clt->interrupt(lpc->chip_id, cirqs);
			lock(&lpc->lock);
		}
	}

	/* Our SerIRQ are level sensitive, we clear the latch after
	 * we call the handler.
	 */
	if (!clear_latch)
		return;

	rc = opb_write(lpc, lpc_reg_opb_base + LPC_HC_IRQSTAT, irqs, 4);
	if (rc)
		prerror("Failed to clear SerIRQ latches !\n");
}

void lpc_interrupt(uint32_t chip_id)
{
	struct proc_chip *chip = get_chip(chip_id);
	struct lpcm *lpc;
	uint32_t irqs, opb_irqs;
	int rc;

	/* No initialized LPC controller on that chip */
	if (!chip || !chip->lpc)
		return;
	lpc = chip->lpc;

	lock(&lpc->lock);

	/* Grab OPB Master LS interrupt status */
	rc = opb_read(lpc, opb_master_reg_base + OPB_MASTER_LS_IRQ_STAT,
		      &opb_irqs, 4);
	if (rc) {
		prerror("Failed to read OPB IRQ state\n");
		unlock(&lpc->lock);
		return;
	}

	DBG_IRQ("LPC: OPB IRQ on chip 0x%x, oirqs=0x%08x\n", chip_id, opb_irqs);

	/* Check if it's an LPC interrupt */
	if (!(opb_irqs & OPB_MASTER_IRQ_LPC)) {
		/* Something we don't support ? Ack it anyway... */
		goto bail;
	}

	/* Handle the lpc interrupt source (errors etc...) */
	rc = opb_read(lpc, lpc_reg_opb_base + LPC_HC_IRQSTAT, &irqs, 4);
	if (rc) {
		prerror("Failed to read LPC IRQ state\n");
		goto bail;
	}

	DBG_IRQ("LPC: LPC IRQ on chip 0x%x, irqs=0x%08x\n", chip_id, irqs);

	/* Handle error interrupts */
	if (irqs & LPC_HC_IRQ_BASE_IRQS)
		lpc_dispatch_err_irqs(lpc, irqs);

	/* Handle SerIRQ interrupts */
	if (irqs & LPC_HC_IRQ_SERIRQ_ALL)
		lpc_dispatch_ser_irqs(lpc, irqs, true);
 bail:
	/* Ack it at the OPB level */
	opb_write(lpc, opb_master_reg_base + OPB_MASTER_LS_IRQ_STAT,
		  opb_irqs, 4);
	unlock(&lpc->lock);
}

void lpc_serirq(uint32_t chip_id, uint32_t index __unused)
{
	struct proc_chip *chip = get_chip(chip_id);
	struct lpcm *lpc;
	uint32_t irqs;
	int rc;

	/* No initialized LPC controller on that chip */
	if (!chip || !chip->lpc)
		return;
	lpc = chip->lpc;

	lock(&lpc->lock);

	/* Handle the lpc interrupt source (errors etc...) */
	rc = opb_read(lpc, lpc_reg_opb_base + LPC_HC_IRQSTAT, &irqs, 4);
	if (rc) {
		prerror("LPC: Failed to read LPC IRQ state\n");
		goto bail;
	}

	DBG_IRQ("LPC: IRQ on chip 0x%x, irqs=0x%08x\n", chip_id, irqs);

	/* Handle SerIRQ interrupts */
	if (irqs & LPC_HC_IRQ_SERIRQ_ALL)
		lpc_dispatch_ser_irqs(lpc, irqs, true);

 bail:
	unlock(&lpc->lock);
}

void lpc_all_interrupts(uint32_t chip_id)
{
	struct proc_chip *chip = get_chip(chip_id);
	struct lpcm *lpc;

	/* No initialized LPC controller on that chip */
	if (!chip || !chip->lpc)
		return;
	lpc = chip->lpc;

	/* Dispatch all */
	lock(&lpc->lock);
	lpc_dispatch_ser_irqs(lpc, LPC_HC_IRQ_SERIRQ_ALL, false);
	unlock(&lpc->lock);
}

static void lpc_init_chip_p8(struct dt_node *xn)
 {
	uint32_t gcid = dt_get_chip_id(xn);
	struct proc_chip *chip;
	struct lpcm *lpc;

	chip = get_chip(gcid);
	assert(chip);

	lpc = zalloc(sizeof(struct lpcm));
	assert(lpc);
	lpc->chip_id = gcid;
	lpc->xbase = dt_get_address(xn, 0, NULL);
	lpc->fw_idsel = 0xff;
	lpc->fw_rdsz = 0xff;
	list_head_init(&lpc->clients);
	init_lock(&lpc->lock);

	if (lpc_default_chip_id < 0 ||
	    dt_has_node_property(xn, "primary", NULL)) {
		lpc_default_chip_id = gcid;
	}

	/* Mask all interrupts for now */
	opb_write(lpc, lpc_reg_opb_base + LPC_HC_IRQMASK, 0, 4);

	printf("LPC[%03x]: Initialized, access via XSCOM @0x%x\n",
	       gcid, lpc->xbase);

	dt_add_property(xn, "interrupt-controller", NULL, 0);
	dt_add_property_cells(xn, "#interrupt-cells", 1);
	assert(dt_prop_get_u32(xn, "#address-cells") == 2);

	chip->lpc = lpc;
}

static void lpc_parse_interrupt_map(struct lpcm *lpc, struct dt_node *lpc_node)
{
	const u32 *imap;
	size_t imap_size;

	imap = dt_prop_get_def_size(lpc_node, "interrupt-map", NULL, &imap_size);
	if (!imap)
		return;
	imap_size >>= 2;
	if (imap_size % 5) {
		prerror("LPC[%03x]: Odd format for LPC interrupt-map !\n",
			lpc->chip_id);
		return;
	}

	while(imap_size >= 5) {
		uint32_t sirq = be32_to_cpu(imap[2]);
		uint32_t pirq = be32_to_cpu(imap[4]);

		if (sirq >= LPC_NUM_SERIRQ) {
			prerror("LPC[%03x]: LPC irq %d out of range in"
				" interrupt-map\n", lpc->chip_id, sirq);
		} else if (pirq < P9_PSI_IRQ_LPC_SIRQ0 ||
			   pirq > P9_PSI_IRQ_LPC_SIRQ3) {
			prerror("LPC[%03x]: PSI irq %d out of range in"
				" interrupt-map\n", lpc->chip_id, pirq);
		} else {
			uint32_t pin = pirq - P9_PSI_IRQ_LPC_SIRQ0;
			lpc->sirq_routes[sirq] = pin;
			prlog(PR_INFO, "LPC[%03x]: SerIRQ %d routed to PSI input %d\n",
			      lpc->chip_id, sirq, pin);
		}
		imap += 5;
		imap_size -= 5;
	}
}

static void lpc_init_chip_p9(struct dt_node *opb_node)
{
	uint32_t gcid = dt_get_chip_id(opb_node);
	struct dt_node *lpc_node;
	struct proc_chip *chip;
	struct lpcm *lpc;
	u64 addr;
	u32 val;

	chip = get_chip(gcid);
	assert(chip);

	/* Grab OPB base address */
	addr = dt_prop_get_cell(opb_node, "ranges", 1);
	addr <<= 32;
	addr |= dt_prop_get_cell(opb_node, "ranges", 2);

	/* Find the "lpc" child node */
	lpc_node = dt_find_compatible_node(opb_node, NULL, "ibm,power9-lpc");
	if (!lpc_node)
		return;

	lpc = zalloc(sizeof(struct lpcm));
	assert(lpc);
	lpc->chip_id = gcid;
	lpc->mbase = (void *)addr;
	lpc->fw_idsel = 0xff;
	lpc->fw_rdsz = 0xff;
	list_head_init(&lpc->clients);
	init_lock(&lpc->lock);

	if (lpc_default_chip_id < 0 ||
	    dt_has_node_property(opb_node, "primary", NULL)) {
		lpc_default_chip_id = gcid;
	}

	/* Parse interrupt map if any to setup initial routing */
	lpc_parse_interrupt_map(lpc, lpc_node);

	/* Mask all interrupts for now */
	opb_write(lpc, lpc_reg_opb_base + LPC_HC_IRQMASK, 0, 4);

	/* Default with routing to PSI SerIRQ 0, this will be updated
	 * later when interrupts are initialized.
	 */
	opb_read(lpc, opb_master_reg_base + 8, &val, 4);
	val &= 0xff03ffff;
	opb_write(lpc, opb_master_reg_base + 8, val, 4);
	opb_read(lpc, opb_master_reg_base + 0xc, &val, 4);
	val &= 0xf0000000;
	opb_write(lpc, opb_master_reg_base + 0xc, val, 4);

	printf("LPC[%03x]: Initialized, access via MMIO @%p\n",
	       gcid, lpc->mbase);

	chip->lpc = lpc;
}

void lpc_init(void)
{
	struct dt_node *xn;
	bool has_lpc = false;

	dt_for_each_compatible(dt_root, xn, "ibm,power8-lpc") {
		lpc_init_chip_p8(xn);
		has_lpc = true;
	}
	dt_for_each_compatible(dt_root, xn, "ibm,power9-lpcm-opb") {
		lpc_init_chip_p9(xn);
		has_lpc = true;
	}
	if (lpc_default_chip_id >= 0)
		printf("LPC: Default bus on chip 0x%x\n", lpc_default_chip_id);

	if (has_lpc) {
		opal_register(OPAL_LPC_WRITE, opal_lpc_write, 5);
		opal_register(OPAL_LPC_READ, opal_lpc_read, 5);
	}
}

void lpc_used_by_console(void)
{
	struct proc_chip *chip;

	xscom_used_by_console();

	for_each_chip(chip) {
		struct lpcm *lpc = chip->lpc;
		if (lpc) {
			lpc->lock.in_con_path = true;
			lock(&lpc->lock);
			unlock(&lpc->lock);
		}
	}
}

bool lpc_ok(void)
{
	struct proc_chip *chip;

	if (lpc_default_chip_id < 0)
		return false;
	if (!xscom_ok())
		return false;
	chip = get_chip(lpc_default_chip_id);
	if (!chip->lpc)
		return false;
	return !lock_held_by_me(&chip->lpc->lock);
}

void lpc_register_client(uint32_t chip_id,
			 const struct lpc_client *clt)
{
	struct lpc_client_entry *ent;
	struct proc_chip *chip;
	struct lpcm *lpc;

	chip = get_chip(chip_id);
	assert(chip);
	lpc = chip->lpc;
	if (!lpc) {
		prerror("LPC: Attempt to register client on bad chip 0x%x\n",
			chip_id);
		return;
	}
	ent = malloc(sizeof(*ent));
	assert(ent);
	ent->clt = clt;
	lock(&lpc->lock);
	list_add(&lpc->clients, &ent->node);
	if (lpc->has_serirq)
		lpc_setup_serirq(lpc);
	unlock(&lpc->lock);
}
