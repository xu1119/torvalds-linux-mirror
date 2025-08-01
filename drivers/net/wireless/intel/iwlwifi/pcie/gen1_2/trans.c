// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Copyright (C) 2007-2015, 2018-2024 Intel Corporation
 * Copyright (C) 2013-2015 Intel Mobile Communications GmbH
 * Copyright (C) 2016-2017 Intel Deutschland GmbH
 */
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/debugfs.h>
#include <linux/sched.h>
#include <linux/bitops.h>
#include <linux/gfp.h>
#include <linux/vmalloc.h>
#include <linux/module.h>
#include <linux/wait.h>
#include <linux/seq_file.h>

#include "iwl-drv.h"
#include "iwl-trans.h"
#include "iwl-csr.h"
#include "iwl-prph.h"
#include "iwl-scd.h"
#include "iwl-agn-hw.h"
#include "fw/error-dump.h"
#include "fw/dbg.h"
#include "fw/api/tx.h"
#include "fw/acpi.h"
#include "fw/api/tx.h"
#include "mei/iwl-mei.h"
#include "internal.h"
#include "iwl-fh.h"
#include "pcie/iwl-context-info-v2.h"
#include "pcie/utils.h"

/* extended range in FW SRAM */
#define IWL_FW_MEM_EXTENDED_START	0x40000
#define IWL_FW_MEM_EXTENDED_END		0x57FFF

int iwl_trans_pcie_sw_reset(struct iwl_trans *trans, bool retake_ownership)
{
	/* Reset entire device - do controller reset (results in SHRD_HW_RST) */
	if (trans->mac_cfg->device_family >= IWL_DEVICE_FAMILY_BZ) {
		iwl_set_bit(trans, CSR_GP_CNTRL,
			    CSR_GP_CNTRL_REG_FLAG_SW_RESET);
		usleep_range(10000, 20000);
	} else {
		iwl_set_bit(trans, CSR_RESET,
			    CSR_RESET_REG_FLAG_SW_RESET);
		usleep_range(5000, 6000);
	}

	if (retake_ownership)
		return iwl_pcie_prepare_card_hw(trans);

	return 0;
}

static void iwl_pcie_free_fw_monitor(struct iwl_trans *trans)
{
	struct iwl_dram_data *fw_mon = &trans->dbg.fw_mon;

	if (!fw_mon->size)
		return;

	dma_free_coherent(trans->dev, fw_mon->size, fw_mon->block,
			  fw_mon->physical);

	fw_mon->block = NULL;
	fw_mon->physical = 0;
	fw_mon->size = 0;
}

static void iwl_pcie_alloc_fw_monitor_block(struct iwl_trans *trans,
					    u8 max_power)
{
	struct iwl_dram_data *fw_mon = &trans->dbg.fw_mon;
	void *block = NULL;
	dma_addr_t physical = 0;
	u32 size = 0;
	u8 power;

	if (fw_mon->size) {
		memset(fw_mon->block, 0, fw_mon->size);
		return;
	}

	/* need at least 2 KiB, so stop at 11 */
	for (power = max_power; power >= 11; power--) {
		size = BIT(power);
		block = dma_alloc_coherent(trans->dev, size, &physical,
					   GFP_KERNEL | __GFP_NOWARN);
		if (!block)
			continue;

		IWL_INFO(trans,
			 "Allocated 0x%08x bytes for firmware monitor.\n",
			 size);
		break;
	}

	if (WARN_ON_ONCE(!block))
		return;

	if (power != max_power)
		IWL_ERR(trans,
			"Sorry - debug buffer is only %luK while you requested %luK\n",
			(unsigned long)BIT(power - 10),
			(unsigned long)BIT(max_power - 10));

	fw_mon->block = block;
	fw_mon->physical = physical;
	fw_mon->size = size;
}

void iwl_pcie_alloc_fw_monitor(struct iwl_trans *trans, u8 max_power)
{
	if (!max_power) {
		/* default max_power is maximum */
		max_power = 26;
	} else {
		max_power += 11;
	}

	if (WARN(max_power > 26,
		 "External buffer size for monitor is too big %d, check the FW TLV\n",
		 max_power))
		return;

	iwl_pcie_alloc_fw_monitor_block(trans, max_power);
}

static u32 iwl_trans_pcie_read_shr(struct iwl_trans *trans, u32 reg)
{
	iwl_write32(trans, HEEP_CTRL_WRD_PCIEX_CTRL_REG,
		    ((reg & 0x0000ffff) | (2 << 28)));
	return iwl_read32(trans, HEEP_CTRL_WRD_PCIEX_DATA_REG);
}

static void iwl_trans_pcie_write_shr(struct iwl_trans *trans, u32 reg, u32 val)
{
	iwl_write32(trans, HEEP_CTRL_WRD_PCIEX_DATA_REG, val);
	iwl_write32(trans, HEEP_CTRL_WRD_PCIEX_CTRL_REG,
		    ((reg & 0x0000ffff) | (3 << 28)));
}

static void iwl_pcie_set_pwr(struct iwl_trans *trans, bool vaux)
{
	if (trans->mac_cfg->base->apmg_not_supported)
		return;

	if (vaux && pci_pme_capable(to_pci_dev(trans->dev), PCI_D3cold))
		iwl_set_bits_mask_prph(trans, APMG_PS_CTRL_REG,
				       APMG_PS_CTRL_VAL_PWR_SRC_VAUX,
				       ~APMG_PS_CTRL_MSK_PWR_SRC);
	else
		iwl_set_bits_mask_prph(trans, APMG_PS_CTRL_REG,
				       APMG_PS_CTRL_VAL_PWR_SRC_VMAIN,
				       ~APMG_PS_CTRL_MSK_PWR_SRC);
}

/* PCI registers */
#define PCI_CFG_RETRY_TIMEOUT	0x041

void iwl_pcie_apm_config(struct iwl_trans *trans)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	u16 lctl;
	u16 cap;

	/*
	 * L0S states have been found to be unstable with our devices
	 * and in newer hardware they are not officially supported at
	 * all, so we must always set the L0S_DISABLED bit.
	 */
	iwl_set_bit(trans, CSR_GIO_REG, CSR_GIO_REG_VAL_L0S_DISABLED);

	pcie_capability_read_word(trans_pcie->pci_dev, PCI_EXP_LNKCTL, &lctl);
	trans->pm_support = !(lctl & PCI_EXP_LNKCTL_ASPM_L0S);

	pcie_capability_read_word(trans_pcie->pci_dev, PCI_EXP_DEVCTL2, &cap);
	trans->ltr_enabled = cap & PCI_EXP_DEVCTL2_LTR_EN;
	IWL_DEBUG_POWER(trans, "L1 %sabled - LTR %sabled\n",
			(lctl & PCI_EXP_LNKCTL_ASPM_L1) ? "En" : "Dis",
			trans->ltr_enabled ? "En" : "Dis");
}

/*
 * Start up NIC's basic functionality after it has been reset
 * (e.g. after platform boot, or shutdown via iwl_pcie_apm_stop())
 * NOTE:  This does not load uCode nor start the embedded processor
 */
static int iwl_pcie_apm_init(struct iwl_trans *trans)
{
	int ret;

	IWL_DEBUG_INFO(trans, "Init card's basic functions\n");

	/*
	 * Use "set_bit" below rather than "write", to preserve any hardware
	 * bits already set by default after reset.
	 */

	/* Disable L0S exit timer (platform NMI Work/Around) */
	if (trans->mac_cfg->device_family < IWL_DEVICE_FAMILY_8000)
		iwl_set_bit(trans, CSR_GIO_CHICKEN_BITS,
			    CSR_GIO_CHICKEN_BITS_REG_BIT_DIS_L0S_EXIT_TIMER);

	/*
	 * Disable L0s without affecting L1;
	 *  don't wait for ICH L0s (ICH bug W/A)
	 */
	iwl_set_bit(trans, CSR_GIO_CHICKEN_BITS,
		    CSR_GIO_CHICKEN_BITS_REG_BIT_L1A_NO_L0S_RX);

	/* Set FH wait threshold to maximum (HW error during stress W/A) */
	iwl_set_bit(trans, CSR_DBG_HPET_MEM_REG, CSR_DBG_HPET_MEM_REG_VAL);

	/*
	 * Enable HAP INTA (interrupt from management bus) to
	 * wake device's PCI Express link L1a -> L0s
	 */
	iwl_set_bit(trans, CSR_HW_IF_CONFIG_REG,
		    CSR_HW_IF_CONFIG_REG_HAP_WAKE);

	iwl_pcie_apm_config(trans);

	/* Configure analog phase-lock-loop before activating to D0A */
	if (trans->mac_cfg->base->pll_cfg)
		iwl_set_bit(trans, CSR_ANA_PLL_CFG, CSR50_ANA_PLL_CFG_VAL);

	ret = iwl_finish_nic_init(trans);
	if (ret)
		return ret;

	if (trans->cfg->host_interrupt_operation_mode) {
		/*
		 * This is a bit of an abuse - This is needed for 7260 / 3160
		 * only check host_interrupt_operation_mode even if this is
		 * not related to host_interrupt_operation_mode.
		 *
		 * Enable the oscillator to count wake up time for L1 exit. This
		 * consumes slightly more power (100uA) - but allows to be sure
		 * that we wake up from L1 on time.
		 *
		 * This looks weird: read twice the same register, discard the
		 * value, set a bit, and yet again, read that same register
		 * just to discard the value. But that's the way the hardware
		 * seems to like it.
		 */
		iwl_read_prph(trans, OSC_CLK);
		iwl_read_prph(trans, OSC_CLK);
		iwl_set_bits_prph(trans, OSC_CLK, OSC_CLK_FORCE_CONTROL);
		iwl_read_prph(trans, OSC_CLK);
		iwl_read_prph(trans, OSC_CLK);
	}

	/*
	 * Enable DMA clock and wait for it to stabilize.
	 *
	 * Write to "CLK_EN_REG"; "1" bits enable clocks, while "0"
	 * bits do not disable clocks.  This preserves any hardware
	 * bits already set by default in "CLK_CTRL_REG" after reset.
	 */
	if (!trans->mac_cfg->base->apmg_not_supported) {
		iwl_write_prph(trans, APMG_CLK_EN_REG,
			       APMG_CLK_VAL_DMA_CLK_RQT);
		udelay(20);

		/* Disable L1-Active */
		iwl_set_bits_prph(trans, APMG_PCIDEV_STT_REG,
				  APMG_PCIDEV_STT_VAL_L1_ACT_DIS);

		/* Clear the interrupt in APMG if the NIC is in RFKILL */
		iwl_write_prph(trans, APMG_RTC_INT_STT_REG,
			       APMG_RTC_INT_STT_RFKILL);
	}

	set_bit(STATUS_DEVICE_ENABLED, &trans->status);

	return 0;
}

/*
 * Enable LP XTAL to avoid HW bug where device may consume much power if
 * FW is not loaded after device reset. LP XTAL is disabled by default
 * after device HW reset. Do it only if XTAL is fed by internal source.
 * Configure device's "persistence" mode to avoid resetting XTAL again when
 * SHRD_HW_RST occurs in S3.
 */
static void iwl_pcie_apm_lp_xtal_enable(struct iwl_trans *trans)
{
	int ret;
	u32 apmg_gp1_reg;
	u32 apmg_xtal_cfg_reg;
	u32 dl_cfg_reg;

	/* Force XTAL ON */
	iwl_trans_set_bit(trans, CSR_GP_CNTRL,
			  CSR_GP_CNTRL_REG_FLAG_XTAL_ON);

	ret = iwl_trans_pcie_sw_reset(trans, true);

	if (!ret)
		ret = iwl_finish_nic_init(trans);

	if (WARN_ON(ret)) {
		/* Release XTAL ON request */
		iwl_trans_clear_bit(trans, CSR_GP_CNTRL,
				    CSR_GP_CNTRL_REG_FLAG_XTAL_ON);
		return;
	}

	/*
	 * Clear "disable persistence" to avoid LP XTAL resetting when
	 * SHRD_HW_RST is applied in S3.
	 */
	iwl_clear_bits_prph(trans, APMG_PCIDEV_STT_REG,
				    APMG_PCIDEV_STT_VAL_PERSIST_DIS);

	/*
	 * Force APMG XTAL to be active to prevent its disabling by HW
	 * caused by APMG idle state.
	 */
	apmg_xtal_cfg_reg = iwl_trans_pcie_read_shr(trans,
						    SHR_APMG_XTAL_CFG_REG);
	iwl_trans_pcie_write_shr(trans, SHR_APMG_XTAL_CFG_REG,
				 apmg_xtal_cfg_reg |
				 SHR_APMG_XTAL_CFG_XTAL_ON_REQ);

	ret = iwl_trans_pcie_sw_reset(trans, true);
	if (ret)
		IWL_ERR(trans,
			"iwl_pcie_apm_lp_xtal_enable: failed to retake NIC ownership\n");

	/* Enable LP XTAL by indirect access through CSR */
	apmg_gp1_reg = iwl_trans_pcie_read_shr(trans, SHR_APMG_GP1_REG);
	iwl_trans_pcie_write_shr(trans, SHR_APMG_GP1_REG, apmg_gp1_reg |
				 SHR_APMG_GP1_WF_XTAL_LP_EN |
				 SHR_APMG_GP1_CHICKEN_BIT_SELECT);

	/* Clear delay line clock power up */
	dl_cfg_reg = iwl_trans_pcie_read_shr(trans, SHR_APMG_DL_CFG_REG);
	iwl_trans_pcie_write_shr(trans, SHR_APMG_DL_CFG_REG, dl_cfg_reg &
				 ~SHR_APMG_DL_CFG_DL_CLOCK_POWER_UP);

	/*
	 * Enable persistence mode to avoid LP XTAL resetting when
	 * SHRD_HW_RST is applied in S3.
	 */
	iwl_set_bit(trans, CSR_HW_IF_CONFIG_REG,
		    CSR_HW_IF_CONFIG_REG_PERSISTENCE);

	/*
	 * Clear "initialization complete" bit to move adapter from
	 * D0A* (powered-up Active) --> D0U* (Uninitialized) state.
	 */
	iwl_clear_bit(trans, CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_INIT_DONE);

	/* Activates XTAL resources monitor */
	iwl_trans_set_bit(trans, CSR_MONITOR_CFG_REG,
			  CSR_MONITOR_XTAL_RESOURCES);

	/* Release XTAL ON request */
	iwl_trans_clear_bit(trans, CSR_GP_CNTRL,
			    CSR_GP_CNTRL_REG_FLAG_XTAL_ON);
	udelay(10);

	/* Release APMG XTAL */
	iwl_trans_pcie_write_shr(trans, SHR_APMG_XTAL_CFG_REG,
				 apmg_xtal_cfg_reg &
				 ~SHR_APMG_XTAL_CFG_XTAL_ON_REQ);
}

void iwl_pcie_apm_stop_master(struct iwl_trans *trans)
{
	int ret;

	/* stop device's busmaster DMA activity */

	if (trans->mac_cfg->device_family >= IWL_DEVICE_FAMILY_BZ) {
		iwl_set_bit(trans, CSR_GP_CNTRL,
			    CSR_GP_CNTRL_REG_FLAG_BUS_MASTER_DISABLE_REQ);

		ret = iwl_poll_bits(trans, CSR_GP_CNTRL,
				    CSR_GP_CNTRL_REG_FLAG_BUS_MASTER_DISABLE_STATUS,
				    100);
		usleep_range(10000, 20000);
	} else {
		iwl_set_bit(trans, CSR_RESET, CSR_RESET_REG_FLAG_STOP_MASTER);

		ret = iwl_poll_bits(trans, CSR_RESET,
				    CSR_RESET_REG_FLAG_MASTER_DISABLED, 100);
	}

	if (ret)
		IWL_WARN(trans, "Master Disable Timed Out, 100 usec\n");

	IWL_DEBUG_INFO(trans, "stop master\n");
}

static void iwl_pcie_apm_stop(struct iwl_trans *trans, bool op_mode_leave)
{
	IWL_DEBUG_INFO(trans, "Stop card, put in low power state\n");

	if (op_mode_leave) {
		if (!test_bit(STATUS_DEVICE_ENABLED, &trans->status))
			iwl_pcie_apm_init(trans);

		/* inform ME that we are leaving */
		if (trans->mac_cfg->device_family == IWL_DEVICE_FAMILY_7000)
			iwl_set_bits_prph(trans, APMG_PCIDEV_STT_REG,
					  APMG_PCIDEV_STT_VAL_WAKE_ME);
		else if (trans->mac_cfg->device_family >=
			 IWL_DEVICE_FAMILY_8000) {
			iwl_set_bit(trans, CSR_DBG_LINK_PWR_MGMT_REG,
				    CSR_RESET_LINK_PWR_MGMT_DISABLED);
			iwl_set_bit(trans, CSR_HW_IF_CONFIG_REG,
				    CSR_HW_IF_CONFIG_REG_WAKE_ME |
				    CSR_HW_IF_CONFIG_REG_WAKE_ME_PCIE_OWNER_EN);
			mdelay(1);
			iwl_clear_bit(trans, CSR_DBG_LINK_PWR_MGMT_REG,
				      CSR_RESET_LINK_PWR_MGMT_DISABLED);
		}
		mdelay(5);
	}

	clear_bit(STATUS_DEVICE_ENABLED, &trans->status);

	/* Stop device's DMA activity */
	iwl_pcie_apm_stop_master(trans);

	if (trans->cfg->lp_xtal_workaround) {
		iwl_pcie_apm_lp_xtal_enable(trans);
		return;
	}

	iwl_trans_pcie_sw_reset(trans, false);

	/*
	 * Clear "initialization complete" bit to move adapter from
	 * D0A* (powered-up Active) --> D0U* (Uninitialized) state.
	 */
	iwl_clear_bit(trans, CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_INIT_DONE);
}

static int iwl_pcie_nic_init(struct iwl_trans *trans)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	int ret;

	/* nic_init */
	spin_lock_bh(&trans_pcie->irq_lock);
	ret = iwl_pcie_apm_init(trans);
	spin_unlock_bh(&trans_pcie->irq_lock);

	if (ret)
		return ret;

	iwl_pcie_set_pwr(trans, false);

	iwl_op_mode_nic_config(trans->op_mode);

	/* Allocate the RX queue, or reset if it is already allocated */
	ret = iwl_pcie_rx_init(trans);
	if (ret)
		return ret;

	/* Allocate or reset and init all Tx and Command queues */
	if (iwl_pcie_tx_init(trans)) {
		iwl_pcie_rx_free(trans);
		return -ENOMEM;
	}

	if (trans->mac_cfg->base->shadow_reg_enable) {
		/* enable shadow regs in HW */
		iwl_set_bit(trans, CSR_MAC_SHADOW_REG_CTRL, 0x800FFFFF);
		IWL_DEBUG_INFO(trans, "Enabling shadow registers in device\n");
	}

	return 0;
}

#define HW_READY_TIMEOUT (50)

/* Note: returns poll_bit return value, which is >= 0 if success */
static int iwl_pcie_set_hw_ready(struct iwl_trans *trans)
{
	int ret;

	iwl_set_bit(trans, CSR_HW_IF_CONFIG_REG,
		    CSR_HW_IF_CONFIG_REG_PCI_OWN_SET);

	/* See if we got it */
	ret = iwl_poll_bits(trans, CSR_HW_IF_CONFIG_REG,
			    CSR_HW_IF_CONFIG_REG_PCI_OWN_SET,
			    HW_READY_TIMEOUT);

	if (!ret)
		iwl_set_bit(trans, CSR_MBOX_SET_REG, CSR_MBOX_SET_REG_OS_ALIVE);

	IWL_DEBUG_INFO(trans, "hardware%s ready\n", ret ? " not" : "");
	return ret;
}

