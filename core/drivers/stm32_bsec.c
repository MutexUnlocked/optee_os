// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) 2017-2019, STMicroelectronics
 */

#include <assert.h>
#include <drivers/stm32_bsec.h>
#include <io.h>
#include <kernel/dt.h>
#include <kernel/generic_boot.h>
#include <kernel/spinlock.h>
#include <limits.h>
#include <mm/core_memprot.h>
#include <platform_config.h>
#include <stdint.h>
#include <stm32_util.h>
#include <stm32mp_dt.h>
#include <util.h>

#ifdef CFG_DT
#include <libfdt.h>
#include <string.h>
#endif

#define BSEC_COMPAT			"st,stm32mp15-bsec"
#define DT_NVMEM_LAYOUT_COMPAT		"st,stm32-nvmem-layout"

#define BITS_PER_WORD	(CHAR_BIT * sizeof(uint32_t))
#define OTP_ACCESS_SIZE	(ROUNDUP(OTP_MAX_SIZE, BITS_PER_WORD) / BITS_PER_WORD)

static uint32_t __maybe_unused otp_nsec_access[OTP_ACCESS_SIZE];

static uint32_t bsec_power_safmem(bool enable);

/* Bsec access protection */
static unsigned int lock = SPINLOCK_UNLOCK;

static uint32_t bsec_lock(void)
{
	return may_spin_lock(&lock);
}

static void bsec_unlock(uint32_t exceptions)
{
	may_spin_unlock(&lock, exceptions);
}

#ifdef CFG_DT
static int bsec_get_dt_node(void *fdt, struct dt_node_info *info)
{
	int node;

	node = fdt_get_node(fdt, info, -1, BSEC_COMPAT);
	if (node < 0) {
		return -FDT_ERR_NOTFOUND;
	}

	return node;
}

static void enable_non_secure_access(uint32_t otp)
{
	otp_nsec_access[otp / BITS_PER_WORD] |= BIT(otp % BITS_PER_WORD);

	if (bsec_shadow_register(otp) != BSEC_OK) {
		panic();
	}
}

static bool non_secure_can_access(uint32_t otp)
{
	return (otp_nsec_access[otp / BITS_PER_WORD] &
		BIT(otp % BITS_PER_WORD)) != 0;
}

static int bsec_dt_otp_nsec_access(void *fdt, int bsec_node)
{
	int bsec_subnode;

	fdt_for_each_subnode(bsec_subnode, fdt, bsec_node) {
		const fdt32_t *cuint;
		uint32_t otp;
		uint32_t i;
		uint32_t size;
		uint32_t offset;
		uint32_t length;

		cuint = fdt_getprop(fdt, bsec_subnode, "reg", NULL);
		if (cuint == NULL) {
			panic();
		}

		offset = fdt32_to_cpu(*cuint);
		cuint++;
		length = fdt32_to_cpu(*cuint);

		otp = offset / sizeof(uint32_t);

		if (otp < STM32MP1_UPPER_OTP_START) {
			unsigned int otp_end = ROUNDUP(offset + length,
						       sizeof(uint32_t)) /
					       sizeof(uint32_t);

			if (otp_end > STM32MP1_UPPER_OTP_START) {
				/*
				 * OTP crosses Lower/Upper boundary, consider
				 * only the upper part.
				 */
				otp = STM32MP1_UPPER_OTP_START;
				length -= (STM32MP1_UPPER_OTP_START *
					   sizeof(uint32_t)) - offset;
				offset = STM32MP1_UPPER_OTP_START *
					 sizeof(uint32_t);

				IMSG("OTP crosses Lower/Upper boundary");
			} else {
				continue;
			}
		}

		if ((fdt_getprop(fdt, bsec_subnode,
				 "st,non-secure-otp", NULL)) == NULL) {
			continue;
		}

		if (((offset % sizeof(uint32_t)) != 0) ||
		    ((length % sizeof(uint32_t)) != 0)) {
			EMSG("Unaligned non-secure OTP\n");
			panic();
		}

		size = length / sizeof(uint32_t);

		for (i = otp; i < (otp + size); i++) {
			enable_non_secure_access(i);
		}
	}

	return 0;
}

struct nvmem_layout {
	char *name;
	uint32_t number;
	size_t bit_len;
};

static struct nvmem_layout *nvmem_layout;
static size_t nvmem_layout_count;