/* Note: returns standard 0/-ERROR code */
int iwl_pcie_prepare_card_hw(struct iwl_trans *trans)
{
	int ret;
	int iter;

	IWL_DEBUG_INFO(trans, "iwl_trans_prepare_card_hw enter\n");

	ret = iwl_pcie_set_hw_ready(trans);
	/* If the card is ready, exit 0 */
	if (!ret) {
		trans->csme_own = false;
		return 0;
	}

	iwl_set_bit(trans, CSR_DBG_LINK_PWR_MGMT_REG,
		    CSR_RESET_LINK_PWR_MGMT_DISABLED);
	usleep_range(1000, 2000);

	for (iter = 0; iter < 10; iter++) {
		int t = 0;

		/* If HW is not ready, prepare the conditions to check again */
		iwl_set_bit(trans, CSR_HW_IF_CONFIG_REG,
			    CSR_HW_IF_CONFIG_REG_WAKE_ME);

		do {
			ret = iwl_pcie_set_hw_ready(trans);
			if (!ret) {
				trans->csme_own = false;
				return 0;
			}

			if (iwl_mei_is_connected()) {
				IWL_DEBUG_INFO(trans,
					       "Couldn't prepare the card but SAP is connected\n");
				trans->csme_own = true;
				if (trans->mac_cfg->device_family !=
				    IWL_DEVICE_FAMILY_9000)
					IWL_ERR(trans,
						"SAP not supported for this NIC family\n");

				return -EBUSY;
			}

			usleep_range(200, 1000);
			t += 200;
		} while (t < 150000);
		msleep(25);
	}

	IWL_ERR(trans, "Couldn't prepare the card\n");

	return ret;
}

/*
 * ucode
 */
static void iwl_pcie_load_firmware_chunk_fh(struct iwl_trans *trans,
					    u32 dst_addr, dma_addr_t phy_addr,
					    u32 byte_cnt)
{
	iwl_write32(trans, FH_TCSR_CHNL_TX_CONFIG_REG(FH_SRVC_CHNL),
		    FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_PAUSE);

	iwl_write32(trans, FH_SRVC_CHNL_SRAM_ADDR_REG(FH_SRVC_CHNL),
		    dst_addr);

	iwl_write32(trans, FH_TFDIB_CTRL0_REG(FH_SRVC_CHNL),
		    phy_addr & FH_MEM_TFDIB_DRAM_ADDR_LSB_MSK);

	iwl_write32(trans, FH_TFDIB_CTRL1_REG(FH_SRVC_CHNL),
		    (iwl_get_dma_hi_addr(phy_addr)
			<< FH_MEM_TFDIB_REG1_ADDR_BITSHIFT) | byte_cnt);

	iwl_write32(trans, FH_TCSR_CHNL_TX_BUF_STS_REG(FH_SRVC_CHNL),
		    BIT(FH_TCSR_CHNL_TX_BUF_STS_REG_POS_TB_NUM) |
		    BIT(FH_TCSR_CHNL_TX_BUF_STS_REG_POS_TB_IDX) |
		    FH_TCSR_CHNL_TX_BUF_STS_REG_VAL_TFDB_VALID);

	iwl_write32(trans, FH_TCSR_CHNL_TX_CONFIG_REG(FH_SRVC_CHNL),
		    FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_ENABLE |
		    FH_TCSR_TX_CONFIG_REG_VAL_DMA_CREDIT_DISABLE |
		    FH_TCSR_TX_CONFIG_REG_VAL_CIRQ_HOST_ENDTFD);
}

static int iwl_pcie_load_firmware_chunk(struct iwl_trans *trans,
					u32 dst_addr, dma_addr_t phy_addr,
					u32 byte_cnt)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	int ret;

	trans_pcie->ucode_write_complete = false;

	if (!iwl_trans_grab_nic_access(trans))
		return -EIO;

	iwl_pcie_load_firmware_chunk_fh(trans, dst_addr, phy_addr,
					byte_cnt);
	iwl_trans_release_nic_access(trans);

	ret = wait_event_timeout(trans_pcie->ucode_write_waitq,
				 trans_pcie->ucode_write_complete, 5 * HZ);
	if (!ret) {
		IWL_ERR(trans, "Failed to load firmware chunk!\n");
		iwl_trans_pcie_dump_regs(trans, trans_pcie->pci_dev);
		return -ETIMEDOUT;
	}

	return 0;
}

static int iwl_pcie_load_section(struct iwl_trans *trans, u8 section_num,
			    const struct fw_desc *section)
{
	u8 *v_addr;
	dma_addr_t p_addr;
	u32 offset, chunk_sz = min_t(u32, FH_MEM_TB_MAX_LENGTH, section->len);
	int ret = 0;

	IWL_DEBUG_FW(trans, "[%d] uCode section being loaded...\n",
		     section_num);

	v_addr = dma_alloc_coherent(trans->dev, chunk_sz, &p_addr,
				    GFP_KERNEL | __GFP_NOWARN);
	if (!v_addr) {
		IWL_DEBUG_INFO(trans, "Falling back to small chunks of DMA\n");
		chunk_sz = PAGE_SIZE;
		v_addr = dma_alloc_coherent(trans->dev, chunk_sz,
					    &p_addr, GFP_KERNEL);
		if (!v_addr)
			return -ENOMEM;
	}

	for (offset = 0; offset < section->len; offset += chunk_sz) {
		u32 copy_size, dst_addr;
		bool extended_addr = false;

		copy_size = min_t(u32, chunk_sz, section->len - offset);
		dst_addr = section->offset + offset;

		if (dst_addr >= IWL_FW_MEM_EXTENDED_START &&
		    dst_addr <= IWL_FW_MEM_EXTENDED_END)
			extended_addr = true;

		if (extended_addr)
			iwl_set_bits_prph(trans, LMPM_CHICK,
					  LMPM_CHICK_EXTENDED_ADDR_SPACE);

		memcpy(v_addr, (const u8 *)section->data + offset, copy_size);
		ret = iwl_pcie_load_firmware_chunk(trans, dst_addr, p_addr,
						   copy_size);

		if (extended_addr)
			iwl_clear_bits_prph(trans, LMPM_CHICK,
					    LMPM_CHICK_EXTENDED_ADDR_SPACE);

		if (ret) {
			IWL_ERR(trans,
				"Could not load the [%d] uCode section\n",
				section_num);
			break;
		}
	}

	dma_free_coherent(trans->dev, chunk_sz, v_addr, p_addr);
	return ret;
}

static int iwl_pcie_load_cpu_sections_8000(struct iwl_trans *trans,
					   const struct fw_img *image,
					   int cpu,
					   int *first_ucode_section)
{
	int shift_param;
	int i, ret = 0, sec_num = 0x1;
	u32 val, last_read_idx = 0;

	if (cpu == 1) {
		shift_param = 0;
		*first_ucode_section = 0;
	} else {
		shift_param = 16;
		(*first_ucode_section)++;
	}

	for (i = *first_ucode_section; i < image->num_sec; i++) {
		last_read_idx = i;

		/*
		 * CPU1_CPU2_SEPARATOR_SECTION delimiter - separate between
		 * CPU1 to CPU2.
		 * PAGING_SEPARATOR_SECTION delimiter - separate between
		 * CPU2 non paged to CPU2 paging sec.
		 */
		if (!image->sec[i].data ||
		    image->sec[i].offset == CPU1_CPU2_SEPARATOR_SECTION ||
		    image->sec[i].offset == PAGING_SEPARATOR_SECTION) {
			IWL_DEBUG_FW(trans,
				     "Break since Data not valid or Empty section, sec = %d\n",
				     i);
			break;
		}

		ret = iwl_pcie_load_section(trans, i, &image->sec[i]);
		if (ret)
			return ret;

		/* Notify ucode of loaded section number and status */
		val = iwl_read_direct32(trans, FH_UCODE_LOAD_STATUS);
		val = val | (sec_num << shift_param);
		iwl_write_direct32(trans, FH_UCODE_LOAD_STATUS, val);

		sec_num = (sec_num << 1) | 0x1;
	}

	*first_ucode_section = last_read_idx;

	iwl_enable_interrupts(trans);

	if (trans->mac_cfg->gen2) {
		if (cpu == 1)
			iwl_write_prph(trans, UREG_UCODE_LOAD_STATUS,
				       0xFFFF);
		else
			iwl_write_prph(trans, UREG_UCODE_LOAD_STATUS,
				       0xFFFFFFFF);
	} else {
		if (cpu == 1)
			iwl_write_direct32(trans, FH_UCODE_LOAD_STATUS,
					   0xFFFF);
		else
			iwl_write_direct32(trans, FH_UCODE_LOAD_STATUS,
					   0xFFFFFFFF);
	}

	return 0;
}

static int iwl_pcie_load_cpu_sections(struct iwl_trans *trans,
				      const struct fw_img *image,
				      int cpu,
				      int *first_ucode_section)
{
	int i, ret = 0;
	u32 last_read_idx = 0;

	if (cpu == 1)
		*first_ucode_section = 0;
	else
		(*first_ucode_section)++;

	for (i = *first_ucode_section; i < image->num_sec; i++) {
		last_read_idx = i;

		/*
		 * CPU1_CPU2_SEPARATOR_SECTION delimiter - separate between
		 * CPU1 to CPU2.
		 * PAGING_SEPARATOR_SECTION delimiter - separate between
		 * CPU2 non paged to CPU2 paging sec.
		 */
		if (!image->sec[i].data ||
		    image->sec[i].offset == CPU1_CPU2_SEPARATOR_SECTION ||
		    image->sec[i].offset == PAGING_SEPARATOR_SECTION) {
			IWL_DEBUG_FW(trans,
				     "Break since Data not valid or Empty section, sec = %d\n",
				     i);
			break;
		}

		ret = iwl_pcie_load_section(trans, i, &image->sec[i]);
		if (ret)
			return ret;
	}

	*first_ucode_section = last_read_idx;

	return 0;
}

static void iwl_pcie_apply_destination_ini(struct iwl_trans *trans)
{
	enum iwl_fw_ini_allocation_id alloc_id = IWL_FW_INI_ALLOCATION_ID_DBGC1;
	struct iwl_fw_ini_allocation_tlv *fw_mon_cfg =
		&trans->dbg.fw_mon_cfg[alloc_id];
	struct iwl_dram_data *frag;

	if (!iwl_trans_dbg_ini_valid(trans))
		return;

	if (le32_to_cpu(fw_mon_cfg->buf_location) ==
	    IWL_FW_INI_LOCATION_SRAM_PATH) {
		IWL_DEBUG_FW(trans, "WRT: Applying SMEM buffer destination\n");
		/* set sram monitor by enabling bit 7 */
		iwl_set_bit(trans, CSR_HW_IF_CONFIG_REG,
			    CSR_HW_IF_CONFIG_REG_BIT_MONITOR_SRAM);

		return;
	}

	if (le32_to_cpu(fw_mon_cfg->buf_location) !=
	    IWL_FW_INI_LOCATION_DRAM_PATH ||
	    !trans->dbg.fw_mon_ini[alloc_id].num_frags)
		return;

	frag = &trans->dbg.fw_mon_ini[alloc_id].frags[0];

	IWL_DEBUG_FW(trans, "WRT: Applying DRAM destination (alloc_id=%u)\n",
		     alloc_id);

	iwl_write_umac_prph(trans, MON_BUFF_BASE_ADDR_VER2,
			    frag->physical >> MON_BUFF_SHIFT_VER2);
	iwl_write_umac_prph(trans, MON_BUFF_END_ADDR_VER2,
			    (frag->physical + frag->size - 256) >>
			    MON_BUFF_SHIFT_VER2);
}

void iwl_pcie_apply_destination(struct iwl_trans *trans)
{
	const struct iwl_fw_dbg_dest_tlv_v1 *dest = trans->dbg.dest_tlv;
	const struct iwl_dram_data *fw_mon = &trans->dbg.fw_mon;
	int i;

	if (iwl_trans_dbg_ini_valid(trans)) {
		iwl_pcie_apply_destination_ini(trans);
		return;
	}

	IWL_INFO(trans, "Applying debug destination %s\n",
		 get_fw_dbg_mode_string(dest->monitor_mode));

	if (dest->monitor_mode == EXTERNAL_MODE)
		iwl_pcie_alloc_fw_monitor(trans, dest->size_power);
	else
		IWL_WARN(trans, "PCI should have external buffer debug\n");

	for (i = 0; i < trans->dbg.n_dest_reg; i++) {
		u32 addr = le32_to_cpu(dest->reg_ops[i].addr);
		u32 val = le32_to_cpu(dest->reg_ops[i].val);

		switch (dest->reg_ops[i].op) {
		case CSR_ASSIGN:
			iwl_write32(trans, addr, val);
			break;
		case CSR_SETBIT:
			iwl_set_bit(trans, addr, BIT(val));
			break;
		case CSR_CLEARBIT:
			iwl_clear_bit(trans, addr, BIT(val));
			break;
		case PRPH_ASSIGN:
			iwl_write_prph(trans, addr, val);
			break;
		case PRPH_SETBIT:
			iwl_set_bits_prph(trans, addr, BIT(val));
			break;
		case PRPH_CLEARBIT:
			iwl_clear_bits_prph(trans, addr, BIT(val));
			break;
		case PRPH_BLOCKBIT:
			if (iwl_read_prph(trans, addr) & BIT(val)) {
				IWL_ERR(trans,
					"BIT(%u) in address 0x%x is 1, stopping FW configuration\n",
					val, addr);
				goto monitor;
			}
			break;
		default:
			IWL_ERR(trans, "FW debug - unknown OP %d\n",
				dest->reg_ops[i].op);
			break;
		}
	}

monitor:
	if (dest->monitor_mode == EXTERNAL_MODE && fw_mon->size) {
		iwl_write_prph(trans, le32_to_cpu(dest->base_reg),
			       fw_mon->physical >> dest->base_shift);
		if (trans->mac_cfg->device_family >= IWL_DEVICE_FAMILY_8000)
			iwl_write_prph(trans, le32_to_cpu(dest->end_reg),
				       (fw_mon->physical + fw_mon->size -
					256) >> dest->end_shift);
		else
			iwl_write_prph(trans, le32_to_cpu(dest->end_reg),
				       (fw_mon->physical + fw_mon->size) >>
				       dest->end_shift);
	}
}

static int iwl_pcie_load_given_ucode(struct iwl_trans *trans,
				const struct fw_img *image)
{
	int ret = 0;
	int first_ucode_section;

	IWL_DEBUG_FW(trans, "working with %s CPU\n",
		     image->is_dual_cpus ? "Dual" : "Single");

	/* load to FW the binary non secured sections of CPU1 */
	ret = iwl_pcie_load_cpu_sections(trans, image, 1, &first_ucode_section);
	if (ret)
		return ret;

	if (image->is_dual_cpus) {
		/* set CPU2 header address */
		iwl_write_prph(trans,
			       LMPM_SECURE_UCODE_LOAD_CPU2_HDR_ADDR,
			       LMPM_SECURE_CPU2_HDR_MEM_SPACE);

		/* load to FW the binary sections of CPU2 */
		ret = iwl_pcie_load_cpu_sections(trans, image, 2,
						 &first_ucode_section);
		if (ret)
			return ret;
	}

	if (iwl_pcie_dbg_on(trans))
		iwl_pcie_apply_destination(trans);

	iwl_enable_interrupts(trans);

	/* release CPU reset */
	iwl_write32(trans, CSR_RESET, 0);

	return 0;
}

static int iwl_pcie_load_given_ucode_8000(struct iwl_trans *trans,
					  const struct fw_img *image)
{
	int ret = 0;
	int first_ucode_section;

	IWL_DEBUG_FW(trans, "working with %s CPU\n",
		     image->is_dual_cpus ? "Dual" : "Single");

	if (iwl_pcie_dbg_on(trans))
		iwl_pcie_apply_destination(trans);

	IWL_DEBUG_POWER(trans, "Original WFPM value = 0x%08X\n",
			iwl_read_prph(trans, WFPM_GP2));

	/*
	 * Set default value. On resume reading the values that were
	 * zeored can provide debug data on the resume flow.
	 * This is for debugging only and has no functional impact.
	 */
	iwl_write_prph(trans, WFPM_GP2, 0x01010101);

	/* configure the ucode to be ready to get the secured image */
	/* release CPU reset */
	iwl_write_prph(trans, RELEASE_CPU_RESET, RELEASE_CPU_RESET_BIT);

	/* load to FW the binary Secured sections of CPU1 */
	ret = iwl_pcie_load_cpu_sections_8000(trans, image, 1,
					      &first_ucode_section);
	if (ret)
		return ret;

	/* load to FW the binary sections of CPU2 */
	return iwl_pcie_load_cpu_sections_8000(trans, image, 2,
					       &first_ucode_section);
}

bool iwl_pcie_check_hw_rf_kill(struct iwl_trans *trans)
{
	struct iwl_trans_pcie *trans_pcie =  IWL_TRANS_GET_PCIE_TRANS(trans);
	bool hw_rfkill = iwl_is_rfkill_set(trans);
	bool prev = test_bit(STATUS_RFKILL_OPMODE, &trans->status);
	bool report;

	if (hw_rfkill) {
		set_bit(STATUS_RFKILL_HW, &trans->status);
		set_bit(STATUS_RFKILL_OPMODE, &trans->status);
	} else {
		clear_bit(STATUS_RFKILL_HW, &trans->status);
		if (trans_pcie->opmode_down)
			clear_bit(STATUS_RFKILL_OPMODE, &trans->status);
	}

	report = test_bit(STATUS_RFKILL_OPMODE, &trans->status);

	if (prev != report)
		iwl_trans_pcie_rf_kill(trans, report, false);

	return hw_rfkill;
}

struct iwl_causes_list {
	u16 mask_reg;
	u8 bit;
	u8 addr;
};

#define IWL_CAUSE(reg, mask)						\
	{								\
		.mask_reg = reg,					\
		.bit = ilog2(mask),					\
		.addr = ilog2(mask) +					\
			((reg) == CSR_MSIX_FH_INT_MASK_AD ? -16 :	\
			 (reg) == CSR_MSIX_HW_INT_MASK_AD ? 16 :	\
			 0xffff),	/* causes overflow warning */	\
	}

static const struct iwl_causes_list causes_list_common[] = {
	IWL_CAUSE(CSR_MSIX_FH_INT_MASK_AD, MSIX_FH_INT_CAUSES_D2S_CH0_NUM),
	IWL_CAUSE(CSR_MSIX_FH_INT_MASK_AD, MSIX_FH_INT_CAUSES_D2S_CH1_NUM),
	IWL_CAUSE(CSR_MSIX_FH_INT_MASK_AD, MSIX_FH_INT_CAUSES_S2D),
	IWL_CAUSE(CSR_MSIX_FH_INT_MASK_AD, MSIX_FH_INT_CAUSES_FH_ERR),
	IWL_CAUSE(CSR_MSIX_HW_INT_MASK_AD, MSIX_HW_INT_CAUSES_REG_ALIVE),
	IWL_CAUSE(CSR_MSIX_HW_INT_MASK_AD, MSIX_HW_INT_CAUSES_REG_WAKEUP),
	IWL_CAUSE(CSR_MSIX_HW_INT_MASK_AD, MSIX_HW_INT_CAUSES_REG_RESET_DONE),
	IWL_CAUSE(CSR_MSIX_HW_INT_MASK_AD, MSIX_HW_INT_CAUSES_REG_TOP_FATAL_ERR),
	IWL_CAUSE(CSR_MSIX_HW_INT_MASK_AD, MSIX_HW_INT_CAUSES_REG_CT_KILL),
	IWL_CAUSE(CSR_MSIX_HW_INT_MASK_AD, MSIX_HW_INT_CAUSES_REG_RF_KILL),
	IWL_CAUSE(CSR_MSIX_HW_INT_MASK_AD, MSIX_HW_INT_CAUSES_REG_PERIODIC),
	IWL_CAUSE(CSR_MSIX_HW_INT_MASK_AD, MSIX_HW_INT_CAUSES_REG_SCD),
	IWL_CAUSE(CSR_MSIX_HW_INT_MASK_AD, MSIX_HW_INT_CAUSES_REG_FH_TX),
	IWL_CAUSE(CSR_MSIX_HW_INT_MASK_AD, MSIX_HW_INT_CAUSES_REG_HW_ERR),
	IWL_CAUSE(CSR_MSIX_HW_INT_MASK_AD, MSIX_HW_INT_CAUSES_REG_HAP),
};

static const struct iwl_causes_list causes_list_pre_bz[] = {
	IWL_CAUSE(CSR_MSIX_HW_INT_MASK_AD, MSIX_HW_INT_CAUSES_REG_SW_ERR),
};

static const struct iwl_causes_list causes_list_bz[] = {
	IWL_CAUSE(CSR_MSIX_HW_INT_MASK_AD, MSIX_HW_INT_CAUSES_REG_SW_ERR_BZ),
};

static void iwl_pcie_map_list(struct iwl_trans *trans,
			      const struct iwl_causes_list *causes,
			      int arr_size, int val)
{
	int i;

	for (i = 0; i < arr_size; i++) {
		iwl_write8(trans, CSR_MSIX_IVAR(causes[i].addr), val);
		iwl_clear_bit(trans, causes[i].mask_reg,
			      BIT(causes[i].bit));
	}
}

static void iwl_pcie_map_non_rx_causes(struct iwl_trans *trans)
{
	struct iwl_trans_pcie *trans_pcie =  IWL_TRANS_GET_PCIE_TRANS(trans);
	int val = trans_pcie->def_irq | MSIX_NON_AUTO_CLEAR_CAUSE;
	/*
	 * Access all non RX causes and map them to the default irq.
	 * In case we are missing at least one interrupt vector,
	 * the first interrupt vector will serve non-RX and FBQ causes.
	 */
	iwl_pcie_map_list(trans, causes_list_common,
			  ARRAY_SIZE(causes_list_common), val);
	if (trans->mac_cfg->device_family >= IWL_DEVICE_FAMILY_BZ)
		iwl_pcie_map_list(trans, causes_list_bz,
				  ARRAY_SIZE(causes_list_bz), val);
	else
		iwl_pcie_map_list(trans, causes_list_pre_bz,
				  ARRAY_SIZE(causes_list_pre_bz), val);
}

static void iwl_pcie_map_rx_causes(struct iwl_trans *trans)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	u32 offset =
		trans_pcie->shared_vec_mask & IWL_SHARED_IRQ_FIRST_RSS ? 1 : 0;
	u32 val, idx;

	/*
	 * The first RX queue - fallback queue, which is designated for
	 * management frame, command responses etc, is always mapped to the
	 * first interrupt vector. The other RX queues are mapped to
	 * the other (N - 2) interrupt vectors.
	 */
	val = BIT(MSIX_FH_INT_CAUSES_Q(0));
	for (idx = 1; idx < trans->info.num_rxqs; idx++) {
		iwl_write8(trans, CSR_MSIX_RX_IVAR(idx),
			   MSIX_FH_INT_CAUSES_Q(idx - offset));
		val |= BIT(MSIX_FH_INT_CAUSES_Q(idx));
	}
	iwl_write32(trans, CSR_MSIX_FH_INT_MASK_AD, ~val);

	val = MSIX_FH_INT_CAUSES_Q(0);
	if (trans_pcie->shared_vec_mask & IWL_SHARED_IRQ_NON_RX)
		val |= MSIX_NON_AUTO_CLEAR_CAUSE;
	iwl_write8(trans, CSR_MSIX_RX_IVAR(0), val);

	if (trans_pcie->shared_vec_mask & IWL_SHARED_IRQ_FIRST_RSS)
		iwl_write8(trans, CSR_MSIX_RX_IVAR(1), val);
}

void iwl_pcie_conf_msix_hw(struct iwl_trans_pcie *trans_pcie)
{
	struct iwl_trans *trans = trans_pcie->trans;

	if (!trans_pcie->msix_enabled) {
		if (trans->mac_cfg->mq_rx_supported &&
		    test_bit(STATUS_DEVICE_ENABLED, &trans->status))
			iwl_write_umac_prph(trans, UREG_CHICK,
					    UREG_CHICK_MSI_ENABLE);
		return;
	}
	/*
	 * The IVAR table needs to be configured again after reset,
	 * but if the device is disabled, we can't write to
	 * prph.
	 */
	if (test_bit(STATUS_DEVICE_ENABLED, &trans->status))
		iwl_write_umac_prph(trans, UREG_CHICK, UREG_CHICK_MSIX_ENABLE);

	/*
	 * Each cause from the causes list above and the RX causes is
	 * represented as a byte in the IVAR table. The first nibble
	 * represents the bound interrupt vector of the cause, the second
	 * represents no auto clear for this cause. This will be set if its
	 * interrupt vector is bound to serve other causes.
	 */
	iwl_pcie_map_rx_causes(trans);

	iwl_pcie_map_non_rx_causes(trans);
}

static void iwl_pcie_init_msix(struct iwl_trans_pcie *trans_pcie)
{
	struct iwl_trans *trans = trans_pcie->trans;

	iwl_pcie_conf_msix_hw(trans_pcie);

	if (!trans_pcie->msix_enabled)
		return;

	trans_pcie->fh_init_mask = ~iwl_read32(trans, CSR_MSIX_FH_INT_MASK_AD);
	trans_pcie->fh_mask = trans_pcie->fh_init_mask;
	trans_pcie->hw_init_mask = ~iwl_read32(trans, CSR_MSIX_HW_INT_MASK_AD);
	trans_pcie->hw_mask = trans_pcie->hw_init_mask;
}

static void _iwl_trans_pcie_stop_device(struct iwl_trans *trans, bool from_irq)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);

	lockdep_assert_held(&trans_pcie->mutex);

	if (trans_pcie->is_down)
		return;

	trans_pcie->is_down = true;

	/* tell the device to stop sending interrupts */
	iwl_disable_interrupts(trans);

	/* device going down, Stop using ICT table */
	iwl_pcie_disable_ict(trans);

	/*
	 * If a HW restart happens during firmware loading,
	 * then the firmware loading might call this function
	 * and later it might be called again due to the
	 * restart. So don't process again if the device is
	 * already dead.
	 */
	if (test_and_clear_bit(STATUS_DEVICE_ENABLED, &trans->status)) {
		IWL_DEBUG_INFO(trans,
			       "DEVICE_ENABLED bit was set and is now cleared\n");
		if (!from_irq)
			iwl_pcie_synchronize_irqs(trans);
		iwl_pcie_rx_napi_sync(trans);
		iwl_pcie_tx_stop(trans);
		iwl_pcie_rx_stop(trans);

		/* Power-down device's busmaster DMA clocks */
		if (!trans->mac_cfg->base->apmg_not_supported) {
			iwl_write_prph(trans, APMG_CLK_DIS_REG,
				       APMG_CLK_VAL_DMA_CLK_RQT);
			udelay(5);
		}
	}

	/* Make sure (redundant) we've released our request to stay awake */
	if (trans->mac_cfg->device_family >= IWL_DEVICE_FAMILY_BZ)
		iwl_clear_bit(trans, CSR_GP_CNTRL,
			      CSR_GP_CNTRL_REG_FLAG_BZ_MAC_ACCESS_REQ);
	else
		iwl_clear_bit(trans, CSR_GP_CNTRL,
			      CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);

	/* Stop the device, and put it in low power state */
	iwl_pcie_apm_stop(trans, false);

	/* re-take ownership to prevent other users from stealing the device */
	iwl_trans_pcie_sw_reset(trans, true);

	/*
	 * Upon stop, the IVAR table gets erased, so msi-x won't
	 * work. This causes a bug in RF-KILL flows, since the interrupt
	 * that enables radio won't fire on the correct irq, and the
	 * driver won't be able to handle the interrupt.
	 * Configure the IVAR table again after reset.
	 */
	iwl_pcie_conf_msix_hw(trans_pcie);

	/*
	 * Upon stop, the APM issues an interrupt if HW RF kill is set.
	 * This is a bug in certain verions of the hardware.
	 * Certain devices also keep sending HW RF kill interrupt all
	 * the time, unless the interrupt is ACKed even if the interrupt
	 * should be masked. Re-ACK all the interrupts here.
	 */
	iwl_disable_interrupts(trans);

	/* clear all status bits */
	clear_bit(STATUS_SYNC_HCMD_ACTIVE, &trans->status);
	clear_bit(STATUS_INT_ENABLED, &trans->status);
	clear_bit(STATUS_TPOWER_PMI, &trans->status);

	/*
	 * Even if we stop the HW, we still want the RF kill
	 * interrupt
	 */
	iwl_enable_rfkill_int(trans);
}

void iwl_pcie_synchronize_irqs(struct iwl_trans *trans)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);

	if (trans_pcie->msix_enabled) {
		int i;

		for (i = 0; i < trans_pcie->alloc_vecs; i++)
			synchronize_irq(trans_pcie->msix_entries[i].vector);
	} else {
		synchronize_irq(trans_pcie->pci_dev->irq);
	}
}

int iwl_trans_pcie_start_fw(struct iwl_trans *trans,
			    const struct iwl_fw *fw,
			    const struct fw_img *img,
			    bool run_in_rfkill)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	bool hw_rfkill;
	int ret;

	/* This may fail if AMT took ownership of the device */
	if (iwl_pcie_prepare_card_hw(trans)) {
		IWL_WARN(trans, "Exit HW not ready\n");
		return -EIO;
	}

	iwl_enable_rfkill_int(trans);

	iwl_write32(trans, CSR_INT, 0xFFFFFFFF);

	/*
	 * We enabled the RF-Kill interrupt and the handler may very
	 * well be running. Disable the interrupts to make sure no other
	 * interrupt can be fired.
	 */
	iwl_disable_interrupts(trans);

	/* Make sure it finished running */
	iwl_pcie_synchronize_irqs(trans);

	mutex_lock(&trans_pcie->mutex);

	/* If platform's RF_KILL switch is NOT set to KILL */
	hw_rfkill = iwl_pcie_check_hw_rf_kill(trans);
	if (hw_rfkill && !run_in_rfkill) {
		ret = -ERFKILL;
		goto out;
	}

	/* Someone called stop_device, don't try to start_fw */
	if (trans_pcie->is_down) {
		IWL_WARN(trans,
			 "Can't start_fw since the HW hasn't been started\n");
		ret = -EIO;
		goto out;
	}

	/* make sure rfkill handshake bits are cleared */
	iwl_write32(trans, CSR_UCODE_DRV_GP1_CLR, CSR_UCODE_SW_BIT_RFKILL);
	iwl_write32(trans, CSR_UCODE_DRV_GP1_CLR,
		    CSR_UCODE_DRV_GP1_BIT_CMD_BLOCKED);

	/* clear (again), then enable host interrupts */
	iwl_write32(trans, CSR_INT, 0xFFFFFFFF);

	ret = iwl_pcie_nic_init(trans);
	if (ret) {
		IWL_ERR(trans, "Unable to init nic\n");
		goto out;
	}

	/*
	 * Now, we load the firmware and don't want to be interrupted, even
	 * by the RF-Kill interrupt (hence mask all the interrupt besides the
	 * FH_TX interrupt which is needed to load the firmware). If the
	 * RF-Kill switch is toggled, we will find out after having loaded
	 * the firmware and return the proper value to the caller.
	 */
	iwl_enable_fw_load_int(trans);

	/* really make sure rfkill handshake bits are cleared */
	iwl_write32(trans, CSR_UCODE_DRV_GP1_CLR, CSR_UCODE_SW_BIT_RFKILL);
	iwl_write32(trans, CSR_UCODE_DRV_GP1_CLR, CSR_UCODE_SW_BIT_RFKILL);

	/* Load the given image to the HW */
	if (trans->mac_cfg->device_family >= IWL_DEVICE_FAMILY_8000)
		ret = iwl_pcie_load_given_ucode_8000(trans, img);
	else
		ret = iwl_pcie_load_given_ucode(trans, img);

	/* re-check RF-Kill state since we may have missed the interrupt */
	hw_rfkill = iwl_pcie_check_hw_rf_kill(trans);
	if (hw_rfkill && !run_in_rfkill)
		ret = -ERFKILL;

out:
	mutex_unlock(&trans_pcie->mutex);
	return ret;
}

void iwl_trans_pcie_fw_alive(struct iwl_trans *trans)
{
	iwl_pcie_reset_ict(trans);
	iwl_pcie_tx_start(trans);
}

void iwl_trans_pcie_handle_stop_rfkill(struct iwl_trans *trans,
				       bool was_in_rfkill)
{
	bool hw_rfkill;

	/*
	 * Check again since the RF kill state may have changed while
	 * all the interrupts were disabled, in this case we couldn't
	 * receive the RF kill interrupt and update the state in the
	 * op_mode.
	 * Don't call the op_mode if the rkfill state hasn't changed.
	 * This allows the op_mode to call stop_device from the rfkill
	 * notification without endless recursion. Under very rare
	 * circumstances, we might have a small recursion if the rfkill
	 * state changed exactly now while we were called from stop_device.
	 * This is very unlikely but can happen and is supported.
	 */
	hw_rfkill = iwl_is_rfkill_set(trans);
	if (hw_rfkill) {
		set_bit(STATUS_RFKILL_HW, &trans->status);
		set_bit(STATUS_RFKILL_OPMODE, &trans->status);
	} else {
		clear_bit(STATUS_RFKILL_HW, &trans->status);
		clear_bit(STATUS_RFKILL_OPMODE, &trans->status);
	}
	if (hw_rfkill != was_in_rfkill)
		iwl_trans_pcie_rf_kill(trans, hw_rfkill, false);
}

void iwl_trans_pcie_stop_device(struct iwl_trans *trans)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	bool was_in_rfkill;

	iwl_op_mode_time_point(trans->op_mode,
			       IWL_FW_INI_TIME_POINT_HOST_DEVICE_DISABLE,
			       NULL);

	mutex_lock(&trans_pcie->mutex);
	trans_pcie->opmode_down = true;
	was_in_rfkill = test_bit(STATUS_RFKILL_OPMODE, &trans->status);
	_iwl_trans_pcie_stop_device(trans, false);
	iwl_trans_pcie_handle_stop_rfkill(trans, was_in_rfkill);
	mutex_unlock(&trans_pcie->mutex);
}

void iwl_trans_pcie_rf_kill(struct iwl_trans *trans, bool state, bool from_irq)
{
	struct iwl_trans_pcie __maybe_unused *trans_pcie =
		IWL_TRANS_GET_PCIE_TRANS(trans);

	lockdep_assert_held(&trans_pcie->mutex);

	IWL_WARN(trans, "reporting RF_KILL (radio %s)\n",
		 state ? "disabled" : "enabled");
	if (iwl_op_mode_hw_rf_kill(trans->op_mode, state) &&
	    !WARN_ON(trans->mac_cfg->gen2))
		_iwl_trans_pcie_stop_device(trans, from_irq);
}

static void iwl_pcie_d3_complete_suspend(struct iwl_trans *trans,
					 bool test, bool reset)
{
	iwl_disable_interrupts(trans);

	/*
	 * in testing mode, the host stays awake and the
	 * hardware won't be reset (not even partially)
	 */
	if (test)
		return;

	iwl_pcie_disable_ict(trans);

	iwl_pcie_synchronize_irqs(trans);

	if (trans->mac_cfg->device_family >= IWL_DEVICE_FAMILY_BZ) {
		iwl_clear_bit(trans, CSR_GP_CNTRL,
			      CSR_GP_CNTRL_REG_FLAG_BZ_MAC_ACCESS_REQ);
		iwl_clear_bit(trans, CSR_GP_CNTRL,
			      CSR_GP_CNTRL_REG_FLAG_MAC_INIT);
	} else {
		iwl_clear_bit(trans, CSR_GP_CNTRL,
			      CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
		iwl_clear_bit(trans, CSR_GP_CNTRL,
			      CSR_GP_CNTRL_REG_FLAG_INIT_DONE);
	}

	if (reset) {
		/*
		 * reset TX queues -- some of their registers reset during S3
		 * so if we don't reset everything here the D3 image would try
		 * to execute some invalid memory upon resume
		 */
		iwl_trans_pcie_tx_reset(trans);
	}

	iwl_pcie_set_pwr(trans, true);
}

static int iwl_pcie_d3_handshake(struct iwl_trans *trans, bool suspend)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	int ret;

	if (trans->mac_cfg->device_family < IWL_DEVICE_FAMILY_AX210)
		return 0;

	trans_pcie->sx_state = IWL_SX_WAITING;

	if (trans->mac_cfg->device_family == IWL_DEVICE_FAMILY_AX210)
		iwl_write_umac_prph(trans, UREG_DOORBELL_TO_ISR6,
				    suspend ? UREG_DOORBELL_TO_ISR6_SUSPEND :
					      UREG_DOORBELL_TO_ISR6_RESUME);
	else
		iwl_write32(trans, CSR_IPC_SLEEP_CONTROL,
			    suspend ? CSR_IPC_SLEEP_CONTROL_SUSPEND :
				      CSR_IPC_SLEEP_CONTROL_RESUME);

	ret = wait_event_timeout(trans_pcie->sx_waitq,
				 trans_pcie->sx_state != IWL_SX_WAITING,
				 2 * HZ);
	if (!ret) {
		IWL_ERR(trans, "Timeout %s D3\n",
			suspend ? "entering" : "exiting");
		ret = -ETIMEDOUT;
	} else {
		ret = 0;
	}

	if (trans_pcie->sx_state == IWL_SX_ERROR) {
		IWL_ERR(trans, "FW error while %s D3\n",
			suspend ? "entering" : "exiting");
		ret = -EIO;
	}

	/* Invalidate it toward next suspend or resume */
	trans_pcie->sx_state = IWL_SX_INVALID;

	return ret;
}

int iwl_trans_pcie_d3_suspend(struct iwl_trans *trans, bool test, bool reset)
{
	int ret;

	if (!reset)
		/* Enable persistence mode to avoid reset */
		iwl_set_bit(trans, CSR_HW_IF_CONFIG_REG,
			    CSR_HW_IF_CONFIG_REG_PERSISTENCE);

	ret = iwl_pcie_d3_handshake(trans, true);
	if (ret)
		return ret;

	iwl_pcie_d3_complete_suspend(trans, test, reset);

	return 0;
}

int iwl_trans_pcie_d3_resume(struct iwl_trans *trans,
			     enum iwl_d3_status *status,
			     bool test,  bool reset)
{
	struct iwl_trans_pcie *trans_pcie =  IWL_TRANS_GET_PCIE_TRANS(trans);
	u32 val;
	int ret;

	if (test) {
		iwl_enable_interrupts(trans);
		*status = IWL_D3_STATUS_ALIVE;
		ret = 0;
		goto out;
	}

	if (trans->mac_cfg->device_family >= IWL_DEVICE_FAMILY_BZ)
		iwl_set_bit(trans, CSR_GP_CNTRL,
			    CSR_GP_CNTRL_REG_FLAG_BZ_MAC_ACCESS_REQ);
	else
		iwl_set_bit(trans, CSR_GP_CNTRL,
			    CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);

	ret = iwl_finish_nic_init(trans);
	if (ret)
		return ret;

	/*
	 * Reconfigure IVAR table in case of MSIX or reset ict table in
	 * MSI mode since HW reset erased it.
	 * Also enables interrupts - none will happen as
	 * the device doesn't know we're waking it up, only when
	 * the opmode actually tells it after this call.
	 */
	iwl_pcie_conf_msix_hw(trans_pcie);
	if (!trans_pcie->msix_enabled)
		iwl_pcie_reset_ict(trans);
	iwl_enable_interrupts(trans);

	iwl_pcie_set_pwr(trans, false);

	if (!reset) {
		iwl_clear_bit(trans, CSR_GP_CNTRL,
			      CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
	} else {
		iwl_trans_pcie_tx_reset(trans);

		ret = iwl_pcie_rx_init(trans);
		if (ret) {
			IWL_ERR(trans,
				"Failed to resume the device (RX reset)\n");
			return ret;
		}
	}

	IWL_DEBUG_POWER(trans, "WFPM value upon resume = 0x%08X\n",
			iwl_read_umac_prph(trans, WFPM_GP2));

	val = iwl_read32(trans, CSR_RESET);
	if (val & CSR_RESET_REG_FLAG_NEVO_RESET)
		*status = IWL_D3_STATUS_RESET;
	else
		*status = IWL_D3_STATUS_ALIVE;

out:
	if (*status == IWL_D3_STATUS_ALIVE)
		ret = iwl_pcie_d3_handshake(trans, false);
	else
		trans->state = IWL_TRANS_NO_FW;

	return ret;
}

static void
iwl_pcie_set_interrupt_capa(struct pci_dev *pdev,
			    struct iwl_trans *trans,
			    const struct iwl_mac_cfg *mac_cfg,
			    struct iwl_trans_info *info)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	int max_irqs, num_irqs, i, ret;
	u16 pci_cmd;
	u32 max_rx_queues = IWL_MAX_RX_HW_QUEUES;

	if (!mac_cfg->mq_rx_supported)
		goto enable_msi;

	if (mac_cfg->device_family <= IWL_DEVICE_FAMILY_9000)
		max_rx_queues = IWL_9000_MAX_RX_HW_QUEUES;

	max_irqs = min_t(u32, num_online_cpus() + 2, max_rx_queues);
	for (i = 0; i < max_irqs; i++)
		trans_pcie->msix_entries[i].entry = i;

	num_irqs = pci_enable_msix_range(pdev, trans_pcie->msix_entries,
					 MSIX_MIN_INTERRUPT_VECTORS,
					 max_irqs);
	if (num_irqs < 0) {
		IWL_DEBUG_INFO(trans,
			       "Failed to enable msi-x mode (ret %d). Moving to msi mode.\n",
			       num_irqs);
		goto enable_msi;
	}
	trans_pcie->def_irq = (num_irqs == max_irqs) ? num_irqs - 1 : 0;

	IWL_DEBUG_INFO(trans,
		       "MSI-X enabled. %d interrupt vectors were allocated\n",
		       num_irqs);

	/*
	 * In case the OS provides fewer interrupts than requested, different
	 * causes will share the same interrupt vector as follows:
	 * One interrupt less: non rx causes shared with FBQ.
	 * Two interrupts less: non rx causes shared with FBQ and RSS.
	 * More than two interrupts: we will use fewer RSS queues.
	 */
	if (num_irqs <= max_irqs - 2) {
		info->num_rxqs = num_irqs + 1;
		trans_pcie->shared_vec_mask = IWL_SHARED_IRQ_NON_RX |
			IWL_SHARED_IRQ_FIRST_RSS;
	} else if (num_irqs == max_irqs - 1) {
		info->num_rxqs = num_irqs;
		trans_pcie->shared_vec_mask = IWL_SHARED_IRQ_NON_RX;
	} else {
		info->num_rxqs = num_irqs - 1;
	}

	IWL_DEBUG_INFO(trans,
		       "MSI-X enabled with rx queues %d, vec mask 0x%x\n",
		       info->num_rxqs, trans_pcie->shared_vec_mask);

	WARN_ON(info->num_rxqs > IWL_MAX_RX_HW_QUEUES);

	trans_pcie->alloc_vecs = num_irqs;
	trans_pcie->msix_enabled = true;
	return;

enable_msi:
	info->num_rxqs = 1;
	ret = pci_enable_msi(pdev);
	if (ret) {
		dev_err(&pdev->dev, "pci_enable_msi failed - %d\n", ret);
		/* enable rfkill interrupt: hw bug w/a */
		pci_read_config_word(pdev, PCI_COMMAND, &pci_cmd);
		if (pci_cmd & PCI_COMMAND_INTX_DISABLE) {
			pci_cmd &= ~PCI_COMMAND_INTX_DISABLE;
			pci_write_config_word(pdev, PCI_COMMAND, pci_cmd);
		}
	}
}