static int save_dt_nvmem_layout(void *fdt)
{
	const fdt32_t *cells;
	int i;
	int cell_nb;
	int nvmem_node;

	nvmem_node = fdt_get_node_by_compatible(fdt, DT_NVMEM_LAYOUT_COMPAT);
	if (nvmem_node < 0) {
		return 0;
	}

	cells = fdt_getprop(fdt, nvmem_node, "nvmem-cells", &cell_nb);
	if (cells == NULL) {
		cell_nb = 0;
	}

	cell_nb /= sizeof(uint32_t);

	i = fdt_stringlist_count(fdt, nvmem_node, "nvmem-cell-names");
	if (i < 0) {
		i = 0;
	}

	if (cell_nb != i) {
		EMSG("Inconsistent NVMEM layout");
		panic();
	}

	nvmem_layout = calloc(cell_nb, sizeof(*nvmem_layout));
	if (nvmem_layout == NULL) {
		panic();
	}

	nvmem_layout_count = (size_t)cell_nb;

	for (i = 0; i < cell_nb; i++) {
		const fdt32_t *cuint;
		const char *string;
		int len;
		int node;

		node = fdt_node_offset_by_phandle(fdt,
						  fdt32_to_cpu(*(cells + i)));
		if (node < 0) {
			EMSG("Malformed nvmem_layout node: ignored");
			continue;
		}

		cuint = fdt_getprop(fdt, node, "reg", &len);
		if ((cuint == NULL) || (len != (2 * (int)sizeof(uint32_t))))  {
			EMSG("Malformed nvmem_layout node: ignored");
			continue;
		}

		if (fdt32_to_cpu(*cuint) % sizeof(uint32_t)) {
			EMSG("Misaligned nvmem_layout element: ignored");
			continue;
		}

		nvmem_layout[i].number = fdt32_to_cpu(*cuint) /
					 sizeof(uint32_t);
		nvmem_layout[i].bit_len = fdt32_to_cpu(*(cuint + 1)) * CHAR_BIT;

		string = fdt_stringlist_get(fdt, nvmem_node, "nvmem-cell-names",
					    i, &len);
		if ((string == NULL) || (len == 0)) {
			continue;
		}

		nvmem_layout[i].name = calloc(1, len + 1);
		if (nvmem_layout[i].name == NULL) {
			panic();
		}

		memcpy(nvmem_layout[i].name, string, len);
	}

	return 0;
}
#endif

static uint32_t otp_bank_offset(uint32_t otp)
{
	assert(otp <= stm32mp_get_otp_max());

	return ((otp & ~BSEC_OTP_MASK) >> BSEC_OTP_BANK_SHIFT) *
		sizeof(uint32_t);
}

static uintptr_t bsec_get_base(void)
{
	static void *va;

	if (!cpu_mmu_enabled())
		return BSEC_BASE;

	if (!va)
		va = phys_to_virt(BSEC_BASE, MEM_AREA_IO_SEC);

	return (vaddr_t)va;
}

/*
 * bsec_check_error: check BSEC error status.
 * otp: OTP number.
 * check_disturbed: check only error (false) or all sources (true).
 * return value: BSEC_OK if no error.
 */
static uint32_t bsec_check_error(uint32_t otp, bool check_disturbed)
{
	uint32_t bit = BIT(otp & BSEC_OTP_MASK);
	uint32_t bank = otp_bank_offset(otp);

	if ((read32(bsec_get_base() + BSEC_ERROR_OFF + bank) & bit) != 0U) {
		return BSEC_ERROR;
	}

	if (!check_disturbed) {
		return BSEC_OK;
	}

	if ((read32(bsec_get_base() + BSEC_DISTURBED_OFF + bank) & bit) != 0U) {
		return BSEC_DISTURBED;
	}

	return BSEC_OK;
}

#ifdef CFG_DT
/*
 * bsec_find_otp_name_in_nvmem_layout: find and get OTP location from its name.
 * name: sub-node name to look up.
 * otp: pointer to read OTP number or NULL.
 * otp_bit_len: pointer to read OTP length in bits or NULL.
 * return value: BSEC_OK if no error.
 */
uint32_t bsec_find_otp_name_in_nvmem_layout(const char *name, uint32_t *otp,
					    uint32_t *otp_bit_len)
{
	size_t i;
	void *fdt;
	int node;
	int index, len;
	const fdt32_t *cuint;

	if (name == NULL) {
		return BSEC_INVALID_PARAM;
	}

	for (i = 0; i < nvmem_layout_count; i++) {
		if (!nvmem_layout[i].name ||
		    strcmp(name, nvmem_layout[i].name)) {
			continue;
		}

		if (otp != NULL) {
			*otp = nvmem_layout[i].number;
		}

		if (otp_bit_len != NULL) {
			*otp_bit_len = nvmem_layout[i].bit_len;
		}

		return BSEC_OK;
	}

	return BSEC_ERROR;
}
#endif

/*
 * bsec_shadow_register: copy SAFMEM OTP to BSEC data.
 * otp: OTP number.
 * return value: BSEC_OK if no error.
 */
uint32_t bsec_shadow_register(uint32_t otp)
{
	uint32_t result;
	uint32_t exc;
	bool value;

	result = bsec_read_sr_lock(otp, &value);
	if (result != BSEC_OK) {
		DMSG("BSEC: %u Sticky-read bit read Error %i", otp, result);
		return result;
	}

	if (value) {
		DMSG("BSEC: OTP locked, register will not be refreshed");
	}

	result = bsec_power_safmem(true);
	if (result != BSEC_OK) {
		return result;
	}

	exc = bsec_lock();

	write32(otp | BSEC_READ, bsec_get_base() + BSEC_OTP_CTRL_OFF);

	while ((bsec_get_status() & BSEC_MODE_BUSY_MASK) != 0U) {
		;
	}

	result = bsec_check_error(otp, true);

	bsec_unlock(exc);

	bsec_power_safmem(false);

	return result;
}

/*
 * bsec_read_otp: read an OTP data value.
 * val: read value.
 * otp: OTP number.
 * return value: BSEC_OK if no error.
 */
uint32_t bsec_read_otp(uint32_t *val, uint32_t otp)
{
	uint32_t exc;

	if (otp > stm32mp_get_otp_max()) {
		return BSEC_INVALID_PARAM;
	}

	*val = read32(bsec_get_base() + BSEC_OTP_DATA_OFF +
		      (otp * sizeof(uint32_t)));

	return BSEC_OK;
}

/*
 * bsec_write_otp: write value in BSEC data register.
 * val: value to write.
 * otp: OTP number.
 * return value: BSEC_OK if no error.
 */
uint32_t bsec_write_otp(uint32_t val, uint32_t otp)
{
	uint32_t exc;
	uint32_t result;
	bool value;

	result = bsec_read_sw_lock(otp, &value);
	if (result != BSEC_OK) {
		DMSG("BSEC: %u Sticky-write bit read Error %i", otp, result);
		return result;
	}

	if (value) {
		IMSG("BSEC: OTP locked, write will be ignored");
	}

	/* Ensure integrity of each register access sequence */
	exc = bsec_lock();

	write32(val, bsec_get_base() + BSEC_OTP_DATA_OFF +
		(otp * sizeof(uint32_t)));

	bsec_unlock(exc);

	return result;
}

/*
 * bsec_program_otp: program a bit in SAFMEM after the prog.
 *	The OTP data is not refreshed.
 * val: value to program.
 * otp: OTP number.
 * return value: BSEC_OK if no error.
 */
uint32_t bsec_program_otp(uint32_t val, uint32_t otp)
{
	uint32_t result;
	uint32_t exc;
	bool value;

	result = bsec_read_sp_lock(otp, &value);
	if (result != BSEC_OK) {
		DMSG("BSEC: %u Sticky-prog bit read Error %i", otp, result);
		return result;
	}

	if (value) {
		IMSG("BSEC: OTP locked, prog will be ignored");
	}

	if ((read32(bsec_get_base() + BSEC_OTP_LOCK_OFF) &
	    BIT(BSEC_LOCK_PROGRAM)) != 0U) {
		IMSG("BSEC: GPLOCK activated, prog will be ignored");
	}

	result = bsec_power_safmem(true);
	if (result != BSEC_OK) {
		return result;
	}

	exc = bsec_lock();

	write32(val, bsec_get_base() + BSEC_OTP_WRDATA_OFF);
	write32(otp | BSEC_WRITE, bsec_get_base() + BSEC_OTP_CTRL_OFF);

	while ((bsec_get_status() & BSEC_MODE_BUSY_MASK) != 0U) {
		;
	}

	if ((bsec_get_status() & BSEC_MODE_PROGFAIL_MASK) != 0U) {
		result = BSEC_PROG_FAIL;
	} else {
		result = bsec_check_error(otp, true);
	}

	bsec_unlock(exc);

	bsec_power_safmem(false);

	return result;
}