static void iwl_pcie_irq_set_affinity(struct iwl_trans *trans,
				      struct iwl_trans_info *info)
{
#if defined(CONFIG_SMP)
	int iter_rx_q, i, ret, cpu, offset;
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);

	i = trans_pcie->shared_vec_mask & IWL_SHARED_IRQ_FIRST_RSS ? 0 : 1;
	iter_rx_q = info->num_rxqs - 1 + i;
	offset = 1 + i;
	for (; i < iter_rx_q ; i++) {
		/*
		 * Get the cpu prior to the place to search
		 * (i.e. return will be > i - 1).
		 */
		cpu = cpumask_next(i - offset, cpu_online_mask);
		cpumask_set_cpu(cpu, &trans_pcie->affinity_mask[i]);
		ret = irq_set_affinity_hint(trans_pcie->msix_entries[i].vector,
					    &trans_pcie->affinity_mask[i]);
		if (ret)
			IWL_ERR(trans_pcie->trans,
				"Failed to set affinity mask for IRQ %d\n",
				trans_pcie->msix_entries[i].vector);
	}
#endif
}

static int iwl_pcie_init_msix_handler(struct pci_dev *pdev,
				      struct iwl_trans_pcie *trans_pcie,
				      struct iwl_trans_info *info)
{
	int i;

	for (i = 0; i < trans_pcie->alloc_vecs; i++) {
		int ret;
		struct msix_entry *msix_entry;
		const char *qname = queue_name(&pdev->dev, trans_pcie, i);

		if (!qname)
			return -ENOMEM;

		msix_entry = &trans_pcie->msix_entries[i];
		ret = devm_request_threaded_irq(&pdev->dev,
						msix_entry->vector,
						iwl_pcie_msix_isr,
						(i == trans_pcie->def_irq) ?
						iwl_pcie_irq_msix_handler :
						iwl_pcie_irq_rx_msix_handler,
						IRQF_SHARED,
						qname,
						msix_entry);
		if (ret) {
			IWL_ERR(trans_pcie->trans,
				"Error allocating IRQ %d\n", i);

			return ret;
		}
	}
	iwl_pcie_irq_set_affinity(trans_pcie->trans, info);

	return 0;
}

static int iwl_trans_pcie_clear_persistence_bit(struct iwl_trans *trans)
{
	u32 hpm, wprot;

	switch (trans->mac_cfg->device_family) {
	case IWL_DEVICE_FAMILY_9000:
		wprot = PREG_PRPH_WPROT_9000;
		break;
	case IWL_DEVICE_FAMILY_22000:
		wprot = PREG_PRPH_WPROT_22000;
		break;
	default:
		return 0;
	}

	hpm = iwl_read_umac_prph_no_grab(trans, HPM_DEBUG);
	if (!iwl_trans_is_hw_error_value(hpm) && (hpm & PERSISTENCE_BIT)) {
		u32 wprot_val = iwl_read_umac_prph_no_grab(trans, wprot);

		if (wprot_val & PREG_WFPM_ACCESS) {
			IWL_ERR(trans,
				"Error, can not clear persistence bit\n");
			return -EPERM;
		}
		iwl_write_umac_prph_no_grab(trans, HPM_DEBUG,
					    hpm & ~PERSISTENCE_BIT);
	}

	return 0;
}

static int iwl_pcie_gen2_force_power_gating(struct iwl_trans *trans)
{
	int ret;

	ret = iwl_finish_nic_init(trans);
	if (ret < 0)
		return ret;

	iwl_set_bits_prph(trans, HPM_HIPM_GEN_CFG,
			  HPM_HIPM_GEN_CFG_CR_FORCE_ACTIVE);
	udelay(20);
	iwl_set_bits_prph(trans, HPM_HIPM_GEN_CFG,
			  HPM_HIPM_GEN_CFG_CR_PG_EN |
			  HPM_HIPM_GEN_CFG_CR_SLP_EN);
	udelay(20);
	iwl_clear_bits_prph(trans, HPM_HIPM_GEN_CFG,
			    HPM_HIPM_GEN_CFG_CR_FORCE_ACTIVE);

	return iwl_trans_pcie_sw_reset(trans, true);
}

int _iwl_trans_pcie_start_hw(struct iwl_trans *trans)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	int err;

	lockdep_assert_held(&trans_pcie->mutex);

	err = iwl_pcie_prepare_card_hw(trans);
	if (err) {
		IWL_ERR(trans, "Error while preparing HW: %d\n", err);
		return err;
	}

	err = iwl_trans_pcie_clear_persistence_bit(trans);
	if (err)
		return err;

	err = iwl_trans_pcie_sw_reset(trans, true);
	if (err)
		return err;

	if (trans->mac_cfg->device_family == IWL_DEVICE_FAMILY_22000 &&
	    trans->mac_cfg->integrated) {
		err = iwl_pcie_gen2_force_power_gating(trans);
		if (err)
			return err;
	}

	err = iwl_pcie_apm_init(trans);
	if (err)
		return err;

	iwl_pcie_init_msix(trans_pcie);

	/* From now on, the op_mode will be kept updated about RF kill state */
	iwl_enable_rfkill_int(trans);

	trans_pcie->opmode_down = false;

	/* Set is_down to false here so that...*/
	trans_pcie->is_down = false;

	/* ...rfkill can call stop_device and set it false if needed */
	iwl_pcie_check_hw_rf_kill(trans);

	return 0;
}

int iwl_trans_pcie_start_hw(struct iwl_trans *trans)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	int ret;

	mutex_lock(&trans_pcie->mutex);
	ret = _iwl_trans_pcie_start_hw(trans);
	mutex_unlock(&trans_pcie->mutex);

	return ret;
}

void iwl_trans_pcie_op_mode_leave(struct iwl_trans *trans)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);

	mutex_lock(&trans_pcie->mutex);

	/* disable interrupts - don't enable HW RF kill interrupt */
	iwl_disable_interrupts(trans);

	iwl_pcie_apm_stop(trans, true);

	iwl_disable_interrupts(trans);

	iwl_pcie_disable_ict(trans);

	mutex_unlock(&trans_pcie->mutex);

	iwl_pcie_synchronize_irqs(trans);
}

void iwl_trans_pcie_write8(struct iwl_trans *trans, u32 ofs, u8 val)
{
	writeb(val, IWL_TRANS_GET_PCIE_TRANS(trans)->hw_base + ofs);
}

void iwl_trans_pcie_write32(struct iwl_trans *trans, u32 ofs, u32 val)
{
	writel(val, IWL_TRANS_GET_PCIE_TRANS(trans)->hw_base + ofs);
}

u32 iwl_trans_pcie_read32(struct iwl_trans *trans, u32 ofs)
{
	return readl(IWL_TRANS_GET_PCIE_TRANS(trans)->hw_base + ofs);
}

static u32 iwl_trans_pcie_prph_msk(struct iwl_trans *trans)
{
	if (trans->mac_cfg->device_family >= IWL_DEVICE_FAMILY_AX210)
		return 0x00FFFFFF;
	else
		return 0x000FFFFF;
}

u32 iwl_trans_pcie_read_prph(struct iwl_trans *trans, u32 reg)
{
	u32 mask = iwl_trans_pcie_prph_msk(trans);

	iwl_trans_pcie_write32(trans, HBUS_TARG_PRPH_RADDR,
			       ((reg & mask) | (3 << 24)));
	return iwl_trans_pcie_read32(trans, HBUS_TARG_PRPH_RDAT);
}

void iwl_trans_pcie_write_prph(struct iwl_trans *trans, u32 addr, u32 val)
{
	u32 mask = iwl_trans_pcie_prph_msk(trans);

	iwl_trans_pcie_write32(trans, HBUS_TARG_PRPH_WADDR,
			       ((addr & mask) | (3 << 24)));
	iwl_trans_pcie_write32(trans, HBUS_TARG_PRPH_WDAT, val);
}

void iwl_trans_pcie_op_mode_enter(struct iwl_trans *trans)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);

	/* free all first - we might be reconfigured for a different size */
	iwl_pcie_free_rbs_pool(trans);

	trans_pcie->rx_page_order =
		iwl_trans_get_rb_size_order(trans->conf.rx_buf_size);
	trans_pcie->rx_buf_bytes =
		iwl_trans_get_rb_size(trans->conf.rx_buf_size);
}

void iwl_trans_pcie_free_pnvm_dram_regions(struct iwl_dram_regions *dram_regions,
					   struct device *dev)
{
	u8 i;
	struct iwl_dram_data *desc_dram = &dram_regions->prph_scratch_mem_desc;

	/* free DRAM payloads */
	for (i = 0; i < dram_regions->n_regions; i++) {
		dma_free_coherent(dev, dram_regions->drams[i].size,
				  dram_regions->drams[i].block,
				  dram_regions->drams[i].physical);
	}
	dram_regions->n_regions = 0;

	/* free DRAM addresses array */
	if (desc_dram->block) {
		dma_free_coherent(dev, desc_dram->size,
				  desc_dram->block,
				  desc_dram->physical);
	}
	memset(desc_dram, 0, sizeof(*desc_dram));
}

static void iwl_pcie_free_invalid_tx_cmd(struct iwl_trans *trans)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);

	iwl_pcie_free_dma_ptr(trans, &trans_pcie->invalid_tx_cmd);
}

static int iwl_pcie_alloc_invalid_tx_cmd(struct iwl_trans *trans)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	struct iwl_cmd_header_wide bad_cmd = {
		.cmd = INVALID_WR_PTR_CMD,
		.group_id = DEBUG_GROUP,
		.sequence = cpu_to_le16(0xffff),
		.length = cpu_to_le16(0),
		.version = 0,
	};
	int ret;

	ret = iwl_pcie_alloc_dma_ptr(trans, &trans_pcie->invalid_tx_cmd,
				     sizeof(bad_cmd));
	if (ret)
		return ret;
	memcpy(trans_pcie->invalid_tx_cmd.addr, &bad_cmd, sizeof(bad_cmd));
	return 0;
}

void iwl_trans_pcie_free(struct iwl_trans *trans)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	int i;

	iwl_pcie_synchronize_irqs(trans);

	if (trans->mac_cfg->gen2)
		iwl_txq_gen2_tx_free(trans);
	else
		iwl_pcie_tx_free(trans);
	iwl_pcie_rx_free(trans);

	if (trans_pcie->rba.alloc_wq) {
		destroy_workqueue(trans_pcie->rba.alloc_wq);
		trans_pcie->rba.alloc_wq = NULL;
	}

	if (trans_pcie->msix_enabled) {
		for (i = 0; i < trans_pcie->alloc_vecs; i++) {
			irq_set_affinity_hint(
				trans_pcie->msix_entries[i].vector,
				NULL);
		}

		trans_pcie->msix_enabled = false;
	} else {
		iwl_pcie_free_ict(trans);
	}

	free_netdev(trans_pcie->napi_dev);

	iwl_pcie_free_invalid_tx_cmd(trans);

	iwl_pcie_free_fw_monitor(trans);

	iwl_trans_pcie_free_pnvm_dram_regions(&trans_pcie->pnvm_data,
					      trans->dev);
	iwl_trans_pcie_free_pnvm_dram_regions(&trans_pcie->reduced_tables_data,
					      trans->dev);

	mutex_destroy(&trans_pcie->mutex);

	if (trans_pcie->txqs.tso_hdr_page) {
		for_each_possible_cpu(i) {
			struct iwl_tso_hdr_page *p =
				per_cpu_ptr(trans_pcie->txqs.tso_hdr_page, i);

			if (p && p->page)
				__free_page(p->page);
		}

		free_percpu(trans_pcie->txqs.tso_hdr_page);
	}

	iwl_trans_free(trans);
}

static union acpi_object *
iwl_trans_pcie_call_prod_reset_dsm(struct pci_dev *pdev, u16 cmd, u16 value)
{
#ifdef CONFIG_ACPI
	struct iwl_dsm_internal_product_reset_cmd pldr_arg = {
		.cmd = cmd,
		.value = value,
	};
	union acpi_object arg = {
		.buffer.type = ACPI_TYPE_BUFFER,
		.buffer.length = sizeof(pldr_arg),
		.buffer.pointer = (void *)&pldr_arg,
	};
	static const guid_t dsm_guid = GUID_INIT(0x7266172C, 0x220B, 0x4B29,
						 0x81, 0x4F, 0x75, 0xE4,
						 0xDD, 0x26, 0xB5, 0xFD);

	if (!acpi_check_dsm(ACPI_HANDLE(&pdev->dev), &dsm_guid, ACPI_DSM_REV,
			    DSM_INTERNAL_FUNC_PRODUCT_RESET))
		return ERR_PTR(-ENODEV);

	return iwl_acpi_get_dsm_object(&pdev->dev, ACPI_DSM_REV,
				       DSM_INTERNAL_FUNC_PRODUCT_RESET,
				       &arg, &dsm_guid);
#else
	return ERR_PTR(-EOPNOTSUPP);
#endif
}

void iwl_trans_pcie_check_product_reset_mode(struct pci_dev *pdev)
{
	union acpi_object *res;

	res = iwl_trans_pcie_call_prod_reset_dsm(pdev,
						 DSM_INTERNAL_PLDR_CMD_GET_MODE,
						 0);
	if (IS_ERR(res))
		return;

	if (res->type != ACPI_TYPE_INTEGER)
		IWL_ERR_DEV(&pdev->dev,
			    "unexpected return type from product reset DSM\n");
	else
		IWL_DEBUG_DEV_POWER(&pdev->dev,
				    "product reset mode is 0x%llx\n",
				    res->integer.value);

	ACPI_FREE(res);
}

static void iwl_trans_pcie_set_product_reset(struct pci_dev *pdev, bool enable,
					     bool integrated)
{
	union acpi_object *res;
	u16 mode = enable ? DSM_INTERNAL_PLDR_MODE_EN_PROD_RESET : 0;

	if (!integrated)
		mode |= DSM_INTERNAL_PLDR_MODE_EN_WIFI_FLR |
			DSM_INTERNAL_PLDR_MODE_EN_BT_OFF_ON;

	res = iwl_trans_pcie_call_prod_reset_dsm(pdev,
						 DSM_INTERNAL_PLDR_CMD_SET_MODE,
						 mode);
	if (IS_ERR(res)) {
		if (enable)
			IWL_ERR_DEV(&pdev->dev,
				    "ACPI _DSM not available (%d), cannot do product reset\n",
				    (int)PTR_ERR(res));
		return;
	}

	ACPI_FREE(res);
	IWL_DEBUG_DEV_POWER(&pdev->dev, "%sabled product reset via DSM\n",
			    enable ? "En" : "Dis");
	iwl_trans_pcie_check_product_reset_mode(pdev);
}

void iwl_trans_pcie_check_product_reset_status(struct pci_dev *pdev)
{
	union acpi_object *res;

	res = iwl_trans_pcie_call_prod_reset_dsm(pdev,
						 DSM_INTERNAL_PLDR_CMD_GET_STATUS,
						 0);
	if (IS_ERR(res))
		return;

	if (res->type != ACPI_TYPE_INTEGER)
		IWL_ERR_DEV(&pdev->dev,
			    "unexpected return type from product reset DSM\n");
	else
		IWL_DEBUG_DEV_POWER(&pdev->dev,
				    "product reset status is 0x%llx\n",
				    res->integer.value);

	ACPI_FREE(res);
}

static void iwl_trans_pcie_call_reset(struct pci_dev *pdev)
{
#ifdef CONFIG_ACPI
	struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *p, *ref;
	acpi_status status;
	int ret = -EINVAL;

	status = acpi_evaluate_object(ACPI_HANDLE(&pdev->dev),
				      "_PRR", NULL, &buffer);
	if (ACPI_FAILURE(status)) {
		IWL_DEBUG_DEV_POWER(&pdev->dev, "No _PRR method found\n");
		goto out;
	}
	p = buffer.pointer;

	if (p->type != ACPI_TYPE_PACKAGE || p->package.count != 1) {
		pci_err(pdev, "Bad _PRR return type\n");
		goto out;
	}

	ref = &p->package.elements[0];
	if (ref->type != ACPI_TYPE_LOCAL_REFERENCE) {
		pci_err(pdev, "_PRR wasn't a reference\n");
		goto out;
	}

	status = acpi_evaluate_object(ref->reference.handle,
				      "_RST", NULL, NULL);
	if (ACPI_FAILURE(status)) {
		pci_err(pdev,
			"Failed to call _RST on object returned by _PRR (%d)\n",
			status);
		goto out;
	}
	ret = 0;
out:
	kfree(buffer.pointer);
	if (!ret) {
		IWL_DEBUG_DEV_POWER(&pdev->dev, "called _RST on _PRR object\n");
		return;
	}
	IWL_DEBUG_DEV_POWER(&pdev->dev,
			    "No BIOS support, using pci_reset_function()\n");
#endif
	pci_reset_function(pdev);
}

struct iwl_trans_pcie_removal {
	struct pci_dev *pdev;
	struct work_struct work;
	enum iwl_reset_mode mode;
	bool integrated;
};

static void iwl_trans_pcie_removal_wk(struct work_struct *wk)
{
	struct iwl_trans_pcie_removal *removal =
		container_of(wk, struct iwl_trans_pcie_removal, work);
	struct pci_dev *pdev = removal->pdev;
	static char *prop[] = {"EVENT=INACCESSIBLE", NULL};
	struct pci_bus *bus;

	pci_lock_rescan_remove();

	bus = pdev->bus;
	/* in this case, something else already removed the device */
	if (!bus)
		goto out;

	kobject_uevent_env(&pdev->dev.kobj, KOBJ_CHANGE, prop);

	if (removal->mode == IWL_RESET_MODE_PROD_RESET) {
		struct pci_dev *bt = NULL;

		if (!removal->integrated) {
			/* discrete devices have WiFi/BT at function 0/1 */
			int slot = PCI_SLOT(pdev->devfn);
			int func = PCI_FUNC(pdev->devfn);

			if (func == 0)
				bt = pci_get_slot(bus, PCI_DEVFN(slot, 1));
			else
				pci_info(pdev, "Unexpected function %d\n",
					 func);
		} else {
			/* on integrated we have to look up by ID (same bus) */
			static const struct pci_device_id bt_device_ids[] = {
#define BT_DEV(_id) { PCI_DEVICE(PCI_VENDOR_ID_INTEL, _id) }
				BT_DEV(0xA876), /* LNL */
				BT_DEV(0xE476), /* PTL-P */
				BT_DEV(0xE376), /* PTL-H */
				BT_DEV(0xD346), /* NVL-H */
				BT_DEV(0x6E74), /* NVL-S */
				BT_DEV(0x4D76), /* WCL */
				BT_DEV(0xD246), /* RZL-H */
				BT_DEV(0x6C46), /* RZL-M */
				{}
			};
			struct pci_dev *tmp = NULL;

			for_each_pci_dev(tmp) {
				if (tmp->bus != bus)
					continue;

				if (pci_match_id(bt_device_ids, tmp)) {
					bt = tmp;
					break;
				}
			}
		}

		if (bt) {
			pci_info(bt, "Removal by WiFi due to product reset\n");
			pci_stop_and_remove_bus_device(bt);
			pci_dev_put(bt);
		}
	}

	iwl_trans_pcie_set_product_reset(pdev,
					 removal->mode ==
						IWL_RESET_MODE_PROD_RESET,
					 removal->integrated);
	if (removal->mode >= IWL_RESET_MODE_FUNC_RESET)
		iwl_trans_pcie_call_reset(pdev);

	pci_stop_and_remove_bus_device(pdev);
	pci_dev_put(pdev);

	if (removal->mode >= IWL_RESET_MODE_RESCAN) {
		if (bus->parent)
			bus = bus->parent;
		pci_rescan_bus(bus);
	}

out:
	pci_unlock_rescan_remove();

	kfree(removal);
	module_put(THIS_MODULE);
}

void iwl_trans_pcie_reset(struct iwl_trans *trans, enum iwl_reset_mode mode)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	struct iwl_trans_pcie_removal *removal;
	char _msg = 0, *msg = &_msg;

	if (WARN_ON(mode < IWL_RESET_MODE_REMOVE_ONLY ||
		    mode == IWL_RESET_MODE_BACKOFF))
		return;

	if (test_bit(STATUS_TRANS_DEAD, &trans->status))
		return;

	if (trans_pcie->me_present && mode == IWL_RESET_MODE_PROD_RESET) {
		mode = IWL_RESET_MODE_FUNC_RESET;
		if (trans_pcie->me_present < 0)
			msg = " instead of product reset as ME may be present";
		else
			msg = " instead of product reset as ME is present";
	}

	IWL_INFO(trans, "scheduling reset (mode=%d%s)\n", mode, msg);

	iwl_pcie_dump_csr(trans);

	/*
	 * get a module reference to avoid doing this
	 * while unloading anyway and to avoid
	 * scheduling a work with code that's being
	 * removed.
	 */
	if (!try_module_get(THIS_MODULE)) {
		IWL_ERR(trans,
			"Module is being unloaded - abort\n");
		return;
	}

	removal = kzalloc(sizeof(*removal), GFP_ATOMIC);
	if (!removal) {
		module_put(THIS_MODULE);
		return;
	}
	/*
	 * we don't need to clear this flag, because
	 * the trans will be freed and reallocated.
	 */
	set_bit(STATUS_TRANS_DEAD, &trans->status);

	removal->pdev = to_pci_dev(trans->dev);
	removal->mode = mode;
	removal->integrated = trans->mac_cfg->integrated;
	INIT_WORK(&removal->work, iwl_trans_pcie_removal_wk);
	pci_dev_get(removal->pdev);
	schedule_work(&removal->work);
}
EXPORT_SYMBOL(iwl_trans_pcie_reset);

/*
 * This version doesn't disable BHs but rather assumes they're
 * already disabled.
 */
bool __iwl_trans_pcie_grab_nic_access(struct iwl_trans *trans, bool silent)
{
	int ret;
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	u32 write = CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ;
	u32 mask = CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY |
		   CSR_GP_CNTRL_REG_FLAG_GOING_TO_SLEEP;
	u32 poll = CSR_GP_CNTRL_REG_VAL_MAC_ACCESS_EN;

	if (test_bit(STATUS_TRANS_DEAD, &trans->status))
		return false;

	spin_lock(&trans_pcie->reg_lock);

	if (trans_pcie->cmd_hold_nic_awake)
		goto out;

	if (trans->mac_cfg->device_family >= IWL_DEVICE_FAMILY_BZ) {
		write = CSR_GP_CNTRL_REG_FLAG_BZ_MAC_ACCESS_REQ;
		mask = CSR_GP_CNTRL_REG_FLAG_MAC_STATUS;
		poll = CSR_GP_CNTRL_REG_FLAG_MAC_STATUS;
	}

	/* this bit wakes up the NIC */
	iwl_trans_set_bit(trans, CSR_GP_CNTRL, write);
	if (trans->mac_cfg->device_family >= IWL_DEVICE_FAMILY_8000)
		udelay(2);

	/*
	 * These bits say the device is running, and should keep running for
	 * at least a short while (at least as long as MAC_ACCESS_REQ stays 1),
	 * but they do not indicate that embedded SRAM is restored yet;
	 * HW with volatile SRAM must save/restore contents to/from
	 * host DRAM when sleeping/waking for power-saving.
	 * Each direction takes approximately 1/4 millisecond; with this
	 * overhead, it's a good idea to grab and hold MAC_ACCESS_REQUEST if a
	 * series of register accesses are expected (e.g. reading Event Log),
	 * to keep device from sleeping.
	 *
	 * CSR_UCODE_DRV_GP1 register bit MAC_SLEEP == 0 indicates that
	 * SRAM is okay/restored.  We don't check that here because this call
	 * is just for hardware register access; but GP1 MAC_SLEEP
	 * check is a good idea before accessing the SRAM of HW with
	 * volatile SRAM (e.g. reading Event Log).
	 *
	 * 5000 series and later (including 1000 series) have non-volatile SRAM,
	 * and do not save/restore SRAM when power cycling.
	 */
	ret = iwl_poll_bits_mask(trans, CSR_GP_CNTRL, poll, mask, 15000);
	if (unlikely(ret)) {
		u32 cntrl = iwl_read32(trans, CSR_GP_CNTRL);

		if (silent) {
			spin_unlock(&trans_pcie->reg_lock);
			return false;
		}

		WARN_ONCE(1,
			  "Timeout waiting for hardware access (CSR_GP_CNTRL 0x%08x)\n",
			  cntrl);

		iwl_trans_pcie_dump_regs(trans, trans_pcie->pci_dev);

		if (iwlwifi_mod_params.remove_when_gone && cntrl == ~0U)
			iwl_trans_pcie_reset(trans,
					     IWL_RESET_MODE_REMOVE_ONLY);
		else
			iwl_write32(trans, CSR_RESET,
				    CSR_RESET_REG_FLAG_FORCE_NMI);

		spin_unlock(&trans_pcie->reg_lock);
		return false;
	}

out:
	/*
	 * Fool sparse by faking we release the lock - sparse will
	 * track nic_access anyway.
	 */
	__release(&trans_pcie->reg_lock);
	return true;
}

bool iwl_trans_pcie_grab_nic_access(struct iwl_trans *trans)
{
	bool ret;

	local_bh_disable();
	ret = __iwl_trans_pcie_grab_nic_access(trans, false);
	if (ret) {
		/* keep BHs disabled until iwl_trans_pcie_release_nic_access */
		return ret;
	}
	local_bh_enable();
	return false;
}

void __releases(nic_access_nobh)
iwl_trans_pcie_release_nic_access(struct iwl_trans *trans)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);

	lockdep_assert_held(&trans_pcie->reg_lock);

	/*
	 * Fool sparse by faking we acquiring the lock - sparse will
	 * track nic_access anyway.
	 */
	__acquire(&trans_pcie->reg_lock);

	if (trans_pcie->cmd_hold_nic_awake)
		goto out;
	if (trans->mac_cfg->device_family >= IWL_DEVICE_FAMILY_BZ)
		iwl_trans_clear_bit(trans, CSR_GP_CNTRL,
				    CSR_GP_CNTRL_REG_FLAG_BZ_MAC_ACCESS_REQ);
	else
		iwl_trans_clear_bit(trans, CSR_GP_CNTRL,
				    CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
	/*
	 * Above we read the CSR_GP_CNTRL register, which will flush
	 * any previous writes, but we need the write that clears the
	 * MAC_ACCESS_REQ bit to be performed before any other writes
	 * scheduled on different CPUs (after we drop reg_lock).
	 */
out:
	__release(nic_access_nobh);
	spin_unlock_bh(&trans_pcie->reg_lock);
}

int iwl_trans_pcie_read_mem(struct iwl_trans *trans, u32 addr,
			    void *buf, int dwords)
{
#define IWL_MAX_HW_ERRS 5
	unsigned int num_consec_hw_errors = 0;
	int offs = 0;
	u32 *vals = buf;

	while (offs < dwords) {
		/* limit the time we spin here under lock to 1/2s */
		unsigned long end = jiffies + HZ / 2;
		bool resched = false;

		if (iwl_trans_grab_nic_access(trans)) {
			iwl_write32(trans, HBUS_TARG_MEM_RADDR,
				    addr + 4 * offs);

			while (offs < dwords) {
				vals[offs] = iwl_read32(trans,
							HBUS_TARG_MEM_RDAT);

				if (iwl_trans_is_hw_error_value(vals[offs]))
					num_consec_hw_errors++;
				else
					num_consec_hw_errors = 0;

				if (num_consec_hw_errors >= IWL_MAX_HW_ERRS) {
					iwl_trans_release_nic_access(trans);
					return -EIO;
				}

				offs++;

				if (time_after(jiffies, end)) {
					resched = true;
					break;
				}
			}
			iwl_trans_release_nic_access(trans);

			if (resched)
				cond_resched();
		} else {
			return -EBUSY;
		}
	}

	return 0;
}

int iwl_trans_pcie_read_config32(struct iwl_trans *trans, u32 ofs,
				 u32 *val)
{
	return pci_read_config_dword(IWL_TRANS_GET_PCIE_TRANS(trans)->pci_dev,
				     ofs, val);
}

#define IWL_FLUSH_WAIT_MS	2000

int iwl_trans_pcie_rxq_dma_data(struct iwl_trans *trans, int queue,
				struct iwl_trans_rxq_dma_data *data)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);

	if (queue >= trans->info.num_rxqs || !trans_pcie->rxq)
		return -EINVAL;

	data->fr_bd_cb = trans_pcie->rxq[queue].bd_dma;
	data->urbd_stts_wrptr = trans_pcie->rxq[queue].rb_stts_dma;
	data->ur_bd_cb = trans_pcie->rxq[queue].used_bd_dma;
	data->fr_bd_wid = 0;

	return 0;
}

int iwl_trans_pcie_wait_txq_empty(struct iwl_trans *trans, int txq_idx)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	struct iwl_txq *txq;
	unsigned long now = jiffies;
	bool overflow_tx;
	u8 wr_ptr;

	/* Make sure the NIC is still alive in the bus */
	if (test_bit(STATUS_TRANS_DEAD, &trans->status))
		return -ENODEV;

	if (!test_bit(txq_idx, trans_pcie->txqs.queue_used))
		return -EINVAL;

	IWL_DEBUG_TX_QUEUES(trans, "Emptying queue %d...\n", txq_idx);
	txq = trans_pcie->txqs.txq[txq_idx];

	spin_lock_bh(&txq->lock);
	overflow_tx = txq->overflow_tx ||
		      !skb_queue_empty(&txq->overflow_q);
	spin_unlock_bh(&txq->lock);

	wr_ptr = READ_ONCE(txq->write_ptr);

	while ((txq->read_ptr != READ_ONCE(txq->write_ptr) ||
		overflow_tx) &&
	       !time_after(jiffies,
			   now + msecs_to_jiffies(IWL_FLUSH_WAIT_MS))) {
		u8 write_ptr = READ_ONCE(txq->write_ptr);

		/*
		 * If write pointer moved during the wait, warn only
		 * if the TX came from op mode. In case TX came from
		 * trans layer (overflow TX) don't warn.
		 */
		if (WARN_ONCE(wr_ptr != write_ptr && !overflow_tx,
			      "WR pointer moved while flushing %d -> %d\n",
			      wr_ptr, write_ptr))
			return -ETIMEDOUT;
		wr_ptr = write_ptr;

		usleep_range(1000, 2000);

		spin_lock_bh(&txq->lock);
		overflow_tx = txq->overflow_tx ||
			      !skb_queue_empty(&txq->overflow_q);
		spin_unlock_bh(&txq->lock);
	}

	if (txq->read_ptr != txq->write_ptr) {
		IWL_ERR(trans,
			"fail to flush all tx fifo queues Q %d\n", txq_idx);
		iwl_txq_log_scd_error(trans, txq);
		return -ETIMEDOUT;
	}

	IWL_DEBUG_TX_QUEUES(trans, "Queue %d is now empty.\n", txq_idx);

	return 0;
}

int iwl_trans_pcie_wait_txqs_empty(struct iwl_trans *trans, u32 txq_bm)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	int cnt;
	int ret = 0;

	/* waiting for all the tx frames complete might take a while */
	for (cnt = 0;
	     cnt < trans->mac_cfg->base->num_of_queues;
	     cnt++) {

		if (cnt == trans->conf.cmd_queue)
			continue;
		if (!test_bit(cnt, trans_pcie->txqs.queue_used))
			continue;
		if (!(BIT(cnt) & txq_bm))
			continue;

		ret = iwl_trans_pcie_wait_txq_empty(trans, cnt);
		if (ret)
			break;
	}

	return ret;
}

void iwl_trans_pcie_set_bits_mask(struct iwl_trans *trans, u32 reg,
				  u32 mask, u32 value)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);

	spin_lock_bh(&trans_pcie->reg_lock);
	_iwl_trans_set_bits_mask(trans, reg, mask, value);
	spin_unlock_bh(&trans_pcie->reg_lock);
}

static const char *get_csr_string(int cmd)
{
#define IWL_CMD(x) case x: return #x
	switch (cmd) {
	IWL_CMD(CSR_HW_IF_CONFIG_REG);
	IWL_CMD(CSR_INT_COALESCING);
	IWL_CMD(CSR_INT);
	IWL_CMD(CSR_INT_MASK);
	IWL_CMD(CSR_FH_INT_STATUS);
	IWL_CMD(CSR_GPIO_IN);
	IWL_CMD(CSR_RESET);
	IWL_CMD(CSR_GP_CNTRL);
	IWL_CMD(CSR_HW_REV);
	IWL_CMD(CSR_EEPROM_REG);
	IWL_CMD(CSR_EEPROM_GP);
	IWL_CMD(CSR_OTP_GP_REG);
	IWL_CMD(CSR_GIO_REG);
	IWL_CMD(CSR_GP_UCODE_REG);
	IWL_CMD(CSR_GP_DRIVER_REG);
	IWL_CMD(CSR_UCODE_DRV_GP1);
	IWL_CMD(CSR_UCODE_DRV_GP2);
	IWL_CMD(CSR_LED_REG);
	IWL_CMD(CSR_DRAM_INT_TBL_REG);
	IWL_CMD(CSR_GIO_CHICKEN_BITS);
	IWL_CMD(CSR_ANA_PLL_CFG);
	IWL_CMD(CSR_HW_REV_WA_REG);
	IWL_CMD(CSR_MONITOR_STATUS_REG);
	IWL_CMD(CSR_DBG_HPET_MEM_REG);
	default:
		return "UNKNOWN";
	}
#undef IWL_CMD
}

void iwl_pcie_dump_csr(struct iwl_trans *trans)
{
	int i;
	static const u32 csr_tbl[] = {
		CSR_HW_IF_CONFIG_REG,
		CSR_INT_COALESCING,
		CSR_INT,
		CSR_INT_MASK,
		CSR_FH_INT_STATUS,
		CSR_GPIO_IN,
		CSR_RESET,
		CSR_GP_CNTRL,
		CSR_HW_REV,
		CSR_EEPROM_REG,
		CSR_EEPROM_GP,
		CSR_OTP_GP_REG,
		CSR_GIO_REG,
		CSR_GP_UCODE_REG,
		CSR_GP_DRIVER_REG,
		CSR_UCODE_DRV_GP1,
		CSR_UCODE_DRV_GP2,
		CSR_LED_REG,
		CSR_DRAM_INT_TBL_REG,
		CSR_GIO_CHICKEN_BITS,
		CSR_ANA_PLL_CFG,
		CSR_MONITOR_STATUS_REG,
		CSR_HW_REV_WA_REG,
		CSR_DBG_HPET_MEM_REG
	};
	IWL_ERR(trans, "CSR values:\n");
	IWL_ERR(trans, "(2nd byte of CSR_INT_COALESCING is "
		"CSR_INT_PERIODIC_REG)\n");
	for (i = 0; i <  ARRAY_SIZE(csr_tbl); i++) {
		IWL_ERR(trans, "  %25s: 0X%08x\n",
			get_csr_string(csr_tbl[i]),
			iwl_read32(trans, csr_tbl[i]));
	}
}

#ifdef CONFIG_IWLWIFI_DEBUGFS
/* create and remove of files */
#define DEBUGFS_ADD_FILE(name, parent, mode) do {			\
	debugfs_create_file(#name, mode, parent, trans,			\
			    &iwl_dbgfs_##name##_ops);			\
} while (0)

/* file operation */
#define DEBUGFS_READ_FILE_OPS(name)					\
static const struct file_operations iwl_dbgfs_##name##_ops = {		\
	.read = iwl_dbgfs_##name##_read,				\
	.open = simple_open,						\
	.llseek = generic_file_llseek,					\
};

#define DEBUGFS_WRITE_FILE_OPS(name)                                    \
static const struct file_operations iwl_dbgfs_##name##_ops = {          \
	.write = iwl_dbgfs_##name##_write,                              \
	.open = simple_open,						\
	.llseek = generic_file_llseek,					\
};

#define DEBUGFS_READ_WRITE_FILE_OPS(name)				\
static const struct file_operations iwl_dbgfs_##name##_ops = {		\
	.write = iwl_dbgfs_##name##_write,				\
	.read = iwl_dbgfs_##name##_read,				\
	.open = simple_open,						\
	.llseek = generic_file_llseek,					\
};

struct iwl_dbgfs_tx_queue_priv {
	struct iwl_trans *trans;
};

struct iwl_dbgfs_tx_queue_state {
	loff_t pos;
};

static void *iwl_dbgfs_tx_queue_seq_start(struct seq_file *seq, loff_t *pos)
{
	struct iwl_dbgfs_tx_queue_priv *priv = seq->private;
	struct iwl_dbgfs_tx_queue_state *state;

	if (*pos >= priv->trans->mac_cfg->base->num_of_queues)
		return NULL;

	state = kmalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return NULL;
	state->pos = *pos;
	return state;
}

static void *iwl_dbgfs_tx_queue_seq_next(struct seq_file *seq,
					 void *v, loff_t *pos)
{
	struct iwl_dbgfs_tx_queue_priv *priv = seq->private;
	struct iwl_dbgfs_tx_queue_state *state = v;

	*pos = ++state->pos;

	if (*pos >= priv->trans->mac_cfg->base->num_of_queues)
		return NULL;

	return state;
}

static void iwl_dbgfs_tx_queue_seq_stop(struct seq_file *seq, void *v)
{
	kfree(v);
}