/*
 * bsec_permanent_lock_otp: permanent lock of OTP in SAFMEM.
 * otp: OTP number.
 * return value: BSEC_OK if no error.
 */
uint32_t bsec_permanent_lock_otp(uint32_t otp)
{
	uint32_t result;
	uint32_t data;
	uint32_t addr;
	uint32_t exc;

	if (otp > stm32mp_get_otp_max()) {
		return BSEC_INVALID_PARAM;
	}

	result = bsec_power_safmem(true);
	if (result != BSEC_OK) {
		return result;
	}

	if (otp < stm32mp_get_otp_upper_start()) {
		addr = otp >> ADDR_LOWER_OTP_PERLOCK_SHIFT;
		data = DATA_LOWER_OTP_PERLOCK_BIT <<
		       ((otp & DATA_LOWER_OTP_PERLOCK_MASK) << 1U);
	} else {
		addr = (otp >> ADDR_UPPER_OTP_PERLOCK_SHIFT) + 2U;
		data = DATA_UPPER_OTP_PERLOCK_BIT <<
		       (otp & DATA_UPPER_OTP_PERLOCK_MASK);
	}

	exc = bsec_lock();

	write32(data, bsec_get_base() + BSEC_OTP_WRDATA_OFF);
	write32(addr | BSEC_WRITE | BSEC_LOCK,
		bsec_get_base() + BSEC_OTP_CTRL_OFF);

	while ((bsec_get_status() & BSEC_MODE_BUSY_MASK) != 0U) {
		;
	}

	if ((bsec_get_status() & BSEC_MODE_PROGFAIL_MASK) != 0U) {
		result = BSEC_PROG_FAIL;
	} else {
		result = bsec_check_error(otp, false);
	}

	bsec_unlock(exc);

	bsec_power_safmem(false);

	return result;
}

/*
 * bsec_write_debug_conf: write value in debug feature.
 *	to enable/disable debug service.
 * val: value to write.
 * return value: BSEC_OK if no error.
 */
uint32_t bsec_write_debug_conf(uint32_t val)
{
	uint32_t result = BSEC_ERROR;
	uint32_t masked_val = val & BSEC_DEN_ALL_MSK;
	unsigned int exc;

	exc = bsec_lock();

	write32(val, bsec_get_base() + BSEC_DEN_OFF);

	if ((read32(bsec_get_base() + BSEC_DEN_OFF) ^ masked_val) == 0U) {
		result = BSEC_OK;
	}

	bsec_unlock(exc);

	return result;
}

/*
 * bsec_read_debug_conf: return debug configuration register value.
 */
uint32_t bsec_read_debug_conf(void)
{
	return read32(bsec_get_base() + BSEC_DEN_OFF);
}

/*
 * bsec_get_status: return status register value.
 */
uint32_t bsec_get_status(void)
{
	return read32(bsec_get_base() + BSEC_OTP_STATUS_OFF);
}

/*
 * bsec_get_hw_conf: return hardware configuration register value.
 */
uint32_t bsec_get_hw_conf(void)
{
	return read32(bsec_get_base() + BSEC_IPHW_CFG_OFF);
}

/*
 * bsec_get_version: return BSEC version register value.
 */
uint32_t bsec_get_version(void)
{
	return read32(bsec_get_base() + BSEC_IPVR_OFF);
}

/*
 * bsec_get_id: return BSEC ID register value.
 */
uint32_t bsec_get_id(void)
{
	return read32(bsec_get_base() + BSEC_IP_ID_OFF);
}

/*
 * bsec_get_magic_id: return BSEC magic number register value.
 */
uint32_t bsec_get_magic_id(void)
{
	return read32(bsec_get_base() + BSEC_IP_MAGIC_ID_OFF);
}

/*
 * bsec_set_sr_lock: set shadow-read lock.
 * otp: OTP number.
 * return value: BSEC_OK if no error.
 */
uint32_t bsec_set_sr_lock(uint32_t otp)
{
	uint32_t bank = otp_bank_offset(otp);
	uint32_t otp_mask = BIT(otp & BSEC_OTP_MASK);
	uint32_t exc;

	if (otp > STM32MP1_OTP_MAX_ID) {
		return BSEC_INVALID_PARAM;
	}

	exc = bsec_lock();
	write32(otp_mask, bsec_get_base() + BSEC_SRLOCK_OFF + bank);
	bsec_unlock(exc);

	return BSEC_OK;
}