static int iwl_dbgfs_tx_queue_seq_show(struct seq_file *seq, void *v)
{
	struct iwl_dbgfs_tx_queue_priv *priv = seq->private;
	struct iwl_dbgfs_tx_queue_state *state = v;
	struct iwl_trans *trans = priv->trans;
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	struct iwl_txq *txq = trans_pcie->txqs.txq[state->pos];

	seq_printf(seq, "hwq %.3u: used=%d stopped=%d ",
		   (unsigned int)state->pos,
		   !!test_bit(state->pos, trans_pcie->txqs.queue_used),
		   !!test_bit(state->pos, trans_pcie->txqs.queue_stopped));
	if (txq)
		seq_printf(seq,
			   "read=%u write=%u need_update=%d frozen=%d n_window=%d ampdu=%d",
			   txq->read_ptr, txq->write_ptr,
			   txq->need_update, txq->frozen,
			   txq->n_window, txq->ampdu);
	else
		seq_puts(seq, "(unallocated)");

	if (state->pos == trans->conf.cmd_queue)
		seq_puts(seq, " (HCMD)");
	seq_puts(seq, "\n");

	return 0;
}

static const struct seq_operations iwl_dbgfs_tx_queue_seq_ops = {
	.start = iwl_dbgfs_tx_queue_seq_start,
	.next = iwl_dbgfs_tx_queue_seq_next,
	.stop = iwl_dbgfs_tx_queue_seq_stop,
	.show = iwl_dbgfs_tx_queue_seq_show,
};

static int iwl_dbgfs_tx_queue_open(struct inode *inode, struct file *filp)
{
	struct iwl_dbgfs_tx_queue_priv *priv;

	priv = __seq_open_private(filp, &iwl_dbgfs_tx_queue_seq_ops,
				  sizeof(*priv));

	if (!priv)
		return -ENOMEM;

	priv->trans = inode->i_private;
	return 0;
}

static ssize_t iwl_dbgfs_rx_queue_read(struct file *file,
				       char __user *user_buf,
				       size_t count, loff_t *ppos)
{
	struct iwl_trans *trans = file->private_data;
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	char *buf;
	int pos = 0, i, ret;
	size_t bufsz;

	bufsz = sizeof(char) * 121 * trans->info.num_rxqs;

	if (!trans_pcie->rxq)
		return -EAGAIN;

	buf = kzalloc(bufsz, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (i = 0; i < trans->info.num_rxqs && pos < bufsz; i++) {
		struct iwl_rxq *rxq = &trans_pcie->rxq[i];

		spin_lock_bh(&rxq->lock);

		pos += scnprintf(buf + pos, bufsz - pos, "queue#: %2d\n",
				 i);
		pos += scnprintf(buf + pos, bufsz - pos, "\tread: %u\n",
				 rxq->read);
		pos += scnprintf(buf + pos, bufsz - pos, "\twrite: %u\n",
				 rxq->write);
		pos += scnprintf(buf + pos, bufsz - pos, "\twrite_actual: %u\n",
				 rxq->write_actual);
		pos += scnprintf(buf + pos, bufsz - pos, "\tneed_update: %2d\n",
				 rxq->need_update);
		pos += scnprintf(buf + pos, bufsz - pos, "\tfree_count: %u\n",
				 rxq->free_count);
		if (rxq->rb_stts) {
			u32 r =	iwl_get_closed_rb_stts(trans, rxq);
			pos += scnprintf(buf + pos, bufsz - pos,
					 "\tclosed_rb_num: %u\n", r);
		} else {
			pos += scnprintf(buf + pos, bufsz - pos,
					 "\tclosed_rb_num: Not Allocated\n");
		}
		spin_unlock_bh(&rxq->lock);
	}
	ret = simple_read_from_buffer(user_buf, count, ppos, buf, pos);
	kfree(buf);

	return ret;
}

static ssize_t iwl_dbgfs_interrupt_read(struct file *file,
					char __user *user_buf,
					size_t count, loff_t *ppos)
{
	struct iwl_trans *trans = file->private_data;
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	struct isr_statistics *isr_stats = &trans_pcie->isr_stats;

	int pos = 0;
	char *buf;
	int bufsz = 24 * 64; /* 24 items * 64 char per item */
	ssize_t ret;

	buf = kzalloc(bufsz, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	pos += scnprintf(buf + pos, bufsz - pos,
			"Interrupt Statistics Report:\n");

	pos += scnprintf(buf + pos, bufsz - pos, "HW Error:\t\t\t %u\n",
		isr_stats->hw);
	pos += scnprintf(buf + pos, bufsz - pos, "SW Error:\t\t\t %u\n",
		isr_stats->sw);
	if (isr_stats->sw || isr_stats->hw) {
		pos += scnprintf(buf + pos, bufsz - pos,
			"\tLast Restarting Code:  0x%X\n",
			isr_stats->err_code);
	}
#ifdef CONFIG_IWLWIFI_DEBUG
	pos += scnprintf(buf + pos, bufsz - pos, "Frame transmitted:\t\t %u\n",
		isr_stats->sch);
	pos += scnprintf(buf + pos, bufsz - pos, "Alive interrupt:\t\t %u\n",
		isr_stats->alive);
#endif
	pos += scnprintf(buf + pos, bufsz - pos,
		"HW RF KILL switch toggled:\t %u\n", isr_stats->rfkill);

	pos += scnprintf(buf + pos, bufsz - pos, "CT KILL:\t\t\t %u\n",
		isr_stats->ctkill);

	pos += scnprintf(buf + pos, bufsz - pos, "Wakeup Interrupt:\t\t %u\n",
		isr_stats->wakeup);

	pos += scnprintf(buf + pos, bufsz - pos,
		"Rx command responses:\t\t %u\n", isr_stats->rx);

	pos += scnprintf(buf + pos, bufsz - pos, "Tx/FH interrupt:\t\t %u\n",
		isr_stats->tx);

	pos += scnprintf(buf + pos, bufsz - pos, "Unexpected INTA:\t\t %u\n",
		isr_stats->unhandled);

	ret = simple_read_from_buffer(user_buf, count, ppos, buf, pos);
	kfree(buf);
	return ret;
}

static ssize_t iwl_dbgfs_interrupt_write(struct file *file,
					 const char __user *user_buf,
					 size_t count, loff_t *ppos)
{
	struct iwl_trans *trans = file->private_data;
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	struct isr_statistics *isr_stats = &trans_pcie->isr_stats;
	u32 reset_flag;
	int ret;

	ret = kstrtou32_from_user(user_buf, count, 16, &reset_flag);
	if (ret)
		return ret;
	if (reset_flag == 0)
		memset(isr_stats, 0, sizeof(*isr_stats));

	return count;
}

static ssize_t iwl_dbgfs_csr_write(struct file *file,
				   const char __user *user_buf,
				   size_t count, loff_t *ppos)
{
	struct iwl_trans *trans = file->private_data;

	iwl_pcie_dump_csr(trans);

	return count;
}

static ssize_t iwl_dbgfs_fh_reg_read(struct file *file,
				     char __user *user_buf,
				     size_t count, loff_t *ppos)
{
	struct iwl_trans *trans = file->private_data;
	char *buf = NULL;
	ssize_t ret;

	ret = iwl_dump_fh(trans, &buf);
	if (ret < 0)
		return ret;
	if (!buf)
		return -EINVAL;
	ret = simple_read_from_buffer(user_buf, count, ppos, buf, ret);
	kfree(buf);
	return ret;
}

static ssize_t iwl_dbgfs_rfkill_read(struct file *file,
				     char __user *user_buf,
				     size_t count, loff_t *ppos)
{
	struct iwl_trans *trans = file->private_data;
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	char buf[100];
	int pos;

	pos = scnprintf(buf, sizeof(buf), "debug: %d\nhw: %d\n",
			trans_pcie->debug_rfkill,
			!(iwl_read32(trans, CSR_GP_CNTRL) &
				CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW));

	return simple_read_from_buffer(user_buf, count, ppos, buf, pos);
}

static ssize_t iwl_dbgfs_rfkill_write(struct file *file,
				      const char __user *user_buf,
				      size_t count, loff_t *ppos)
{
	struct iwl_trans *trans = file->private_data;
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	bool new_value;
	int ret;

	ret = kstrtobool_from_user(user_buf, count, &new_value);
	if (ret)
		return ret;
	if (new_value == trans_pcie->debug_rfkill)
		return count;
	IWL_WARN(trans, "changing debug rfkill %d->%d\n",
		 trans_pcie->debug_rfkill, new_value);
	trans_pcie->debug_rfkill = new_value;
	iwl_pcie_handle_rfkill_irq(trans, false);

	return count;
}

static int iwl_dbgfs_monitor_data_open(struct inode *inode,
				       struct file *file)
{
	struct iwl_trans *trans = inode->i_private;
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);

	if (!trans->dbg.dest_tlv ||
	    trans->dbg.dest_tlv->monitor_mode != EXTERNAL_MODE) {
		IWL_ERR(trans, "Debug destination is not set to DRAM\n");
		return -ENOENT;
	}

	if (trans_pcie->fw_mon_data.state != IWL_FW_MON_DBGFS_STATE_CLOSED)
		return -EBUSY;

	trans_pcie->fw_mon_data.state = IWL_FW_MON_DBGFS_STATE_OPEN;
	return simple_open(inode, file);
}

static int iwl_dbgfs_monitor_data_release(struct inode *inode,
					  struct file *file)
{
	struct iwl_trans_pcie *trans_pcie =
		IWL_TRANS_GET_PCIE_TRANS(inode->i_private);

	if (trans_pcie->fw_mon_data.state == IWL_FW_MON_DBGFS_STATE_OPEN)
		trans_pcie->fw_mon_data.state = IWL_FW_MON_DBGFS_STATE_CLOSED;
	return 0;
}

static bool iwl_write_to_user_buf(char __user *user_buf, ssize_t count,
				  void *buf, ssize_t *size,
				  ssize_t *bytes_copied)
{
	ssize_t buf_size_left = count - *bytes_copied;

	buf_size_left = buf_size_left - (buf_size_left % sizeof(u32));
	if (*size > buf_size_left)
		*size = buf_size_left;

	*size -= copy_to_user(user_buf, buf, *size);
	*bytes_copied += *size;

	if (buf_size_left == *size)
		return true;
	return false;
}

static ssize_t iwl_dbgfs_monitor_data_read(struct file *file,
					   char __user *user_buf,
					   size_t count, loff_t *ppos)
{
	struct iwl_trans *trans = file->private_data;
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	u8 *cpu_addr = (void *)trans->dbg.fw_mon.block, *curr_buf;
	struct cont_rec *data = &trans_pcie->fw_mon_data;
	u32 write_ptr_addr, wrap_cnt_addr, write_ptr, wrap_cnt;
	ssize_t size, bytes_copied = 0;
	bool b_full;

	if (trans->dbg.dest_tlv) {
		write_ptr_addr =
			le32_to_cpu(trans->dbg.dest_tlv->write_ptr_reg);
		wrap_cnt_addr = le32_to_cpu(trans->dbg.dest_tlv->wrap_count);
	} else {
		write_ptr_addr = MON_BUFF_WRPTR;
		wrap_cnt_addr = MON_BUFF_CYCLE_CNT;
	}

	if (unlikely(!trans->dbg.rec_on))
		return 0;

	mutex_lock(&data->mutex);
	if (data->state ==
	    IWL_FW_MON_DBGFS_STATE_DISABLED) {
		mutex_unlock(&data->mutex);
		return 0;
	}

	/* write_ptr position in bytes rather then DW */
	write_ptr = iwl_read_prph(trans, write_ptr_addr) * sizeof(u32);
	wrap_cnt = iwl_read_prph(trans, wrap_cnt_addr);

	if (data->prev_wrap_cnt == wrap_cnt) {
		size = write_ptr - data->prev_wr_ptr;
		curr_buf = cpu_addr + data->prev_wr_ptr;
		b_full = iwl_write_to_user_buf(user_buf, count,
					       curr_buf, &size,
					       &bytes_copied);
		data->prev_wr_ptr += size;

	} else if (data->prev_wrap_cnt == wrap_cnt - 1 &&
		   write_ptr < data->prev_wr_ptr) {
		size = trans->dbg.fw_mon.size - data->prev_wr_ptr;
		curr_buf = cpu_addr + data->prev_wr_ptr;
		b_full = iwl_write_to_user_buf(user_buf, count,
					       curr_buf, &size,
					       &bytes_copied);
		data->prev_wr_ptr += size;

		if (!b_full) {
			size = write_ptr;
			b_full = iwl_write_to_user_buf(user_buf, count,
						       cpu_addr, &size,
						       &bytes_copied);
			data->prev_wr_ptr = size;
			data->prev_wrap_cnt++;
		}
	} else {
		if (data->prev_wrap_cnt == wrap_cnt - 1 &&
		    write_ptr > data->prev_wr_ptr)
			IWL_WARN(trans,
				 "write pointer passed previous write pointer, start copying from the beginning\n");
		else if (!unlikely(data->prev_wrap_cnt == 0 &&
				   data->prev_wr_ptr == 0))
			IWL_WARN(trans,
				 "monitor data is out of sync, start copying from the beginning\n");

		size = write_ptr;
		b_full = iwl_write_to_user_buf(user_buf, count,
					       cpu_addr, &size,
					       &bytes_copied);
		data->prev_wr_ptr = size;
		data->prev_wrap_cnt = wrap_cnt;
	}

	mutex_unlock(&data->mutex);

	return bytes_copied;
}

static ssize_t iwl_dbgfs_rf_read(struct file *file,
				 char __user *user_buf,
				 size_t count, loff_t *ppos)
{
	struct iwl_trans *trans = file->private_data;
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);

	if (!trans_pcie->rf_name[0])
		return -ENODEV;

	return simple_read_from_buffer(user_buf, count, ppos,
				       trans_pcie->rf_name,
				       strlen(trans_pcie->rf_name));
}

static ssize_t iwl_dbgfs_reset_write(struct file *file,
				     const char __user *user_buf,
				     size_t count, loff_t *ppos)
{
	struct iwl_trans *trans = file->private_data;
	static const char * const modes[] = {
		[IWL_RESET_MODE_SW_RESET] = "sw",
		[IWL_RESET_MODE_REPROBE] = "reprobe",
		[IWL_RESET_MODE_TOP_RESET] = "top",
		[IWL_RESET_MODE_REMOVE_ONLY] = "remove",
		[IWL_RESET_MODE_RESCAN] = "rescan",
		[IWL_RESET_MODE_FUNC_RESET] = "function",
		[IWL_RESET_MODE_PROD_RESET] = "product",
	};
	char buf[10] = {};
	int mode;

	if (count > sizeof(buf) - 1)
		return -EINVAL;

	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;

	mode = sysfs_match_string(modes, buf);
	if (mode < 0)
		return mode;

	if (mode < IWL_RESET_MODE_REMOVE_ONLY) {
		if (!test_bit(STATUS_DEVICE_ENABLED, &trans->status))
			return -EINVAL;
		if (mode == IWL_RESET_MODE_TOP_RESET) {
			if (trans->mac_cfg->device_family < IWL_DEVICE_FAMILY_SC)
				return -EINVAL;
			trans->request_top_reset = 1;
		}
		iwl_op_mode_nic_error(trans->op_mode, IWL_ERR_TYPE_DEBUGFS);
		iwl_trans_schedule_reset(trans, IWL_ERR_TYPE_DEBUGFS);
		return count;
	}

	iwl_trans_pcie_reset(trans, mode);

	return count;
}

DEBUGFS_READ_WRITE_FILE_OPS(interrupt);
DEBUGFS_READ_FILE_OPS(fh_reg);
DEBUGFS_READ_FILE_OPS(rx_queue);
DEBUGFS_WRITE_FILE_OPS(csr);
DEBUGFS_READ_WRITE_FILE_OPS(rfkill);
DEBUGFS_READ_FILE_OPS(rf);
DEBUGFS_WRITE_FILE_OPS(reset);

static const struct file_operations iwl_dbgfs_tx_queue_ops = {
	.owner = THIS_MODULE,
	.open = iwl_dbgfs_tx_queue_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release_private,
};

static const struct file_operations iwl_dbgfs_monitor_data_ops = {
	.read = iwl_dbgfs_monitor_data_read,
	.open = iwl_dbgfs_monitor_data_open,
	.release = iwl_dbgfs_monitor_data_release,
};

/* Create the debugfs files and directories */
void iwl_trans_pcie_dbgfs_register(struct iwl_trans *trans)
{
	struct dentry *dir = trans->dbgfs_dir;

	DEBUGFS_ADD_FILE(rx_queue, dir, 0400);
	DEBUGFS_ADD_FILE(tx_queue, dir, 0400);
	DEBUGFS_ADD_FILE(interrupt, dir, 0600);
	DEBUGFS_ADD_FILE(csr, dir, 0200);
	DEBUGFS_ADD_FILE(fh_reg, dir, 0400);
	DEBUGFS_ADD_FILE(rfkill, dir, 0600);
	DEBUGFS_ADD_FILE(monitor_data, dir, 0400);
	DEBUGFS_ADD_FILE(rf, dir, 0400);
	DEBUGFS_ADD_FILE(reset, dir, 0200);
}

void iwl_trans_pcie_debugfs_cleanup(struct iwl_trans *trans)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	struct cont_rec *data = &trans_pcie->fw_mon_data;

	mutex_lock(&data->mutex);
	data->state = IWL_FW_MON_DBGFS_STATE_DISABLED;
	mutex_unlock(&data->mutex);
}
#endif /*CONFIG_IWLWIFI_DEBUGFS */

static u32 iwl_trans_pcie_get_cmdlen(struct iwl_trans *trans, void *tfd)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	u32 cmdlen = 0;
	int i;

	for (i = 0; i < trans_pcie->txqs.tfd.max_tbs; i++)
		cmdlen += iwl_txq_gen1_tfd_tb_get_len(trans, tfd, i);

	return cmdlen;
}

static u32 iwl_trans_pcie_dump_rbs(struct iwl_trans *trans,
				   struct iwl_fw_error_dump_data **data,
				   int allocated_rb_nums)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	int max_len = trans_pcie->rx_buf_bytes;
	/* Dump RBs is supported only for pre-9000 devices (1 queue) */
	struct iwl_rxq *rxq = &trans_pcie->rxq[0];
	u32 i, r, j, rb_len = 0;

	spin_lock_bh(&rxq->lock);

	r = iwl_get_closed_rb_stts(trans, rxq);

	for (i = rxq->read, j = 0;
	     i != r && j < allocated_rb_nums;
	     i = (i + 1) & RX_QUEUE_MASK, j++) {
		struct iwl_rx_mem_buffer *rxb = rxq->queue[i];
		struct iwl_fw_error_dump_rb *rb;

		dma_sync_single_for_cpu(trans->dev, rxb->page_dma,
					max_len, DMA_FROM_DEVICE);

		rb_len += sizeof(**data) + sizeof(*rb) + max_len;

		(*data)->type = cpu_to_le32(IWL_FW_ERROR_DUMP_RB);
		(*data)->len = cpu_to_le32(sizeof(*rb) + max_len);
		rb = (void *)(*data)->data;
		rb->index = cpu_to_le32(i);
		memcpy(rb->data, page_address(rxb->page), max_len);

		*data = iwl_fw_error_next_data(*data);
	}

	spin_unlock_bh(&rxq->lock);

	return rb_len;
}
#define IWL_CSR_TO_DUMP (0x250)

static u32 iwl_trans_pcie_dump_csr(struct iwl_trans *trans,
				   struct iwl_fw_error_dump_data **data)
{
	u32 csr_len = sizeof(**data) + IWL_CSR_TO_DUMP;
	__le32 *val;
	int i;

	(*data)->type = cpu_to_le32(IWL_FW_ERROR_DUMP_CSR);
	(*data)->len = cpu_to_le32(IWL_CSR_TO_DUMP);
	val = (void *)(*data)->data;

	for (i = 0; i < IWL_CSR_TO_DUMP; i += 4)
		*val++ = cpu_to_le32(iwl_trans_pcie_read32(trans, i));

	*data = iwl_fw_error_next_data(*data);

	return csr_len;
}

static u32 iwl_trans_pcie_fh_regs_dump(struct iwl_trans *trans,
				       struct iwl_fw_error_dump_data **data)
{
	u32 fh_regs_len = FH_MEM_UPPER_BOUND - FH_MEM_LOWER_BOUND;
	__le32 *val;
	int i;

	if (!iwl_trans_grab_nic_access(trans))
		return 0;

	(*data)->type = cpu_to_le32(IWL_FW_ERROR_DUMP_FH_REGS);
	(*data)->len = cpu_to_le32(fh_regs_len);
	val = (void *)(*data)->data;