/*
 * bsec_read_sr_lock: read shadow-read lock.
 * otp: OTP number.
 * value: read value (true or false).
 * return value: BSEC_OK if no error.
 */
uint32_t bsec_read_sr_lock(uint32_t otp, bool *value)
{
	uint32_t bank = otp_bank_offset(otp);
	uint32_t otp_mask = BIT(otp & BSEC_OTP_MASK);
	uint32_t bank_value;

	if (otp > STM32MP1_OTP_MAX_ID) {
		return BSEC_INVALID_PARAM;
	}

	bank_value = read32(bsec_get_base() + BSEC_SRLOCK_OFF + bank);
	*value = ((bank_value & otp_mask) != 0U);

	return BSEC_OK;
}

/*
 * bsec_set_sw_lock: set shadow-write lock.
 * otp: OTP number.
 * return value: BSEC_OK if no error.
 */
uint32_t bsec_set_sw_lock(uint32_t otp)
{
	uint32_t bank = otp_bank_offset(otp);
	uint32_t otp_mask = BIT(otp & BSEC_OTP_MASK);
	unsigned int exc;

	if (otp > STM32MP1_OTP_MAX_ID) {
		return BSEC_INVALID_PARAM;
	}

	exc = bsec_lock();
	write32(otp_mask, bsec_get_base() + BSEC_SWLOCK_OFF + bank);
	bsec_unlock(exc);

	return BSEC_OK;
}

/*
 * bsec_read_sw_lock: read shadow-write lock.
 * otp: OTP number.
 * value: read value (true or false).
 * return value: BSEC_OK if no error.
 */
uint32_t bsec_read_sw_lock(uint32_t otp, bool *value)
{
	uint32_t bank = otp_bank_offset(otp);
	uint32_t otp_mask = BIT(otp & BSEC_OTP_MASK);
	uint32_t bank_value;

	if (otp > STM32MP1_OTP_MAX_ID) {
		return BSEC_INVALID_PARAM;
	}

	bank_value = read32(bsec_get_base() + BSEC_SWLOCK_OFF + bank);
	*value = ((bank_value & otp_mask) != 0U);

	return BSEC_OK;
}

/*
 * bsec_set_sp_lock: set shadow-program lock.
 * otp: OTP number.
 * return value: BSEC_OK if no error.
 */
uint32_t bsec_set_sp_lock(uint32_t otp)
{
	uint32_t bank = otp_bank_offset(otp);
	uint32_t otp_mask = BIT(otp & BSEC_OTP_MASK);
	unsigned int exc;

	if (otp > STM32MP1_OTP_MAX_ID) {
		return BSEC_INVALID_PARAM;
	}

	exc = bsec_lock();
	write32(otp_mask, bsec_get_base() + BSEC_SPLOCK_OFF + bank);
	bsec_unlock(exc);

	return BSEC_OK;
}

/*
 * bsec_read_sp_lock: read shadow-program lock.
 * otp: OTP number.
 * value: read value (true or false).
 * return value: BSEC_OK if no error.
 */
uint32_t bsec_read_sp_lock(uint32_t otp, bool *value)
{
	uint32_t bank = otp_bank_offset(otp);
	uint32_t otp_mask = BIT(otp & BSEC_OTP_MASK);
	uint32_t bank_value;

	if (otp > STM32MP1_OTP_MAX_ID) {
		return BSEC_INVALID_PARAM;
	}

	bank_value = read32(bsec_get_base() + BSEC_SPLOCK_OFF + bank);
	*value = ((bank_value & otp_mask) != 0U);

	return BSEC_OK;
}

/*
 * bsec_read_permanent_lock: Read permanent lock status.
 * otp: OTP number.
 * value: read value (true or false).
 * return value: BSEC_OK if no error.
 */
uint32_t bsec_read_permanent_lock(uint32_t otp, bool *value)
{
	uint32_t bank = otp_bank_offset(otp);
	uint32_t otp_mask = BIT(otp & BSEC_OTP_MASK);
	uint32_t bank_value;

	if (otp > STM32MP1_OTP_MAX_ID) {
		return BSEC_INVALID_PARAM;
	}

	bank_value = read32(bsec_get_base() + BSEC_WRLOCK_OFF + bank);
	*value = ((bank_value & otp_mask) != 0U);

	return BSEC_OK;
}

/*
 * bsec_otp_lock: Lock Upper OTP or Global Programming or Debug Enable.
 * service: Service to lock see header file.
 * return value: BSEC_OK if no error.
 */