	if (!trans->mac_cfg->gen2)
		for (i = FH_MEM_LOWER_BOUND; i < FH_MEM_UPPER_BOUND;
		     i += sizeof(u32))
			*val++ = cpu_to_le32(iwl_trans_pcie_read32(trans, i));
	else
		for (i = iwl_umac_prph(trans, FH_MEM_LOWER_BOUND_GEN2);
		     i < iwl_umac_prph(trans, FH_MEM_UPPER_BOUND_GEN2);
		     i += sizeof(u32))
			*val++ = cpu_to_le32(iwl_trans_pcie_read_prph(trans,
								      i));

	iwl_trans_release_nic_access(trans);

	*data = iwl_fw_error_next_data(*data);

	return sizeof(**data) + fh_regs_len;
}

static u32
iwl_trans_pci_dump_marbh_monitor(struct iwl_trans *trans,
				 struct iwl_fw_error_dump_fw_mon *fw_mon_data,
				 u32 monitor_len)
{
	u32 buf_size_in_dwords = (monitor_len >> 2);
	u32 *buffer = (u32 *)fw_mon_data->data;
	u32 i;

	if (!iwl_trans_grab_nic_access(trans))
		return 0;

	iwl_write_umac_prph_no_grab(trans, MON_DMARB_RD_CTL_ADDR, 0x1);
	for (i = 0; i < buf_size_in_dwords; i++)
		buffer[i] = iwl_read_umac_prph_no_grab(trans,
						       MON_DMARB_RD_DATA_ADDR);
	iwl_write_umac_prph_no_grab(trans, MON_DMARB_RD_CTL_ADDR, 0x0);

	iwl_trans_release_nic_access(trans);

	return monitor_len;
}

static void
iwl_trans_pcie_dump_pointers(struct iwl_trans *trans,
			     struct iwl_fw_error_dump_fw_mon *fw_mon_data)
{
	u32 base, base_high, write_ptr, write_ptr_val, wrap_cnt;

	if (trans->mac_cfg->device_family >= IWL_DEVICE_FAMILY_AX210) {
		base = DBGC_CUR_DBGBUF_BASE_ADDR_LSB;
		base_high = DBGC_CUR_DBGBUF_BASE_ADDR_MSB;
		write_ptr = DBGC_CUR_DBGBUF_STATUS;
		wrap_cnt = DBGC_DBGBUF_WRAP_AROUND;
	} else if (trans->dbg.dest_tlv) {
		write_ptr = le32_to_cpu(trans->dbg.dest_tlv->write_ptr_reg);
		wrap_cnt = le32_to_cpu(trans->dbg.dest_tlv->wrap_count);
		base = le32_to_cpu(trans->dbg.dest_tlv->base_reg);
	} else {
		base = MON_BUFF_BASE_ADDR;
		write_ptr = MON_BUFF_WRPTR;
		wrap_cnt = MON_BUFF_CYCLE_CNT;
	}

	write_ptr_val = iwl_read_prph(trans, write_ptr);
	fw_mon_data->fw_mon_cycle_cnt =
		cpu_to_le32(iwl_read_prph(trans, wrap_cnt));
	fw_mon_data->fw_mon_base_ptr =
		cpu_to_le32(iwl_read_prph(trans, base));
	if (trans->mac_cfg->device_family >= IWL_DEVICE_FAMILY_AX210) {
		fw_mon_data->fw_mon_base_high_ptr =
			cpu_to_le32(iwl_read_prph(trans, base_high));
		write_ptr_val &= DBGC_CUR_DBGBUF_STATUS_OFFSET_MSK;
		/* convert wrtPtr to DWs, to align with all HWs */
		write_ptr_val >>= 2;
	}
	fw_mon_data->fw_mon_wr_ptr = cpu_to_le32(write_ptr_val);
}

static u32
iwl_trans_pcie_dump_monitor(struct iwl_trans *trans,
			    struct iwl_fw_error_dump_data **data,
			    u32 monitor_len)
{
	struct iwl_dram_data *fw_mon = &trans->dbg.fw_mon;
	u32 len = 0;

	if (trans->dbg.dest_tlv ||
	    (fw_mon->size &&
	     (trans->mac_cfg->device_family == IWL_DEVICE_FAMILY_7000 ||
	      trans->mac_cfg->device_family >= IWL_DEVICE_FAMILY_AX210))) {
		struct iwl_fw_error_dump_fw_mon *fw_mon_data;

		(*data)->type = cpu_to_le32(IWL_FW_ERROR_DUMP_FW_MONITOR);
		fw_mon_data = (void *)(*data)->data;

		iwl_trans_pcie_dump_pointers(trans, fw_mon_data);

		len += sizeof(**data) + sizeof(*fw_mon_data);
		if (fw_mon->size) {
			memcpy(fw_mon_data->data, fw_mon->block, fw_mon->size);
			monitor_len = fw_mon->size;
		} else if (trans->dbg.dest_tlv->monitor_mode == SMEM_MODE) {
			u32 base = le32_to_cpu(fw_mon_data->fw_mon_base_ptr);
			/*
			 * Update pointers to reflect actual values after
			 * shifting
			 */
			if (trans->dbg.dest_tlv->version) {
				base = (iwl_read_prph(trans, base) &
					IWL_LDBG_M2S_BUF_BA_MSK) <<
				       trans->dbg.dest_tlv->base_shift;
				base *= IWL_M2S_UNIT_SIZE;
				base += trans->mac_cfg->base->smem_offset;
			} else {
				base = iwl_read_prph(trans, base) <<
				       trans->dbg.dest_tlv->base_shift;
			}

			iwl_trans_pcie_read_mem(trans, base, fw_mon_data->data,
						monitor_len / sizeof(u32));
		} else if (trans->dbg.dest_tlv->monitor_mode == MARBH_MODE) {
			monitor_len =
				iwl_trans_pci_dump_marbh_monitor(trans,
								 fw_mon_data,
								 monitor_len);
		} else {
			/* Didn't match anything - output no monitor data */
			monitor_len = 0;
		}

		len += monitor_len;
		(*data)->len = cpu_to_le32(monitor_len + sizeof(*fw_mon_data));
	}

	return len;
}

static int iwl_trans_get_fw_monitor_len(struct iwl_trans *trans, u32 *len)
{
	if (trans->dbg.fw_mon.size) {
		*len += sizeof(struct iwl_fw_error_dump_data) +
			sizeof(struct iwl_fw_error_dump_fw_mon) +
			trans->dbg.fw_mon.size;
		return trans->dbg.fw_mon.size;
	} else if (trans->dbg.dest_tlv) {
		u32 base, end, cfg_reg, monitor_len;

		if (trans->dbg.dest_tlv->version == 1) {
			cfg_reg = le32_to_cpu(trans->dbg.dest_tlv->base_reg);
			cfg_reg = iwl_read_prph(trans, cfg_reg);
			base = (cfg_reg & IWL_LDBG_M2S_BUF_BA_MSK) <<
				trans->dbg.dest_tlv->base_shift;
			base *= IWL_M2S_UNIT_SIZE;
			base += trans->mac_cfg->base->smem_offset;

			monitor_len =
				(cfg_reg & IWL_LDBG_M2S_BUF_SIZE_MSK) >>
				trans->dbg.dest_tlv->end_shift;
			monitor_len *= IWL_M2S_UNIT_SIZE;
		} else {
			base = le32_to_cpu(trans->dbg.dest_tlv->base_reg);
			end = le32_to_cpu(trans->dbg.dest_tlv->end_reg);

			base = iwl_read_prph(trans, base) <<
			       trans->dbg.dest_tlv->base_shift;
			end = iwl_read_prph(trans, end) <<
			      trans->dbg.dest_tlv->end_shift;

			/* Make "end" point to the actual end */
			if (trans->mac_cfg->device_family >=
			    IWL_DEVICE_FAMILY_8000 ||
			    trans->dbg.dest_tlv->monitor_mode == MARBH_MODE)
				end += (1 << trans->dbg.dest_tlv->end_shift);
			monitor_len = end - base;
		}
		*len += sizeof(struct iwl_fw_error_dump_data) +
			sizeof(struct iwl_fw_error_dump_fw_mon) +
			monitor_len;
		return monitor_len;
	}
	return 0;
}

struct iwl_trans_dump_data *
iwl_trans_pcie_dump_data(struct iwl_trans *trans, u32 dump_mask,
			 const struct iwl_dump_sanitize_ops *sanitize_ops,
			 void *sanitize_ctx)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	struct iwl_fw_error_dump_data *data;
	struct iwl_txq *cmdq = trans_pcie->txqs.txq[trans->conf.cmd_queue];
	struct iwl_fw_error_dump_txcmd *txcmd;
	struct iwl_trans_dump_data *dump_data;
	u32 len, num_rbs = 0, monitor_len = 0;
	int i, ptr;
	bool dump_rbs = test_bit(STATUS_FW_ERROR, &trans->status) &&
			!trans->mac_cfg->mq_rx_supported &&
			dump_mask & BIT(IWL_FW_ERROR_DUMP_RB);

	if (!dump_mask)
		return NULL;

	/* transport dump header */
	len = sizeof(*dump_data);

	/* host commands */
	if (dump_mask & BIT(IWL_FW_ERROR_DUMP_TXCMD) && cmdq)
		len += sizeof(*data) +
			cmdq->n_window * (sizeof(*txcmd) +
					  TFD_MAX_PAYLOAD_SIZE);

	/* FW monitor */
	if (dump_mask & BIT(IWL_FW_ERROR_DUMP_FW_MONITOR))
		monitor_len = iwl_trans_get_fw_monitor_len(trans, &len);

	/* CSR registers */
	if (dump_mask & BIT(IWL_FW_ERROR_DUMP_CSR))
		len += sizeof(*data) + IWL_CSR_TO_DUMP;

	/* FH registers */
	if (dump_mask & BIT(IWL_FW_ERROR_DUMP_FH_REGS)) {
		if (trans->mac_cfg->gen2)
			len += sizeof(*data) +
			       (iwl_umac_prph(trans, FH_MEM_UPPER_BOUND_GEN2) -
				iwl_umac_prph(trans, FH_MEM_LOWER_BOUND_GEN2));
		else
			len += sizeof(*data) +
			       (FH_MEM_UPPER_BOUND -
				FH_MEM_LOWER_BOUND);
	}

	if (dump_rbs) {
		/* Dump RBs is supported only for pre-9000 devices (1 queue) */
		struct iwl_rxq *rxq = &trans_pcie->rxq[0];
		/* RBs */
		spin_lock_bh(&rxq->lock);
		num_rbs = iwl_get_closed_rb_stts(trans, rxq);
		num_rbs = (num_rbs - rxq->read) & RX_QUEUE_MASK;
		spin_unlock_bh(&rxq->lock);

		len += num_rbs * (sizeof(*data) +
				  sizeof(struct iwl_fw_error_dump_rb) +
				  (PAGE_SIZE << trans_pcie->rx_page_order));
	}

	/* Paged memory for gen2 HW */
	if (trans->mac_cfg->gen2 && dump_mask & BIT(IWL_FW_ERROR_DUMP_PAGING))
		for (i = 0; i < trans->init_dram.paging_cnt; i++)
			len += sizeof(*data) +
			       sizeof(struct iwl_fw_error_dump_paging) +
			       trans->init_dram.paging[i].size;

	dump_data = vzalloc(len);
	if (!dump_data)
		return NULL;

	len = 0;
	data = (void *)dump_data->data;

	if (dump_mask & BIT(IWL_FW_ERROR_DUMP_TXCMD) && cmdq) {
		u16 tfd_size = trans_pcie->txqs.tfd.size;

		data->type = cpu_to_le32(IWL_FW_ERROR_DUMP_TXCMD);
		txcmd = (void *)data->data;
		spin_lock_bh(&cmdq->lock);
		ptr = cmdq->write_ptr;
		for (i = 0; i < cmdq->n_window; i++) {
			u8 idx = iwl_txq_get_cmd_index(cmdq, ptr);
			u8 tfdidx;
			u32 caplen, cmdlen;

			if (trans->mac_cfg->gen2)
				tfdidx = idx;
			else
				tfdidx = ptr;

			cmdlen = iwl_trans_pcie_get_cmdlen(trans,
							   (u8 *)cmdq->tfds +
							   tfd_size * tfdidx);
			caplen = min_t(u32, TFD_MAX_PAYLOAD_SIZE, cmdlen);

			if (cmdlen) {
				len += sizeof(*txcmd) + caplen;
				txcmd->cmdlen = cpu_to_le32(cmdlen);
				txcmd->caplen = cpu_to_le32(caplen);
				memcpy(txcmd->data, cmdq->entries[idx].cmd,
				       caplen);
				if (sanitize_ops && sanitize_ops->frob_hcmd)
					sanitize_ops->frob_hcmd(sanitize_ctx,
								txcmd->data,
								caplen);
				txcmd = (void *)((u8 *)txcmd->data + caplen);
			}

			ptr = iwl_txq_dec_wrap(trans, ptr);
		}
		spin_unlock_bh(&cmdq->lock);

		data->len = cpu_to_le32(len);
		len += sizeof(*data);
		data = iwl_fw_error_next_data(data);
	}

	if (dump_mask & BIT(IWL_FW_ERROR_DUMP_CSR))
		len += iwl_trans_pcie_dump_csr(trans, &data);
	if (dump_mask & BIT(IWL_FW_ERROR_DUMP_FH_REGS))
		len += iwl_trans_pcie_fh_regs_dump(trans, &data);
	if (dump_rbs)
		len += iwl_trans_pcie_dump_rbs(trans, &data, num_rbs);

	/* Paged memory for gen2 HW */
	if (trans->mac_cfg->gen2 &&
	    dump_mask & BIT(IWL_FW_ERROR_DUMP_PAGING)) {
		for (i = 0; i < trans->init_dram.paging_cnt; i++) {
			struct iwl_fw_error_dump_paging *paging;
			u32 page_len = trans->init_dram.paging[i].size;

			data->type = cpu_to_le32(IWL_FW_ERROR_DUMP_PAGING);
			data->len = cpu_to_le32(sizeof(*paging) + page_len);
			paging = (void *)data->data;
			paging->index = cpu_to_le32(i);
			memcpy(paging->data,
			       trans->init_dram.paging[i].block, page_len);
			data = iwl_fw_error_next_data(data);

			len += sizeof(*data) + sizeof(*paging) + page_len;
		}
	}
	if (dump_mask & BIT(IWL_FW_ERROR_DUMP_FW_MONITOR))
		len += iwl_trans_pcie_dump_monitor(trans, &data, monitor_len);

	dump_data->len = len;

	return dump_data;
}

void iwl_trans_pci_interrupts(struct iwl_trans *trans, bool enable)
{
	if (enable)
		iwl_enable_interrupts(trans);
	else
		iwl_disable_interrupts(trans);
}

void iwl_trans_pcie_sync_nmi(struct iwl_trans *trans)
{
	u32 inta_addr, sw_err_bit;
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);

	if (trans_pcie->msix_enabled) {
		inta_addr = CSR_MSIX_HW_INT_CAUSES_AD;
		if (trans->mac_cfg->device_family >= IWL_DEVICE_FAMILY_BZ)
			sw_err_bit = MSIX_HW_INT_CAUSES_REG_SW_ERR_BZ;
		else
			sw_err_bit = MSIX_HW_INT_CAUSES_REG_SW_ERR;
	} else {
		inta_addr = CSR_INT;
		sw_err_bit = CSR_INT_BIT_SW_ERR;
	}

	iwl_trans_sync_nmi_with_addr(trans, inta_addr, sw_err_bit);
}

static int iwl_trans_pcie_set_txcmd_info(const struct iwl_mac_cfg *mac_cfg,
					 unsigned int *txcmd_size,
					 unsigned int *txcmd_align)
{
	if (!mac_cfg->gen2) {
		*txcmd_size = sizeof(struct iwl_tx_cmd_v6);
		*txcmd_align = sizeof(void *);
	} else if (mac_cfg->device_family < IWL_DEVICE_FAMILY_AX210) {
		*txcmd_size = sizeof(struct iwl_tx_cmd_v9);
		*txcmd_align = 64;
	} else {
		*txcmd_size = sizeof(struct iwl_tx_cmd);
		*txcmd_align = 128;
	}

	*txcmd_size += sizeof(struct iwl_cmd_header);
	*txcmd_size += 36; /* biggest possible 802.11 header */

	/* Ensure device TX cmd cannot reach/cross a page boundary in gen2 */
	if (WARN_ON((mac_cfg->gen2 && *txcmd_size >= *txcmd_align)))
		return -EINVAL;

	return 0;
}

static struct iwl_trans *
iwl_trans_pcie_alloc(struct pci_dev *pdev,
		     const struct iwl_mac_cfg *mac_cfg,
		     struct iwl_trans_info *info, u8 __iomem *hw_base)
{
	struct iwl_trans_pcie *trans_pcie, **priv;
	unsigned int txcmd_size, txcmd_align;
	struct iwl_trans *trans;
	unsigned int bc_tbl_n_entries;
	int ret, addr_size;

	ret = iwl_trans_pcie_set_txcmd_info(mac_cfg, &txcmd_size,
					    &txcmd_align);
	if (ret)
		return ERR_PTR(ret);

	trans = iwl_trans_alloc(sizeof(struct iwl_trans_pcie), &pdev->dev,
				mac_cfg, txcmd_size, txcmd_align);
	if (!trans)
		return ERR_PTR(-ENOMEM);

	trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);

	trans_pcie->hw_base = hw_base;

	/* Initialize the wait queue for commands */
	init_waitqueue_head(&trans_pcie->wait_command_queue);

	if (trans->mac_cfg->gen2) {
		trans_pcie->txqs.tfd.addr_size = 64;
		trans_pcie->txqs.tfd.max_tbs = IWL_TFH_NUM_TBS;
		trans_pcie->txqs.tfd.size = sizeof(struct iwl_tfh_tfd);
	} else {
		trans_pcie->txqs.tfd.addr_size = 36;
		trans_pcie->txqs.tfd.max_tbs = IWL_NUM_OF_TBS;
		trans_pcie->txqs.tfd.size = sizeof(struct iwl_tfd);
	}

	trans_pcie->supported_dma_mask = (u32)DMA_BIT_MASK(12);
	if (trans->mac_cfg->device_family >= IWL_DEVICE_FAMILY_AX210)
		trans_pcie->supported_dma_mask = (u32)DMA_BIT_MASK(11);

	info->max_skb_frags = IWL_TRANS_PCIE_MAX_FRAGS(trans_pcie);

	trans_pcie->txqs.tso_hdr_page = alloc_percpu(struct iwl_tso_hdr_page);
	if (!trans_pcie->txqs.tso_hdr_page) {
		ret = -ENOMEM;
		goto out_free_trans;
	}

	if (trans->mac_cfg->device_family >= IWL_DEVICE_FAMILY_BZ)
		bc_tbl_n_entries = TFD_QUEUE_BC_SIZE_BZ;
	else if (trans->mac_cfg->device_family >= IWL_DEVICE_FAMILY_AX210)
		bc_tbl_n_entries = TFD_QUEUE_BC_SIZE_AX210;
	else
		bc_tbl_n_entries = TFD_QUEUE_BC_SIZE;

	trans_pcie->txqs.bc_tbl_size =
		sizeof(struct iwl_bc_tbl_entry) * bc_tbl_n_entries;
	/*
	 * For gen2 devices, we use a single allocation for each byte-count
	 * table, but they're pretty small (1k) so use a DMA pool that we
	 * allocate here.
	 */
	if (trans->mac_cfg->gen2) {
		trans_pcie->txqs.bc_pool =
			dmam_pool_create("iwlwifi:bc", trans->dev,
					 trans_pcie->txqs.bc_tbl_size,
					 256, 0);
		if (!trans_pcie->txqs.bc_pool) {
			ret = -ENOMEM;
			goto out_free_tso;
		}
	}

	/* Some things must not change even if the config does */
	WARN_ON(trans_pcie->txqs.tfd.addr_size !=
		(trans->mac_cfg->gen2 ? 64 : 36));

	/* Initialize NAPI here - it should be before registering to mac80211
	 * in the opmode but after the HW struct is allocated.
	 */
	trans_pcie->napi_dev = alloc_netdev_dummy(sizeof(struct iwl_trans_pcie *));
	if (!trans_pcie->napi_dev) {
		ret = -ENOMEM;
		goto out_free_tso;
	}
	/* The private struct in netdev is a pointer to struct iwl_trans_pcie */
	priv = netdev_priv(trans_pcie->napi_dev);
	*priv = trans_pcie;

	trans_pcie->trans = trans;
	trans_pcie->opmode_down = true;
	spin_lock_init(&trans_pcie->irq_lock);
	spin_lock_init(&trans_pcie->reg_lock);
	spin_lock_init(&trans_pcie->alloc_page_lock);
	mutex_init(&trans_pcie->mutex);
	init_waitqueue_head(&trans_pcie->ucode_write_waitq);
	init_waitqueue_head(&trans_pcie->fw_reset_waitq);
	init_waitqueue_head(&trans_pcie->imr_waitq);

	trans_pcie->rba.alloc_wq = alloc_workqueue("rb_allocator",
						   WQ_HIGHPRI | WQ_UNBOUND, 0);
	if (!trans_pcie->rba.alloc_wq) {
		ret = -ENOMEM;
		goto out_free_ndev;
	}
	INIT_WORK(&trans_pcie->rba.rx_alloc, iwl_pcie_rx_allocator_work);

	trans_pcie->debug_rfkill = -1;

	if (!mac_cfg->base->pcie_l1_allowed) {
		/*
		 * W/A - seems to solve weird behavior. We need to remove this
		 * if we don't want to stay in L1 all the time. This wastes a
		 * lot of power.
		 */
		pci_disable_link_state(pdev, PCIE_LINK_STATE_L0S |
				       PCIE_LINK_STATE_L1 |
				       PCIE_LINK_STATE_CLKPM);
	}

	addr_size = trans_pcie->txqs.tfd.addr_size;
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(addr_size));
	if (ret) {
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		/* both attempts failed: */
		if (ret) {
			dev_err(&pdev->dev, "No suitable DMA available\n");
			goto out_no_pci;
		}
	}

	/* We disable the RETRY_TIMEOUT register (0x41) to keep
	 * PCI Tx retries from interfering with C3 CPU state */
	pci_write_config_byte(pdev, PCI_CFG_RETRY_TIMEOUT, 0x00);

	trans_pcie->pci_dev = pdev;
	iwl_disable_interrupts(trans);

	/*
	 * In the 8000 HW family the format of the 4 bytes of CSR_HW_REV have
	 * changed, and now the revision step also includes bit 0-1 (no more
	 * "dash" value). To keep hw_rev backwards compatible - we'll store it
	 * in the old format.
	 */
	if (mac_cfg->device_family >= IWL_DEVICE_FAMILY_8000)
		info->hw_rev_step = info->hw_rev & 0xF;
	else
		info->hw_rev_step = (info->hw_rev & 0xC) >> 2;

	IWL_DEBUG_INFO(trans, "HW REV: 0x%0x\n", info->hw_rev);

	iwl_pcie_set_interrupt_capa(pdev, trans, mac_cfg, info);

	init_waitqueue_head(&trans_pcie->sx_waitq);

	ret = iwl_pcie_alloc_invalid_tx_cmd(trans);
	if (ret)
		goto out_no_pci;

	if (trans_pcie->msix_enabled) {
		ret = iwl_pcie_init_msix_handler(pdev, trans_pcie, info);
		if (ret)
			goto out_no_pci;
	 } else {
		ret = iwl_pcie_alloc_ict(trans);
		if (ret)
			goto out_no_pci;

		ret = devm_request_threaded_irq(&pdev->dev, pdev->irq,
						iwl_pcie_isr,
						iwl_pcie_irq_handler,
						IRQF_SHARED, DRV_NAME, trans);
		if (ret) {
			IWL_ERR(trans, "Error allocating IRQ %d\n", pdev->irq);
			goto out_free_ict;
		}
	 }

#ifdef CONFIG_IWLWIFI_DEBUGFS
	trans_pcie->fw_mon_data.state = IWL_FW_MON_DBGFS_STATE_CLOSED;
	mutex_init(&trans_pcie->fw_mon_data.mutex);
#endif

	iwl_dbg_tlv_init(trans);

	return trans;

out_free_ict:
	iwl_pcie_free_ict(trans);
out_no_pci:
	destroy_workqueue(trans_pcie->rba.alloc_wq);
out_free_ndev:
	free_netdev(trans_pcie->napi_dev);
out_free_tso:
	free_percpu(trans_pcie->txqs.tso_hdr_page);
out_free_trans:
	iwl_trans_free(trans);
	return ERR_PTR(ret);
}

void iwl_trans_pcie_copy_imr_fh(struct iwl_trans *trans,
				u32 dst_addr, u64 src_addr, u32 byte_cnt)
{
	iwl_write_prph(trans, IMR_UREG_CHICK,
		       iwl_read_prph(trans, IMR_UREG_CHICK) |
		       IMR_UREG_CHICK_HALT_UMAC_PERMANENTLY_MSK);
	iwl_write_prph(trans, IMR_TFH_SRV_DMA_CHNL0_SRAM_ADDR, dst_addr);
	iwl_write_prph(trans, IMR_TFH_SRV_DMA_CHNL0_DRAM_ADDR_LSB,
		       (u32)(src_addr & 0xFFFFFFFF));
	iwl_write_prph(trans, IMR_TFH_SRV_DMA_CHNL0_DRAM_ADDR_MSB,
		       iwl_get_dma_hi_addr(src_addr));
	iwl_write_prph(trans, IMR_TFH_SRV_DMA_CHNL0_BC, byte_cnt);
	iwl_write_prph(trans, IMR_TFH_SRV_DMA_CHNL0_CTRL,
		       IMR_TFH_SRV_DMA_CHNL0_CTRL_D2S_IRQ_TARGET_POS |
		       IMR_TFH_SRV_DMA_CHNL0_CTRL_D2S_DMA_EN_POS |
		       IMR_TFH_SRV_DMA_CHNL0_CTRL_D2S_RS_MSK);
}

int iwl_trans_pcie_copy_imr(struct iwl_trans *trans,
			    u32 dst_addr, u64 src_addr, u32 byte_cnt)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	int ret = -1;

	trans_pcie->imr_status = IMR_D2S_REQUESTED;
	iwl_trans_pcie_copy_imr_fh(trans, dst_addr, src_addr, byte_cnt);
	ret = wait_event_timeout(trans_pcie->imr_waitq,
				 trans_pcie->imr_status !=
				 IMR_D2S_REQUESTED, 5 * HZ);
	if (!ret || trans_pcie->imr_status == IMR_D2S_ERROR) {
		IWL_ERR(trans, "Failed to copy IMR Memory chunk!\n");
		iwl_trans_pcie_dump_regs(trans, trans_pcie->pci_dev);
		return -ETIMEDOUT;
	}
	trans_pcie->imr_status = IMR_D2S_IDLE;
	return 0;
}

/*
 * Read rf id and cdb info from prph register and store it
 */
static void get_crf_id(struct iwl_trans *iwl_trans,
		       struct iwl_trans_info *info)
{
	u32 sd_reg_ver_addr;
	u32 hw_wfpm_id;
	u32 val = 0;
	u8 step;

	if (iwl_trans->mac_cfg->device_family >= IWL_DEVICE_FAMILY_AX210)
		sd_reg_ver_addr = SD_REG_VER_GEN2;
	else
		sd_reg_ver_addr = SD_REG_VER;

	/* Enable access to peripheral registers */
	val = iwl_read_umac_prph_no_grab(iwl_trans, WFPM_CTRL_REG);
	val |= WFPM_AUX_CTL_AUX_IF_MAC_OWNER_MSK;
	iwl_write_umac_prph_no_grab(iwl_trans, WFPM_CTRL_REG, val);

	/* Read crf info */
	info->hw_crf_id = iwl_read_prph_no_grab(iwl_trans, sd_reg_ver_addr);

	/* Read cnv info */
	info->hw_cnv_id = iwl_read_prph_no_grab(iwl_trans, CNVI_AUX_MISC_CHIP);

	/* For BZ-W, take B step also when A step is indicated */
	if (CSR_HW_REV_TYPE(info->hw_rev) == IWL_CFG_MAC_TYPE_BZ_W)
		step = SILICON_B_STEP;

	/* In BZ, the MAC step must be read from the CNVI aux register */
	if (CSR_HW_REV_TYPE(info->hw_rev) == IWL_CFG_MAC_TYPE_BZ) {
		step = CNVI_AUX_MISC_CHIP_MAC_STEP(info->hw_cnv_id);

		/* For BZ-U, take B step also when A step is indicated */
		if ((CNVI_AUX_MISC_CHIP_PROD_TYPE(info->hw_cnv_id) ==
		    CNVI_AUX_MISC_CHIP_PROD_TYPE_BZ_U) &&
		    step == SILICON_A_STEP)
			step = SILICON_B_STEP;
	}

	if (CSR_HW_REV_TYPE(info->hw_rev) == IWL_CFG_MAC_TYPE_BZ ||
	    CSR_HW_REV_TYPE(info->hw_rev) == IWL_CFG_MAC_TYPE_BZ_W) {
		info->hw_rev_step = step;
		info->hw_rev |= step;
	}

	/* Read cdb info (also contains the jacket info if needed in the future */
	hw_wfpm_id = iwl_read_umac_prph_no_grab(iwl_trans, WFPM_OTP_CFG1_ADDR);
	IWL_INFO(iwl_trans, "Detected crf-id 0x%x, cnv-id 0x%x wfpm id 0x%x\n",
		 info->hw_crf_id, info->hw_cnv_id, hw_wfpm_id);
}

/*
 * In case that there is no OTP on the NIC, map the rf id and cdb info
 * from the prph registers.
 */
static int map_crf_id(struct iwl_trans *iwl_trans,
		      struct iwl_trans_info *info)
{
	int ret = 0;
	u32 val = info->hw_crf_id;
	u32 step_id = REG_CRF_ID_STEP(val);
	u32 slave_id = REG_CRF_ID_SLAVE(val);
	u32 jacket_id_cnv = REG_CRF_ID_SLAVE(info->hw_cnv_id);
	u32 hw_wfpm_id = iwl_read_umac_prph_no_grab(iwl_trans,
						    WFPM_OTP_CFG1_ADDR);
	u32 jacket_id_wfpm = WFPM_OTP_CFG1_IS_JACKET(hw_wfpm_id);
	u32 cdb_id_wfpm = WFPM_OTP_CFG1_IS_CDB(hw_wfpm_id);

	/* Map between crf id to rf id */
	switch (REG_CRF_ID_TYPE(val)) {
	case REG_CRF_ID_TYPE_JF_1:
		info->hw_rf_id = (IWL_CFG_RF_TYPE_JF1 << 12);
		break;
	case REG_CRF_ID_TYPE_JF_2:
		info->hw_rf_id = (IWL_CFG_RF_TYPE_JF2 << 12);
		break;
	case REG_CRF_ID_TYPE_HR_NONE_CDB_1X1:
		info->hw_rf_id = (IWL_CFG_RF_TYPE_HR1 << 12);
		break;
	case REG_CRF_ID_TYPE_HR_NONE_CDB:
		info->hw_rf_id = (IWL_CFG_RF_TYPE_HR2 << 12);
		break;
	case REG_CRF_ID_TYPE_HR_CDB:
		info->hw_rf_id = (IWL_CFG_RF_TYPE_HR2 << 12);
		break;
	case REG_CRF_ID_TYPE_GF:
		info->hw_rf_id = (IWL_CFG_RF_TYPE_GF << 12);
		break;
	case REG_CRF_ID_TYPE_FM:
		info->hw_rf_id = (IWL_CFG_RF_TYPE_FM << 12);
		break;
	case REG_CRF_ID_TYPE_WHP:
		info->hw_rf_id = (IWL_CFG_RF_TYPE_WH << 12);
		break;
	case REG_CRF_ID_TYPE_PE:
		info->hw_rf_id = (IWL_CFG_RF_TYPE_PE << 12);
		break;
	default:
		ret = -EIO;
		IWL_ERR(iwl_trans,
			"Can't find a correct rfid for crf id 0x%x\n",
			REG_CRF_ID_TYPE(val));
		goto out;
	}

	/* Set Step-id */
	info->hw_rf_id |= (step_id << 8);

	/* Set CDB capabilities */
	if (cdb_id_wfpm || slave_id) {
		info->hw_rf_id += BIT(28);
		IWL_INFO(iwl_trans, "Adding cdb to rf id\n");
	}

	/* Set Jacket capabilities */
	if (jacket_id_wfpm || jacket_id_cnv) {
		info->hw_rf_id += BIT(29);
		IWL_INFO(iwl_trans, "Adding jacket to rf id\n");
	}

	IWL_INFO(iwl_trans,
		 "Detected rf-type 0x%x step-id 0x%x slave-id 0x%x from crf id 0x%x\n",
		 REG_CRF_ID_TYPE(val), step_id, slave_id, info->hw_rf_id);
	IWL_INFO(iwl_trans,
		 "Detected cdb-id 0x%x jacket-id 0x%x from wfpm id 0x%x\n",
		 cdb_id_wfpm, jacket_id_wfpm, hw_wfpm_id);
	IWL_INFO(iwl_trans, "Detected jacket-id 0x%x from cnvi id 0x%x\n",
		 jacket_id_cnv, info->hw_cnv_id);

out:
	return ret;
}

static void iwl_pcie_recheck_me_status(struct work_struct *wk)
{
	struct iwl_trans_pcie *trans_pcie = container_of(wk,
							 typeof(*trans_pcie),
							 me_recheck_wk.work);
	u32 val;

	val = iwl_read32(trans_pcie->trans, CSR_HW_IF_CONFIG_REG);
	trans_pcie->me_present = !!(val & CSR_HW_IF_CONFIG_REG_IAMT_UP);
}

static void iwl_pcie_check_me_status(struct iwl_trans *trans)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	u32 val;

	trans_pcie->me_present = -1;

	INIT_DELAYED_WORK(&trans_pcie->me_recheck_wk,
			  iwl_pcie_recheck_me_status);

	/* we don't have a good way of determining this until BZ */
	if (trans->mac_cfg->device_family < IWL_DEVICE_FAMILY_BZ)
		return;

	val = iwl_read_prph(trans, CNVI_SCU_REG_FOR_ECO_1);
	if (val & CNVI_SCU_REG_FOR_ECO_1_WIAMT_KNOWN) {
		trans_pcie->me_present =
			!!(val & CNVI_SCU_REG_FOR_ECO_1_WIAMT_PRESENT);
		return;
	}

	val = iwl_read32(trans, CSR_HW_IF_CONFIG_REG);
	if (val & (CSR_HW_IF_CONFIG_REG_ME_OWN |
		   CSR_HW_IF_CONFIG_REG_IAMT_UP)) {
		trans_pcie->me_present = 1;
		return;
	}

	/* recheck again later, ME might still be initializing */
	schedule_delayed_work(&trans_pcie->me_recheck_wk, HZ);
}

int iwl_pci_gen1_2_probe(struct pci_dev *pdev,
			 const struct pci_device_id *ent,
			 const struct iwl_mac_cfg *mac_cfg,
			 u8 __iomem *hw_base, u32 hw_rev)
{
	const struct iwl_dev_info *dev_info;
	struct iwl_trans_info info = {
		.hw_id = (pdev->device << 16) + pdev->subsystem_device,
		.hw_rev = hw_rev,
	};
	struct iwl_trans *iwl_trans;
	struct iwl_trans_pcie *trans_pcie;
	int ret;

	iwl_trans = iwl_trans_pcie_alloc(pdev, mac_cfg, &info, hw_base);
	if (IS_ERR(iwl_trans))
		return PTR_ERR(iwl_trans);

	trans_pcie = IWL_TRANS_GET_PCIE_TRANS(iwl_trans);

	iwl_trans_pcie_check_product_reset_status(pdev);
	iwl_trans_pcie_check_product_reset_mode(pdev);

	/* set the things we know so far for the grab NIC access */
	iwl_trans_set_info(iwl_trans, &info);

	/*
	 * Let's try to grab NIC access early here. Sometimes, NICs may
	 * fail to initialize, and if that happens it's better if we see
	 * issues early on (and can reprobe, per the logic inside), than
	 * first trying to load the firmware etc. and potentially only
	 * detecting any problems when the first interface is brought up.
	 */
	ret = iwl_pcie_prepare_card_hw(iwl_trans);
	if (!ret) {
		ret = iwl_finish_nic_init(iwl_trans);
		if (ret)
			goto out_free_trans;
		if (iwl_trans_grab_nic_access(iwl_trans)) {
			get_crf_id(iwl_trans, &info);
			/* all good */
			iwl_trans_release_nic_access(iwl_trans);
		} else {
			ret = -EIO;
			goto out_free_trans;
		}
	}

	info.hw_rf_id = iwl_read32(iwl_trans, CSR_HW_RF_ID);

	/*
	 * The RF_ID is set to zero in blank OTP so read version to
	 * extract the RF_ID.
	 * This is relevant only for family 9000 and up.
	 */
	if (iwl_trans->mac_cfg->device_family >= IWL_DEVICE_FAMILY_9000 &&
	    !CSR_HW_RFID_TYPE(info.hw_rf_id) && map_crf_id(iwl_trans, &info)) {
		ret = -EINVAL;
		goto out_free_trans;
	}

	IWL_INFO(iwl_trans, "PCI dev %04x/%04x, rev=0x%x, rfid=0x%x\n",
		 pdev->device, pdev->subsystem_device,
		 info.hw_rev, info.hw_rf_id);

	dev_info = iwl_pci_find_dev_info(pdev->device, pdev->subsystem_device,
					 CSR_HW_RFID_TYPE(info.hw_rf_id),
					 CSR_HW_RFID_IS_CDB(info.hw_rf_id),
					 IWL_SUBDEVICE_RF_ID(pdev->subsystem_device),
					 IWL_SUBDEVICE_BW_LIM(pdev->subsystem_device),
					 !iwl_trans->mac_cfg->integrated);
	if (dev_info) {
		iwl_trans->cfg = dev_info->cfg;
		info.name = dev_info->name;
	}

#if IS_ENABLED(CONFIG_IWLMVM)

	/*
	 * special-case 7265D, it has the same PCI IDs.
	 *
	 * Note that because we already pass the cfg to the transport above,
	 * all the parameters that the transport uses must, until that is
	 * changed, be identical to the ones in the 7265D configuration.
	 */
	if (iwl_trans->cfg == &iwl7265_cfg &&
	    (info.hw_rev & CSR_HW_REV_TYPE_MSK) == CSR_HW_REV_TYPE_7265D)
		iwl_trans->cfg = &iwl7265d_cfg;
#endif
	if (!iwl_trans->cfg) {
		pr_err("No config found for PCI dev %04x/%04x, rev=0x%x, rfid=0x%x\n",
		       pdev->device, pdev->subsystem_device,
		       info.hw_rev, info.hw_rf_id);
		ret = -EINVAL;
		goto out_free_trans;
	}

	IWL_INFO(iwl_trans, "Detected %s\n", info.name);

	if (iwl_trans->mac_cfg->mq_rx_supported) {
		if (WARN_ON(!iwl_trans->cfg->num_rbds)) {
			ret = -EINVAL;
			goto out_free_trans;
		}
		trans_pcie->num_rx_bufs = iwl_trans_get_num_rbds(iwl_trans);
	} else {
		trans_pcie->num_rx_bufs = RX_QUEUE_SIZE;
	}

	if (!iwl_trans->mac_cfg->integrated) {
		u16 link_status;

		pcie_capability_read_word(pdev, PCI_EXP_LNKSTA, &link_status);

		info.pcie_link_speed =
			u16_get_bits(link_status, PCI_EXP_LNKSTA_CLS);
	}

	iwl_trans_set_info(iwl_trans, &info);

	pci_set_drvdata(pdev, iwl_trans);

	iwl_pcie_check_me_status(iwl_trans);

	/* try to get ownership so that we'll know if we don't own it */
	iwl_pcie_prepare_card_hw(iwl_trans);

	iwl_trans->drv = iwl_drv_start(iwl_trans);

	if (IS_ERR(iwl_trans->drv)) {
		ret = PTR_ERR(iwl_trans->drv);
		goto out_free_trans;
	}

	/* register transport layer debugfs here */
	iwl_trans_pcie_dbgfs_register(iwl_trans);

	return 0;

out_free_trans:
	iwl_trans_pcie_free(iwl_trans);
	return ret;
}