uint32_t bsec_otp_lock(uint32_t service)
{
	uintptr_t reg = bsec_get_base() + BSEC_OTP_LOCK_OFF;

	switch (service) {
	case BSEC_LOCK_UPPER_OTP:
		write32(BIT(BSEC_LOCK_UPPER_OTP), reg);
		break;
	case BSEC_LOCK_DEBUG:
		write32(BIT(BSEC_LOCK_DEBUG), reg);
		break;
	case BSEC_LOCK_PROGRAM:
		write32(BIT(BSEC_LOCK_PROGRAM), reg);
		break;
	default:
		return BSEC_INVALID_PARAM;
	}

	return BSEC_OK;
}

static uint32_t enable_power(void)
{
	size_t cntdown;

	io_mask32(bsec_get_base() + BSEC_OTP_CONF_OFF, BSEC_CONF_POWER_UP_MASK,
		  BSEC_CONF_POWER_UP_MASK);

	for (cntdown = BSEC_TIMEOUT_VALUE; cntdown; cntdown--) {
		if (bsec_get_status() & BSEC_MODE_PWR_MASK) {
			break;
		}
	}

	return cntdown ? BSEC_OK : BSEC_TIMEOUT;
}

static uint32_t disable_power(void)
{
	size_t cntdown;

	io_mask32(bsec_get_base() + BSEC_OTP_CONF_OFF, 0,
		  BSEC_CONF_POWER_UP_MASK);

	for (cntdown = BSEC_TIMEOUT_VALUE; cntdown; cntdown--) {
		if (!(bsec_get_status() & BSEC_MODE_PWR_MASK)) {
			break;
		}
	}

	return cntdown ? BSEC_OK : BSEC_TIMEOUT;
}

/*
 * bsec_power_safmem: Activate or deactivate SAFMEM power.
 * power: true to power up, false to power down.
 * return value: BSEC_OK if no error.
 */
static uint32_t bsec_power_safmem(bool enable)
{
	static unsigned int refcnt = ~0UL;
	uint32_t result = BSEC_OK;
	uint32_t exc = 0;

	/* Get the initial state */
	if (refcnt == ~0UL) {
		refcnt = !!(bsec_get_status() & BSEC_MODE_PWR_MASK);
		DMSG("Reset SAFMEM refcnt to %u", refcnt);
	}

	exc = bsec_lock();

	if (enable && (incr_refcnt(&refcnt) != 0U)) {
		result = enable_power();
	}

	if (!enable && (decr_refcnt(&refcnt) != 0U)) {
		result = disable_power();
	}

	bsec_unlock(exc);

	return result;
}

/*
 * bsec_shadow_read_otp: Load OTP from SAFMEM and provide its value.
 * otp_value: read value.
 * word: OTP number.
 * return value: BSEC_OK if no error.
 */
uint32_t bsec_shadow_read_otp(uint32_t *otp_value, uint32_t word)
{
	uint32_t result;

	result = bsec_shadow_register(word);
	if (result != BSEC_OK) {
		EMSG("BSEC: %u Shadowing Error %i\n", word, result);
		return result;
	}

	result = bsec_read_otp(otp_value, word);
	if (result != BSEC_OK) {
		EMSG("BSEC: %u Read Error %i\n", word, result);
	}

	return result;
}

/*
 * bsec_check_nsec_access_rights: check non-secure access rights to target OTP.
 * otp: OTP number.
 * return value: BSEC_OK if authorized access.
 */
uint32_t bsec_check_nsec_access_rights(uint32_t otp)
{
	if (otp > stm32mp_get_otp_max()) {
		return BSEC_INVALID_PARAM;
	}

	if (otp >= stm32mp_get_otp_upper_start()) {
#ifdef CFG_DT
		if (!non_secure_can_access(otp)) {
			return BSEC_ERROR;
		}
#else
		return BSEC_ERROR;
#endif
	}

	return BSEC_OK;
}

#ifdef CFG_DT
static TEE_Result initialize_bsec(void)
{
	void *fdt;
	int node;
	struct dt_node_info bsec_info;

	fdt = get_dt_blob();
	if (fdt == NULL) {
		panic();
	}

	node = bsec_get_dt_node(fdt, &bsec_info);
	if (node < 0) {
		panic();
	}

	bsec_dt_otp_nsec_access(fdt, node);

	save_dt_nvmem_layout(fdt);

	return TEE_SUCCESS;
}
driver_init(initialize_bsec);
#endif
