// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2018-2023, Intel Corporation. */

#include "ice_common.h"
#include "ice_sched.h"
#include "ice_adminq_cmd.h"
#include "ice_flow.h"
#include "ice_ptp_hw.h"
#include <linux/packing.h>

#define ICE_PF_RESET_WAIT_COUNT	300
#define ICE_MAX_NETLIST_SIZE	10

static const char * const ice_link_mode_str_low[] = {
	[0] = "100BASE_TX",
	[1] = "100M_SGMII",
	[2] = "1000BASE_T",
	[3] = "1000BASE_SX",
	[4] = "1000BASE_LX",
	[5] = "1000BASE_KX",
	[6] = "1G_SGMII",
	[7] = "2500BASE_T",
	[8] = "2500BASE_X",
	[9] = "2500BASE_KX",
	[10] = "5GBASE_T",
	[11] = "5GBASE_KR",
	[12] = "10GBASE_T",
	[13] = "10G_SFI_DA",
	[14] = "10GBASE_SR",
	[15] = "10GBASE_LR",
	[16] = "10GBASE_KR_CR1",
	[17] = "10G_SFI_AOC_ACC",
	[18] = "10G_SFI_C2C",
	[19] = "25GBASE_T",
	[20] = "25GBASE_CR",
	[21] = "25GBASE_CR_S",
	[22] = "25GBASE_CR1",
	[23] = "25GBASE_SR",
	[24] = "25GBASE_LR",
	[25] = "25GBASE_KR",
	[26] = "25GBASE_KR_S",
	[27] = "25GBASE_KR1",
	[28] = "25G_AUI_AOC_ACC",
	[29] = "25G_AUI_C2C",
	[30] = "40GBASE_CR4",
	[31] = "40GBASE_SR4",
	[32] = "40GBASE_LR4",
	[33] = "40GBASE_KR4",
	[34] = "40G_XLAUI_AOC_ACC",
	[35] = "40G_XLAUI",
	[36] = "50GBASE_CR2",
	[37] = "50GBASE_SR2",
	[38] = "50GBASE_LR2",
	[39] = "50GBASE_KR2",
	[40] = "50G_LAUI2_AOC_ACC",
	[41] = "50G_LAUI2",
	[42] = "50G_AUI2_AOC_ACC",
	[43] = "50G_AUI2",
	[44] = "50GBASE_CP",
	[45] = "50GBASE_SR",
	[46] = "50GBASE_FR",
	[47] = "50GBASE_LR",
	[48] = "50GBASE_KR_PAM4",
	[49] = "50G_AUI1_AOC_ACC",
	[50] = "50G_AUI1",
	[51] = "100GBASE_CR4",
	[52] = "100GBASE_SR4",
	[53] = "100GBASE_LR4",
	[54] = "100GBASE_KR4",
	[55] = "100G_CAUI4_AOC_ACC",
	[56] = "100G_CAUI4",
	[57] = "100G_AUI4_AOC_ACC",
	[58] = "100G_AUI4",
	[59] = "100GBASE_CR_PAM4",
	[60] = "100GBASE_KR_PAM4",
	[61] = "100GBASE_CP2",
	[62] = "100GBASE_SR2",
	[63] = "100GBASE_DR",
};

static const char * const ice_link_mode_str_high[] = {
	[0] = "100GBASE_KR2_PAM4",
	[1] = "100G_CAUI2_AOC_ACC",
	[2] = "100G_CAUI2",
	[3] = "100G_AUI2_AOC_ACC",
	[4] = "100G_AUI2",
};

/**
 * ice_dump_phy_type - helper function to dump phy_type
 * @hw: pointer to the HW structure
 * @low: 64 bit value for phy_type_low
 * @high: 64 bit value for phy_type_high
 * @prefix: prefix string to differentiate multiple dumps
 */
static void
ice_dump_phy_type(struct ice_hw *hw, u64 low, u64 high, const char *prefix)
{
	ice_debug(hw, ICE_DBG_PHY, "%s: phy_type_low: 0x%016llx\n", prefix, low);

	for (u32 i = 0; i < BITS_PER_TYPE(typeof(low)); i++) {
		if (low & BIT_ULL(i))
			ice_debug(hw, ICE_DBG_PHY, "%s:   bit(%d): %s\n",
				  prefix, i, ice_link_mode_str_low[i]);
	}

	ice_debug(hw, ICE_DBG_PHY, "%s: phy_type_high: 0x%016llx\n", prefix, high);

	for (u32 i = 0; i < BITS_PER_TYPE(typeof(high)); i++) {
		if (high & BIT_ULL(i))
			ice_debug(hw, ICE_DBG_PHY, "%s:   bit(%d): %s\n",
				  prefix, i, ice_link_mode_str_high[i]);
	}
}

/**
 * ice_set_mac_type - Sets MAC type
 * @hw: pointer to the HW structure
 *
 * This function sets the MAC type of the adapter based on the
 * vendor ID and device ID stored in the HW structure.
 */
static int ice_set_mac_type(struct ice_hw *hw)
{
	if (hw->vendor_id != PCI_VENDOR_ID_INTEL)
		return -ENODEV;

	switch (hw->device_id) {
	case ICE_DEV_ID_E810C_BACKPLANE:
	case ICE_DEV_ID_E810C_QSFP:
	case ICE_DEV_ID_E810C_SFP:
	case ICE_DEV_ID_E810_XXV_BACKPLANE:
	case ICE_DEV_ID_E810_XXV_QSFP:
	case ICE_DEV_ID_E810_XXV_SFP:
		hw->mac_type = ICE_MAC_E810;
		break;
	case ICE_DEV_ID_E823C_10G_BASE_T:
	case ICE_DEV_ID_E823C_BACKPLANE:
	case ICE_DEV_ID_E823C_QSFP:
	case ICE_DEV_ID_E823C_SFP:
	case ICE_DEV_ID_E823C_SGMII:
	case ICE_DEV_ID_E822C_10G_BASE_T:
	case ICE_DEV_ID_E822C_BACKPLANE:
	case ICE_DEV_ID_E822C_QSFP:
	case ICE_DEV_ID_E822C_SFP:
	case ICE_DEV_ID_E822C_SGMII:
	case ICE_DEV_ID_E822L_10G_BASE_T:
	case ICE_DEV_ID_E822L_BACKPLANE:
	case ICE_DEV_ID_E822L_SFP:
	case ICE_DEV_ID_E822L_SGMII:
	case ICE_DEV_ID_E823L_10G_BASE_T:
	case ICE_DEV_ID_E823L_1GBE:
	case ICE_DEV_ID_E823L_BACKPLANE:
	case ICE_DEV_ID_E823L_QSFP:
	case ICE_DEV_ID_E823L_SFP:
		hw->mac_type = ICE_MAC_GENERIC;
		break;
	case ICE_DEV_ID_E825C_BACKPLANE:
	case ICE_DEV_ID_E825C_QSFP:
	case ICE_DEV_ID_E825C_SFP:
	case ICE_DEV_ID_E825C_SGMII:
		hw->mac_type = ICE_MAC_GENERIC_3K_E825;
		break;
	case ICE_DEV_ID_E830CC_BACKPLANE:
	case ICE_DEV_ID_E830CC_QSFP56:
	case ICE_DEV_ID_E830CC_SFP:
	case ICE_DEV_ID_E830CC_SFP_DD:
	case ICE_DEV_ID_E830C_BACKPLANE:
	case ICE_DEV_ID_E830_XXV_BACKPLANE:
	case ICE_DEV_ID_E830C_QSFP:
	case ICE_DEV_ID_E830_XXV_QSFP:
	case ICE_DEV_ID_E830C_SFP:
	case ICE_DEV_ID_E830_XXV_SFP:
	case ICE_DEV_ID_E835CC_BACKPLANE:
	case ICE_DEV_ID_E835CC_QSFP56:
	case ICE_DEV_ID_E835CC_SFP:
	case ICE_DEV_ID_E835C_BACKPLANE:
	case ICE_DEV_ID_E835C_QSFP:
	case ICE_DEV_ID_E835C_SFP:
	case ICE_DEV_ID_E835_L_BACKPLANE:
	case ICE_DEV_ID_E835_L_QSFP:
	case ICE_DEV_ID_E835_L_SFP:
		hw->mac_type = ICE_MAC_E830;
		break;
	default:
		hw->mac_type = ICE_MAC_UNKNOWN;
		break;
	}

	ice_debug(hw, ICE_DBG_INIT, "mac_type: %d\n", hw->mac_type);
	return 0;
}

/**
 * ice_is_generic_mac - check if device's mac_type is generic
 * @hw: pointer to the hardware structure
 *
 * Return: true if mac_type is ICE_MAC_GENERIC*, false otherwise.
 */
bool ice_is_generic_mac(struct ice_hw *hw)
{
	return (hw->mac_type == ICE_MAC_GENERIC ||
		hw->mac_type == ICE_MAC_GENERIC_3K_E825);
}

/**
 * ice_is_pf_c827 - check if pf contains c827 phy
 * @hw: pointer to the hw struct
 *
 * Return: true if the device has c827 phy.
 */
static bool ice_is_pf_c827(struct ice_hw *hw)
{
	struct ice_aqc_get_link_topo cmd = {};
	u8 node_part_number;
	u16 node_handle;
	int status;

	if (hw->mac_type != ICE_MAC_E810)
		return false;

	if (hw->device_id != ICE_DEV_ID_E810C_QSFP)
		return true;

	cmd.addr.topo_params.node_type_ctx =
		FIELD_PREP(ICE_AQC_LINK_TOPO_NODE_TYPE_M, ICE_AQC_LINK_TOPO_NODE_TYPE_PHY) |
		FIELD_PREP(ICE_AQC_LINK_TOPO_NODE_CTX_M, ICE_AQC_LINK_TOPO_NODE_CTX_PORT);
	cmd.addr.topo_params.index = 0;

	status = ice_aq_get_netlist_node(hw, &cmd, &node_part_number,
					 &node_handle);

	if (status || node_part_number != ICE_AQC_GET_LINK_TOPO_NODE_NR_C827)
		return false;

	if (node_handle == E810C_QSFP_C827_0_HANDLE || node_handle == E810C_QSFP_C827_1_HANDLE)
		return true;

	return false;
}

/**
 * ice_clear_pf_cfg - Clear PF configuration
 * @hw: pointer to the hardware structure
 *
 * Clears any existing PF configuration (VSIs, VSI lists, switch rules, port
 * configuration, flow director filters, etc.).
 */
int ice_clear_pf_cfg(struct ice_hw *hw)
{
	struct libie_aq_desc desc;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_clear_pf_cfg);

	return ice_aq_send_cmd(hw, &desc, NULL, 0, NULL);
}

/**
 * ice_aq_manage_mac_read - manage MAC address read command
 * @hw: pointer to the HW struct
 * @buf: a virtual buffer to hold the manage MAC read response
 * @buf_size: Size of the virtual buffer
 * @cd: pointer to command details structure or NULL
 *
 * This function is used to return per PF station MAC address (0x0107).
 * NOTE: Upon successful completion of this command, MAC address information
 * is returned in user specified buffer. Please interpret user specified
 * buffer as "manage_mac_read" response.
 * Response such as various MAC addresses are stored in HW struct (port.mac)
 * ice_discover_dev_caps is expected to be called before this function is
 * called.
 */
static int
ice_aq_manage_mac_read(struct ice_hw *hw, void *buf, u16 buf_size,
		       struct ice_sq_cd *cd)
{
	struct ice_aqc_manage_mac_read_resp *resp;
	struct ice_aqc_manage_mac_read *cmd;
	struct libie_aq_desc desc;
	int status;
	u16 flags;
	u8 i;

	cmd = libie_aq_raw(&desc);

	if (buf_size < sizeof(*resp))
		return -EINVAL;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_manage_mac_read);

	status = ice_aq_send_cmd(hw, &desc, buf, buf_size, cd);
	if (status)
		return status;

	resp = buf;
	flags = le16_to_cpu(cmd->flags) & ICE_AQC_MAN_MAC_READ_M;

	if (!(flags & ICE_AQC_MAN_MAC_LAN_ADDR_VALID)) {
		ice_debug(hw, ICE_DBG_LAN, "got invalid MAC address\n");
		return -EIO;
	}

	/* A single port can report up to two (LAN and WoL) addresses */
	for (i = 0; i < cmd->num_addr; i++)
		if (resp[i].addr_type == ICE_AQC_MAN_MAC_ADDR_TYPE_LAN) {
			ether_addr_copy(hw->port_info->mac.lan_addr,
					resp[i].mac_addr);
			ether_addr_copy(hw->port_info->mac.perm_addr,
					resp[i].mac_addr);
			break;
		}

	return 0;
}

/**
 * ice_aq_get_phy_caps - returns PHY capabilities
 * @pi: port information structure
 * @qual_mods: report qualified modules
 * @report_mode: report mode capabilities
 * @pcaps: structure for PHY capabilities to be filled
 * @cd: pointer to command details structure or NULL
 *
 * Returns the various PHY capabilities supported on the Port (0x0600)
 */
int
ice_aq_get_phy_caps(struct ice_port_info *pi, bool qual_mods, u8 report_mode,
		    struct ice_aqc_get_phy_caps_data *pcaps,
		    struct ice_sq_cd *cd)
{
	struct ice_aqc_get_phy_caps *cmd;
	u16 pcaps_size = sizeof(*pcaps);
	struct libie_aq_desc desc;
	const char *prefix;
	struct ice_hw *hw;
	int status;

	cmd = libie_aq_raw(&desc);

	if (!pcaps || (report_mode & ~ICE_AQC_REPORT_MODE_M) || !pi)
		return -EINVAL;
	hw = pi->hw;

	if (report_mode == ICE_AQC_REPORT_DFLT_CFG &&
	    !ice_fw_supports_report_dflt_cfg(hw))
		return -EINVAL;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_get_phy_caps);

	if (qual_mods)
		cmd->param0 |= cpu_to_le16(ICE_AQC_GET_PHY_RQM);

	cmd->param0 |= cpu_to_le16(report_mode);
	status = ice_aq_send_cmd(hw, &desc, pcaps, pcaps_size, cd);

	ice_debug(hw, ICE_DBG_LINK, "get phy caps dump\n");

	switch (report_mode) {
	case ICE_AQC_REPORT_TOPO_CAP_MEDIA:
		prefix = "phy_caps_media";
		break;
	case ICE_AQC_REPORT_TOPO_CAP_NO_MEDIA:
		prefix = "phy_caps_no_media";
		break;
	case ICE_AQC_REPORT_ACTIVE_CFG:
		prefix = "phy_caps_active";
		break;
	case ICE_AQC_REPORT_DFLT_CFG:
		prefix = "phy_caps_default";
		break;
	default:
		prefix = "phy_caps_invalid";
	}

	ice_dump_phy_type(hw, le64_to_cpu(pcaps->phy_type_low),
			  le64_to_cpu(pcaps->phy_type_high), prefix);

	ice_debug(hw, ICE_DBG_LINK, "%s: report_mode = 0x%x\n",
		  prefix, report_mode);
	ice_debug(hw, ICE_DBG_LINK, "%s: caps = 0x%x\n", prefix, pcaps->caps);
	ice_debug(hw, ICE_DBG_LINK, "%s: low_power_ctrl_an = 0x%x\n", prefix,
		  pcaps->low_power_ctrl_an);
	ice_debug(hw, ICE_DBG_LINK, "%s: eee_cap = 0x%x\n", prefix,
		  pcaps->eee_cap);
	ice_debug(hw, ICE_DBG_LINK, "%s: eeer_value = 0x%x\n", prefix,
		  pcaps->eeer_value);
	ice_debug(hw, ICE_DBG_LINK, "%s: link_fec_options = 0x%x\n", prefix,
		  pcaps->link_fec_options);
	ice_debug(hw, ICE_DBG_LINK, "%s: module_compliance_enforcement = 0x%x\n",
		  prefix, pcaps->module_compliance_enforcement);
	ice_debug(hw, ICE_DBG_LINK, "%s: extended_compliance_code = 0x%x\n",
		  prefix, pcaps->extended_compliance_code);
	ice_debug(hw, ICE_DBG_LINK, "%s: module_type[0] = 0x%x\n", prefix,
		  pcaps->module_type[0]);
	ice_debug(hw, ICE_DBG_LINK, "%s: module_type[1] = 0x%x\n", prefix,
		  pcaps->module_type[1]);
	ice_debug(hw, ICE_DBG_LINK, "%s: module_type[2] = 0x%x\n", prefix,
		  pcaps->module_type[2]);

	if (!status && report_mode == ICE_AQC_REPORT_TOPO_CAP_MEDIA) {
		pi->phy.phy_type_low = le64_to_cpu(pcaps->phy_type_low);
		pi->phy.phy_type_high = le64_to_cpu(pcaps->phy_type_high);
		memcpy(pi->phy.link_info.module_type, &pcaps->module_type,
		       sizeof(pi->phy.link_info.module_type));
	}

	return status;
}

/**
 * ice_aq_get_link_topo_handle - get link topology node return status
 * @pi: port information structure
 * @node_type: requested node type
 * @cd: pointer to command details structure or NULL
 *
 * Get link topology node return status for specified node type (0x06E0)
 *
 * Node type cage can be used to determine if cage is present. If AQC
 * returns error (ENOENT), then no cage present. If no cage present, then
 * connection type is backplane or BASE-T.
 */
static int
ice_aq_get_link_topo_handle(struct ice_port_info *pi, u8 node_type,
			    struct ice_sq_cd *cd)
{
	struct ice_aqc_get_link_topo *cmd;
	struct libie_aq_desc desc;

	cmd = libie_aq_raw(&desc);

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_get_link_topo);

	cmd->addr.topo_params.node_type_ctx =
		(ICE_AQC_LINK_TOPO_NODE_CTX_PORT <<
		 ICE_AQC_LINK_TOPO_NODE_CTX_S);

	/* set node type */
	cmd->addr.topo_params.node_type_ctx |=
		(ICE_AQC_LINK_TOPO_NODE_TYPE_M & node_type);

	return ice_aq_send_cmd(pi->hw, &desc, NULL, 0, cd);
}

/**
 * ice_aq_get_netlist_node
 * @hw: pointer to the hw struct
 * @cmd: get_link_topo AQ structure
 * @node_part_number: output node part number if node found
 * @node_handle: output node handle parameter if node found
 *
 * Get netlist node handle.
 */
int
ice_aq_get_netlist_node(struct ice_hw *hw, struct ice_aqc_get_link_topo *cmd,
			u8 *node_part_number, u16 *node_handle)
{
	struct ice_aqc_get_link_topo *resp;
	struct libie_aq_desc desc;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_get_link_topo);
	resp = libie_aq_raw(&desc);
	*resp = *cmd;

	if (ice_aq_send_cmd(hw, &desc, NULL, 0, NULL))
		return -EINTR;

	if (node_handle)
		*node_handle = le16_to_cpu(resp->addr.handle);
	if (node_part_number)
		*node_part_number = resp->node_part_num;

	return 0;
}

/**
 * ice_find_netlist_node
 * @hw: pointer to the hw struct
 * @node_type: type of netlist node to look for
 * @ctx: context of the search
 * @node_part_number: node part number to look for
 * @node_handle: output parameter if node found - optional
 *
 * Scan the netlist for a node handle of the given node type and part number.
 *
 * If node_handle is non-NULL it will be modified on function exit. It is only
 * valid if the function returns zero, and should be ignored on any non-zero
 * return value.
 *
 * Return:
 * * 0 if the node is found,
 * * -ENOENT if no handle was found,
 * * negative error code on failure to access the AQ.
 */
static int ice_find_netlist_node(struct ice_hw *hw, u8 node_type, u8 ctx,
				 u8 node_part_number, u16 *node_handle)
{
	u8 idx;

	for (idx = 0; idx < ICE_MAX_NETLIST_SIZE; idx++) {
		struct ice_aqc_get_link_topo cmd = {};
		u8 rec_node_part_number;
		int status;

		cmd.addr.topo_params.node_type_ctx =
			FIELD_PREP(ICE_AQC_LINK_TOPO_NODE_TYPE_M, node_type) |
			FIELD_PREP(ICE_AQC_LINK_TOPO_NODE_CTX_M, ctx);
		cmd.addr.topo_params.index = idx;

		status = ice_aq_get_netlist_node(hw, &cmd,
						 &rec_node_part_number,
						 node_handle);
		if (status)
			return status;

		if (rec_node_part_number == node_part_number)
			return 0;
	}

	return -ENOENT;
}

/**
 * ice_is_media_cage_present
 * @pi: port information structure
 *
 * Returns true if media cage is present, else false. If no cage, then
 * media type is backplane or BASE-T.
 */
static bool ice_is_media_cage_present(struct ice_port_info *pi)
{
	/* Node type cage can be used to determine if cage is present. If AQC
	 * returns error (ENOENT), then no cage present. If no cage present then
	 * connection type is backplane or BASE-T.
	 */
	return !ice_aq_get_link_topo_handle(pi,
					    ICE_AQC_LINK_TOPO_NODE_TYPE_CAGE,
					    NULL);
}

/**
 * ice_get_media_type - Gets media type
 * @pi: port information structure
 */
static enum ice_media_type ice_get_media_type(struct ice_port_info *pi)
{
	struct ice_link_status *hw_link_info;

	if (!pi)
		return ICE_MEDIA_UNKNOWN;

	hw_link_info = &pi->phy.link_info;
	if (hw_link_info->phy_type_low && hw_link_info->phy_type_high)
		/* If more than one media type is selected, report unknown */
		return ICE_MEDIA_UNKNOWN;

	if (hw_link_info->phy_type_low) {
		/* 1G SGMII is a special case where some DA cable PHYs
		 * may show this as an option when it really shouldn't
		 * be since SGMII is meant to be between a MAC and a PHY
		 * in a backplane. Try to detect this case and handle it
		 */
		if (hw_link_info->phy_type_low == ICE_PHY_TYPE_LOW_1G_SGMII &&
		    (hw_link_info->module_type[ICE_AQC_MOD_TYPE_IDENT] ==
		    ICE_AQC_MOD_TYPE_BYTE1_SFP_PLUS_CU_ACTIVE ||
		    hw_link_info->module_type[ICE_AQC_MOD_TYPE_IDENT] ==
		    ICE_AQC_MOD_TYPE_BYTE1_SFP_PLUS_CU_PASSIVE))
			return ICE_MEDIA_DA;

		switch (hw_link_info->phy_type_low) {
		case ICE_PHY_TYPE_LOW_1000BASE_SX:
		case ICE_PHY_TYPE_LOW_1000BASE_LX:
		case ICE_PHY_TYPE_LOW_10GBASE_SR:
		case ICE_PHY_TYPE_LOW_10GBASE_LR:
		case ICE_PHY_TYPE_LOW_10G_SFI_C2C:
		case ICE_PHY_TYPE_LOW_25GBASE_SR:
		case ICE_PHY_TYPE_LOW_25GBASE_LR:
		case ICE_PHY_TYPE_LOW_40GBASE_SR4:
		case ICE_PHY_TYPE_LOW_40GBASE_LR4:
		case ICE_PHY_TYPE_LOW_50GBASE_SR2:
		case ICE_PHY_TYPE_LOW_50GBASE_LR2:
		case ICE_PHY_TYPE_LOW_50GBASE_SR:
		case ICE_PHY_TYPE_LOW_50GBASE_FR:
		case ICE_PHY_TYPE_LOW_50GBASE_LR:
		case ICE_PHY_TYPE_LOW_100GBASE_SR4:
		case ICE_PHY_TYPE_LOW_100GBASE_LR4:
		case ICE_PHY_TYPE_LOW_100GBASE_SR2:
		case ICE_PHY_TYPE_LOW_100GBASE_DR:
		case ICE_PHY_TYPE_LOW_10G_SFI_AOC_ACC:
		case ICE_PHY_TYPE_LOW_25G_AUI_AOC_ACC:
		case ICE_PHY_TYPE_LOW_40G_XLAUI_AOC_ACC:
		case ICE_PHY_TYPE_LOW_50G_LAUI2_AOC_ACC:
		case ICE_PHY_TYPE_LOW_50G_AUI2_AOC_ACC:
		case ICE_PHY_TYPE_LOW_50G_AUI1_AOC_ACC:
		case ICE_PHY_TYPE_LOW_100G_CAUI4_AOC_ACC:
		case ICE_PHY_TYPE_LOW_100G_AUI4_AOC_ACC:
			return ICE_MEDIA_FIBER;
		case ICE_PHY_TYPE_LOW_100BASE_TX:
		case ICE_PHY_TYPE_LOW_1000BASE_T:
		case ICE_PHY_TYPE_LOW_2500BASE_T:
		case ICE_PHY_TYPE_LOW_5GBASE_T:
		case ICE_PHY_TYPE_LOW_10GBASE_T:
		case ICE_PHY_TYPE_LOW_25GBASE_T:
			return ICE_MEDIA_BASET;
		case ICE_PHY_TYPE_LOW_10G_SFI_DA:
		case ICE_PHY_TYPE_LOW_25GBASE_CR:
		case ICE_PHY_TYPE_LOW_25GBASE_CR_S:
		case ICE_PHY_TYPE_LOW_25GBASE_CR1:
		case ICE_PHY_TYPE_LOW_40GBASE_CR4:
		case ICE_PHY_TYPE_LOW_50GBASE_CR2:
		case ICE_PHY_TYPE_LOW_50GBASE_CP:
		case ICE_PHY_TYPE_LOW_100GBASE_CR4:
		case ICE_PHY_TYPE_LOW_100GBASE_CR_PAM4:
		case ICE_PHY_TYPE_LOW_100GBASE_CP2:
			return ICE_MEDIA_DA;
		case ICE_PHY_TYPE_LOW_25G_AUI_C2C:
		case ICE_PHY_TYPE_LOW_40G_XLAUI:
		case ICE_PHY_TYPE_LOW_50G_LAUI2:
		case ICE_PHY_TYPE_LOW_50G_AUI2:
		case ICE_PHY_TYPE_LOW_50G_AUI1:
		case ICE_PHY_TYPE_LOW_100G_AUI4:
		case ICE_PHY_TYPE_LOW_100G_CAUI4:
			if (ice_is_media_cage_present(pi))
				return ICE_MEDIA_DA;
			fallthrough;
		case ICE_PHY_TYPE_LOW_1000BASE_KX:
		case ICE_PHY_TYPE_LOW_2500BASE_KX:
		case ICE_PHY_TYPE_LOW_2500BASE_X:
		case ICE_PHY_TYPE_LOW_5GBASE_KR:
		case ICE_PHY_TYPE_LOW_10GBASE_KR_CR1:
		case ICE_PHY_TYPE_LOW_25GBASE_KR:
		case ICE_PHY_TYPE_LOW_25GBASE_KR1:
		case ICE_PHY_TYPE_LOW_25GBASE_KR_S:
		case ICE_PHY_TYPE_LOW_40GBASE_KR4:
		case ICE_PHY_TYPE_LOW_50GBASE_KR_PAM4:
		case ICE_PHY_TYPE_LOW_50GBASE_KR2:
		case ICE_PHY_TYPE_LOW_100GBASE_KR4:
		case ICE_PHY_TYPE_LOW_100GBASE_KR_PAM4:
			return ICE_MEDIA_BACKPLANE;
		}
	} else {
		switch (hw_link_info->phy_type_high) {
		case ICE_PHY_TYPE_HIGH_100G_AUI2:
		case ICE_PHY_TYPE_HIGH_100G_CAUI2:
			if (ice_is_media_cage_present(pi))
				return ICE_MEDIA_DA;
			fallthrough;
		case ICE_PHY_TYPE_HIGH_100GBASE_KR2_PAM4:
			return ICE_MEDIA_BACKPLANE;
		case ICE_PHY_TYPE_HIGH_100G_CAUI2_AOC_ACC:
		case ICE_PHY_TYPE_HIGH_100G_AUI2_AOC_ACC:
			return ICE_MEDIA_FIBER;
		}
	}
	return ICE_MEDIA_UNKNOWN;
}

/**
 * ice_get_link_status_datalen
 * @hw: pointer to the HW struct
 *
 * Returns datalength for the Get Link Status AQ command, which is bigger for
 * newer adapter families handled by ice driver.
 */
static u16 ice_get_link_status_datalen(struct ice_hw *hw)
{
	switch (hw->mac_type) {
	case ICE_MAC_E830:
		return ICE_AQC_LS_DATA_SIZE_V2;
	case ICE_MAC_E810:
	default:
		return ICE_AQC_LS_DATA_SIZE_V1;
	}
}

/**
 * ice_aq_get_link_info
 * @pi: port information structure
 * @ena_lse: enable/disable LinkStatusEvent reporting
 * @link: pointer to link status structure - optional
 * @cd: pointer to command details structure or NULL
 *
 * Get Link Status (0x607). Returns the link status of the adapter.
 */
int
ice_aq_get_link_info(struct ice_port_info *pi, bool ena_lse,
		     struct ice_link_status *link, struct ice_sq_cd *cd)
{
	struct ice_aqc_get_link_status_data link_data = { 0 };
	struct ice_aqc_get_link_status *resp;
	struct ice_link_status *li_old, *li;
	enum ice_media_type *hw_media_type;
	struct ice_fc_info *hw_fc_info;
	struct libie_aq_desc desc;
	bool tx_pause, rx_pause;
	struct ice_hw *hw;
	u16 cmd_flags;
	int status;

	if (!pi)
		return -EINVAL;
	hw = pi->hw;
	li_old = &pi->phy.link_info_old;
	hw_media_type = &pi->phy.media_type;
	li = &pi->phy.link_info;
	hw_fc_info = &pi->fc;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_get_link_status);
	cmd_flags = (ena_lse) ? ICE_AQ_LSE_ENA : ICE_AQ_LSE_DIS;
	resp = libie_aq_raw(&desc);
	resp->cmd_flags = cpu_to_le16(cmd_flags);
	resp->lport_num = pi->lport;

	status = ice_aq_send_cmd(hw, &desc, &link_data,
				 ice_get_link_status_datalen(hw), cd);
	if (status)
		return status;

	/* save off old link status information */
	*li_old = *li;

	/* update current link status information */
	li->link_speed = le16_to_cpu(link_data.link_speed);
	li->phy_type_low = le64_to_cpu(link_data.phy_type_low);
	li->phy_type_high = le64_to_cpu(link_data.phy_type_high);
	*hw_media_type = ice_get_media_type(pi);
	li->link_info = link_data.link_info;
	li->link_cfg_err = link_data.link_cfg_err;
	li->an_info = link_data.an_info;
	li->ext_info = link_data.ext_info;
	li->max_frame_size = le16_to_cpu(link_data.max_frame_size);
	li->fec_info = link_data.cfg & ICE_AQ_FEC_MASK;
	li->topo_media_conflict = link_data.topo_media_conflict;
	li->pacing = link_data.cfg & (ICE_AQ_CFG_PACING_M |
				      ICE_AQ_CFG_PACING_TYPE_M);

	/* update fc info */
	tx_pause = !!(link_data.an_info & ICE_AQ_LINK_PAUSE_TX);
	rx_pause = !!(link_data.an_info & ICE_AQ_LINK_PAUSE_RX);
	if (tx_pause && rx_pause)
		hw_fc_info->current_mode = ICE_FC_FULL;
	else if (tx_pause)
		hw_fc_info->current_mode = ICE_FC_TX_PAUSE;
	else if (rx_pause)
		hw_fc_info->current_mode = ICE_FC_RX_PAUSE;
	else
		hw_fc_info->current_mode = ICE_FC_NONE;

	li->lse_ena = !!(resp->cmd_flags & cpu_to_le16(ICE_AQ_LSE_IS_ENABLED));

	ice_debug(hw, ICE_DBG_LINK, "get link info\n");
	ice_debug(hw, ICE_DBG_LINK, "	link_speed = 0x%x\n", li->link_speed);
	ice_debug(hw, ICE_DBG_LINK, "	phy_type_low = 0x%llx\n",
		  (unsigned long long)li->phy_type_low);
	ice_debug(hw, ICE_DBG_LINK, "	phy_type_high = 0x%llx\n",
		  (unsigned long long)li->phy_type_high);
	ice_debug(hw, ICE_DBG_LINK, "	media_type = 0x%x\n", *hw_media_type);
	ice_debug(hw, ICE_DBG_LINK, "	link_info = 0x%x\n", li->link_info);
	ice_debug(hw, ICE_DBG_LINK, "	link_cfg_err = 0x%x\n", li->link_cfg_err);
	ice_debug(hw, ICE_DBG_LINK, "	an_info = 0x%x\n", li->an_info);
	ice_debug(hw, ICE_DBG_LINK, "	ext_info = 0x%x\n", li->ext_info);
	ice_debug(hw, ICE_DBG_LINK, "	fec_info = 0x%x\n", li->fec_info);
	ice_debug(hw, ICE_DBG_LINK, "	lse_ena = 0x%x\n", li->lse_ena);
	ice_debug(hw, ICE_DBG_LINK, "	max_frame = 0x%x\n",
		  li->max_frame_size);
	ice_debug(hw, ICE_DBG_LINK, "	pacing = 0x%x\n", li->pacing);

	/* save link status information */
	if (link)
		*link = *li;

	/* flag cleared so calling functions don't call AQ again */
	pi->phy.get_link_info = false;

	return 0;
}

/**
 * ice_fill_tx_timer_and_fc_thresh
 * @hw: pointer to the HW struct
 * @cmd: pointer to MAC cfg structure
 *
 * Add Tx timer and FC refresh threshold info to Set MAC Config AQ command
 * descriptor
 */
static void
ice_fill_tx_timer_and_fc_thresh(struct ice_hw *hw,
				struct ice_aqc_set_mac_cfg *cmd)
{
	u32 val, fc_thres_m;

	/* We read back the transmit timer and FC threshold value of
	 * LFC. Thus, we will use index =
	 * PRTMAC_HSEC_CTL_TX_PAUSE_QUANTA_MAX_INDEX.
	 *
	 * Also, because we are operating on transmit timer and FC
	 * threshold of LFC, we don't turn on any bit in tx_tmr_priority
	 */
#define E800_IDX_OF_LFC E800_PRTMAC_HSEC_CTL_TX_PS_QNT_MAX
#define E800_REFRESH_TMR E800_PRTMAC_HSEC_CTL_TX_PS_RFSH_TMR

	if (hw->mac_type == ICE_MAC_E830) {
		/* Retrieve the transmit timer */
		val = rd32(hw, E830_PRTMAC_CL01_PS_QNT);
		cmd->tx_tmr_value =
			le16_encode_bits(val, E830_PRTMAC_CL01_PS_QNT_CL0_M);

		/* Retrieve the fc threshold */
		val = rd32(hw, E830_PRTMAC_CL01_QNT_THR);
		fc_thres_m = E830_PRTMAC_CL01_QNT_THR_CL0_M;
	} else {
		/* Retrieve the transmit timer */
		val = rd32(hw,
			   E800_PRTMAC_HSEC_CTL_TX_PS_QNT(E800_IDX_OF_LFC));
		cmd->tx_tmr_value =
			le16_encode_bits(val,
					 E800_PRTMAC_HSEC_CTL_TX_PS_QNT_M);

		/* Retrieve the fc threshold */
		val = rd32(hw,
			   E800_REFRESH_TMR(E800_IDX_OF_LFC));
		fc_thres_m = E800_PRTMAC_HSEC_CTL_TX_PS_RFSH_TMR_M;
	}
	cmd->fc_refresh_threshold = le16_encode_bits(val, fc_thres_m);
}

/**
 * ice_aq_set_mac_cfg
 * @hw: pointer to the HW struct
 * @max_frame_size: Maximum Frame Size to be supported
 * @cd: pointer to command details structure or NULL
 *
 * Set MAC configuration (0x0603)
 */
int
ice_aq_set_mac_cfg(struct ice_hw *hw, u16 max_frame_size, struct ice_sq_cd *cd)
{
	struct ice_aqc_set_mac_cfg *cmd;
	struct libie_aq_desc desc;

	cmd = libie_aq_raw(&desc);

	if (max_frame_size == 0)
		return -EINVAL;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_set_mac_cfg);

	cmd->max_frame_size = cpu_to_le16(max_frame_size);

	ice_fill_tx_timer_and_fc_thresh(hw, cmd);

	return ice_aq_send_cmd(hw, &desc, NULL, 0, cd);
}

/**
 * ice_init_fltr_mgmt_struct - initializes filter management list and locks
 * @hw: pointer to the HW struct
 */
static int ice_init_fltr_mgmt_struct(struct ice_hw *hw)
{
	struct ice_switch_info *sw;
	int status;

	hw->switch_info = devm_kzalloc(ice_hw_to_dev(hw),
				       sizeof(*hw->switch_info), GFP_KERNEL);
	sw = hw->switch_info;

	if (!sw)
		return -ENOMEM;

	INIT_LIST_HEAD(&sw->vsi_list_map_head);
	sw->prof_res_bm_init = 0;

	/* Initialize recipe count with default recipes read from NVM */
	sw->recp_cnt = ICE_SW_LKUP_LAST;

	status = ice_init_def_sw_recp(hw);
	if (status) {
		devm_kfree(ice_hw_to_dev(hw), hw->switch_info);
		return status;
	}
	return 0;
}

/**
 * ice_cleanup_fltr_mgmt_struct - cleanup filter management list and locks
 * @hw: pointer to the HW struct
 */
static void ice_cleanup_fltr_mgmt_struct(struct ice_hw *hw)
{
	struct ice_switch_info *sw = hw->switch_info;
	struct ice_vsi_list_map_info *v_pos_map;
	struct ice_vsi_list_map_info *v_tmp_map;
	struct ice_sw_recipe *recps;
	u8 i;

	list_for_each_entry_safe(v_pos_map, v_tmp_map, &sw->vsi_list_map_head,
				 list_entry) {
		list_del(&v_pos_map->list_entry);
		devm_kfree(ice_hw_to_dev(hw), v_pos_map);
	}
	recps = sw->recp_list;
	for (i = 0; i < ICE_MAX_NUM_RECIPES; i++) {
		recps[i].root_rid = i;

		if (recps[i].adv_rule) {
			struct ice_adv_fltr_mgmt_list_entry *tmp_entry;
			struct ice_adv_fltr_mgmt_list_entry *lst_itr;

			mutex_destroy(&recps[i].filt_rule_lock);
			list_for_each_entry_safe(lst_itr, tmp_entry,
						 &recps[i].filt_rules,
						 list_entry) {
				list_del(&lst_itr->list_entry);
				devm_kfree(ice_hw_to_dev(hw), lst_itr->lkups);
				devm_kfree(ice_hw_to_dev(hw), lst_itr);
			}
		} else {
			struct ice_fltr_mgmt_list_entry *lst_itr, *tmp_entry;

			mutex_destroy(&recps[i].filt_rule_lock);
			list_for_each_entry_safe(lst_itr, tmp_entry,
						 &recps[i].filt_rules,
						 list_entry) {
				list_del(&lst_itr->list_entry);
				devm_kfree(ice_hw_to_dev(hw), lst_itr);
			}
		}
	}
	ice_rm_all_sw_replay_rule_info(hw);
	devm_kfree(ice_hw_to_dev(hw), sw->recp_list);
	devm_kfree(ice_hw_to_dev(hw), sw);
}

/**
 * ice_get_itr_intrl_gran
 * @hw: pointer to the HW struct
 *
 * Determines the ITR/INTRL granularities based on the maximum aggregate
 * bandwidth according to the device's configuration during power-on.
 */
static void ice_get_itr_intrl_gran(struct ice_hw *hw)
{
	u8 max_agg_bw = FIELD_GET(GL_PWR_MODE_CTL_CAR_MAX_BW_M,
				  rd32(hw, GL_PWR_MODE_CTL));

	switch (max_agg_bw) {
	case ICE_MAX_AGG_BW_200G:
	case ICE_MAX_AGG_BW_100G:
	case ICE_MAX_AGG_BW_50G:
		hw->itr_gran = ICE_ITR_GRAN_ABOVE_25;
		hw->intrl_gran = ICE_INTRL_GRAN_ABOVE_25;
		break;
	case ICE_MAX_AGG_BW_25G:
		hw->itr_gran = ICE_ITR_GRAN_MAX_25;
		hw->intrl_gran = ICE_INTRL_GRAN_MAX_25;
		break;
	}
}

/**
 * ice_wait_for_fw - wait for full FW readiness
 * @hw: pointer to the hardware structure
 * @timeout: milliseconds that can elapse before timing out
 *
 * Return: 0 on success, -ETIMEDOUT on timeout.
 */
static int ice_wait_for_fw(struct ice_hw *hw, u32 timeout)
{
	int fw_loading;
	u32 elapsed = 0;

	while (elapsed <= timeout) {
		fw_loading = rd32(hw, GL_MNG_FWSM) & GL_MNG_FWSM_FW_LOADING_M;

		/* firmware was not yet loaded, we have to wait more */
		if (fw_loading) {
			elapsed += 100;
			msleep(100);
			continue;
		}
		return 0;
	}

	return -ETIMEDOUT;
}

/**
 * ice_init_hw - main hardware initialization routine
 * @hw: pointer to the hardware structure
 */
int ice_init_hw(struct ice_hw *hw)
{
	struct ice_aqc_get_phy_caps_data *pcaps __free(kfree) = NULL;
	void *mac_buf __free(kfree) = NULL;
	u16 mac_buf_len;
	int status;

	/* Set MAC type based on DeviceID */
	status = ice_set_mac_type(hw);
	if (status)
		return status;

	hw->pf_id = FIELD_GET(PF_FUNC_RID_FUNC_NUM_M, rd32(hw, PF_FUNC_RID));

	status = ice_reset(hw, ICE_RESET_PFR);
	if (status)
		return status;

	ice_get_itr_intrl_gran(hw);

	status = ice_create_all_ctrlq(hw);
	if (status)
		goto err_unroll_cqinit;

	status = ice_fwlog_init(hw);
	if (status)
		ice_debug(hw, ICE_DBG_FW_LOG, "Error initializing FW logging: %d\n",
			  status);

	status = ice_clear_pf_cfg(hw);
	if (status)
		goto err_unroll_cqinit;

	/* Set bit to enable Flow Director filters */
	wr32(hw, PFQF_FD_ENA, PFQF_FD_ENA_FD_ENA_M);
	INIT_LIST_HEAD(&hw->fdir_list_head);

	ice_clear_pxe_mode(hw);

	status = ice_init_nvm(hw);
	if (status)
		goto err_unroll_cqinit;

	status = ice_get_caps(hw);
	if (status)
		goto err_unroll_cqinit;

	if (!hw->port_info)
		hw->port_info = devm_kzalloc(ice_hw_to_dev(hw),
					     sizeof(*hw->port_info),
					     GFP_KERNEL);
	if (!hw->port_info) {
		status = -ENOMEM;
		goto err_unroll_cqinit;
	}

	hw->port_info->local_fwd_mode = ICE_LOCAL_FWD_MODE_ENABLED;
	/* set the back pointer to HW */
	hw->port_info->hw = hw;

	/* Initialize port_info struct with switch configuration data */
	status = ice_get_initial_sw_cfg(hw);
	if (status)
		goto err_unroll_alloc;

	hw->evb_veb = true;

	/* init xarray for identifying scheduling nodes uniquely */
	xa_init_flags(&hw->port_info->sched_node_ids, XA_FLAGS_ALLOC);

	/* Query the allocated resources for Tx scheduler */
	status = ice_sched_query_res_alloc(hw);
	if (status) {
		ice_debug(hw, ICE_DBG_SCHED, "Failed to get scheduler allocated resources\n");
		goto err_unroll_alloc;
	}
	ice_sched_get_psm_clk_freq(hw);

	/* Initialize port_info struct with scheduler data */
	status = ice_sched_init_port(hw->port_info);
	if (status)
		goto err_unroll_sched;

	pcaps = kzalloc(sizeof(*pcaps), GFP_KERNEL);
	if (!pcaps) {
		status = -ENOMEM;
		goto err_unroll_sched;
	}

	/* Initialize port_info struct with PHY capabilities */
	status = ice_aq_get_phy_caps(hw->port_info, false,
				     ICE_AQC_REPORT_TOPO_CAP_MEDIA, pcaps,
				     NULL);
	if (status)
		dev_warn(ice_hw_to_dev(hw), "Get PHY capabilities failed status = %d, continuing anyway\n",
			 status);

	/* Initialize port_info struct with link information */
	status = ice_aq_get_link_info(hw->port_info, false, NULL, NULL);
	if (status)
		goto err_unroll_sched;

	/* need a valid SW entry point to build a Tx tree */
	if (!hw->sw_entry_point_layer) {
		ice_debug(hw, ICE_DBG_SCHED, "invalid sw entry point\n");
		status = -EIO;
		goto err_unroll_sched;
	}
	INIT_LIST_HEAD(&hw->agg_list);
	/* Initialize max burst size */
	if (!hw->max_burst_size)
		ice_cfg_rl_burst_size(hw, ICE_SCHED_DFLT_BURST_SIZE);

	status = ice_init_fltr_mgmt_struct(hw);
	if (status)
		goto err_unroll_sched;

	/* Get MAC information */
	/* A single port can report up to two (LAN and WoL) addresses */
	mac_buf = kcalloc(2, sizeof(struct ice_aqc_manage_mac_read_resp),
			  GFP_KERNEL);
	if (!mac_buf) {
		status = -ENOMEM;
		goto err_unroll_fltr_mgmt_struct;
	}

	mac_buf_len = 2 * sizeof(struct ice_aqc_manage_mac_read_resp);
	status = ice_aq_manage_mac_read(hw, mac_buf, mac_buf_len, NULL);

	if (status)
		goto err_unroll_fltr_mgmt_struct;
	/* enable jumbo frame support at MAC level */
	status = ice_aq_set_mac_cfg(hw, ICE_AQ_SET_MAC_FRAME_SIZE_MAX, NULL);
	if (status)
		goto err_unroll_fltr_mgmt_struct;
	/* Obtain counter base index which would be used by flow director */
	status = ice_alloc_fd_res_cntr(hw, &hw->fd_ctr_base);
	if (status)
		goto err_unroll_fltr_mgmt_struct;
	status = ice_init_hw_tbls(hw);
	if (status)
		goto err_unroll_fltr_mgmt_struct;
	mutex_init(&hw->tnl_lock);
	ice_init_chk_recipe_reuse_support(hw);

	/* Some cards require longer initialization times
	 * due to necessity of loading FW from an external source.
	 * This can take even half a minute.
	 */
	if (ice_is_pf_c827(hw)) {
		status = ice_wait_for_fw(hw, 30000);
		if (status) {
			dev_err(ice_hw_to_dev(hw), "ice_wait_for_fw timed out");
			goto err_unroll_fltr_mgmt_struct;
		}
	}

	hw->lane_num = ice_get_phy_lane_number(hw);

	return 0;
err_unroll_fltr_mgmt_struct:
	ice_cleanup_fltr_mgmt_struct(hw);
err_unroll_sched:
	ice_sched_cleanup_all(hw);
err_unroll_alloc:
	devm_kfree(ice_hw_to_dev(hw), hw->port_info);
err_unroll_cqinit:
	ice_destroy_all_ctrlq(hw);
	return status;
}

/**
 * ice_deinit_hw - unroll initialization operations done by ice_init_hw
 * @hw: pointer to the hardware structure
 *
 * This should be called only during nominal operation, not as a result of
 * ice_init_hw() failing since ice_init_hw() will take care of unrolling
 * applicable initializations if it fails for any reason.
 */
void ice_deinit_hw(struct ice_hw *hw)
{
	ice_free_fd_res_cntr(hw, hw->fd_ctr_base);
	ice_cleanup_fltr_mgmt_struct(hw);

	ice_sched_cleanup_all(hw);
	ice_sched_clear_agg(hw);
	ice_free_seg(hw);
	ice_free_hw_tbls(hw);
	mutex_destroy(&hw->tnl_lock);

	ice_fwlog_deinit(hw);
	ice_destroy_all_ctrlq(hw);

	/* Clear VSI contexts if not already cleared */
	ice_clear_all_vsi_ctx(hw);
}

/**
 * ice_check_reset - Check to see if a global reset is complete
 * @hw: pointer to the hardware structure
 */
int ice_check_reset(struct ice_hw *hw)
{
	u32 cnt, reg = 0, grst_timeout, uld_mask;

	/* Poll for Device Active state in case a recent CORER, GLOBR,
	 * or EMPR has occurred. The grst delay value is in 100ms units.
	 * Add 1sec for outstanding AQ commands that can take a long time.
	 */
	grst_timeout = FIELD_GET(GLGEN_RSTCTL_GRSTDEL_M,
				 rd32(hw, GLGEN_RSTCTL)) + 10;

	for (cnt = 0; cnt < grst_timeout; cnt++) {
		mdelay(100);
		reg = rd32(hw, GLGEN_RSTAT);
		if (!(reg & GLGEN_RSTAT_DEVSTATE_M))
			break;
	}

	if (cnt == grst_timeout) {
		ice_debug(hw, ICE_DBG_INIT, "Global reset polling failed to complete.\n");
		return -EIO;
	}

#define ICE_RESET_DONE_MASK	(GLNVM_ULD_PCIER_DONE_M |\
				 GLNVM_ULD_PCIER_DONE_1_M |\
				 GLNVM_ULD_CORER_DONE_M |\
				 GLNVM_ULD_GLOBR_DONE_M |\
				 GLNVM_ULD_POR_DONE_M |\
				 GLNVM_ULD_POR_DONE_1_M |\
				 GLNVM_ULD_PCIER_DONE_2_M)

	uld_mask = ICE_RESET_DONE_MASK | (hw->func_caps.common_cap.rdma ?
					  GLNVM_ULD_PE_DONE_M : 0);

	/* Device is Active; check Global Reset processes are done */
	for (cnt = 0; cnt < ICE_PF_RESET_WAIT_COUNT; cnt++) {
		reg = rd32(hw, GLNVM_ULD) & uld_mask;
		if (reg == uld_mask) {
			ice_debug(hw, ICE_DBG_INIT, "Global reset processes done. %d\n", cnt);
			break;
		}
		mdelay(10);
	}

	if (cnt == ICE_PF_RESET_WAIT_COUNT) {
		ice_debug(hw, ICE_DBG_INIT, "Wait for Reset Done timed out. GLNVM_ULD = 0x%x\n",
			  reg);
		return -EIO;
	}

	return 0;
}

/**
 * ice_pf_reset - Reset the PF
 * @hw: pointer to the hardware structure
 *
 * If a global reset has been triggered, this function checks
 * for its completion and then issues the PF reset
 */
static int ice_pf_reset(struct ice_hw *hw)
{
	u32 cnt, reg;

	/* If at function entry a global reset was already in progress, i.e.
	 * state is not 'device active' or any of the reset done bits are not
	 * set in GLNVM_ULD, there is no need for a PF Reset; poll until the
	 * global reset is done.
	 */
	if ((rd32(hw, GLGEN_RSTAT) & GLGEN_RSTAT_DEVSTATE_M) ||
	    (rd32(hw, GLNVM_ULD) & ICE_RESET_DONE_MASK) ^ ICE_RESET_DONE_MASK) {
		/* poll on global reset currently in progress until done */
		if (ice_check_reset(hw))
			return -EIO;

		return 0;
	}

	/* Reset the PF */
	reg = rd32(hw, PFGEN_CTRL);

	wr32(hw, PFGEN_CTRL, (reg | PFGEN_CTRL_PFSWR_M));

	/* Wait for the PFR to complete. The wait time is the global config lock
	 * timeout plus the PFR timeout which will account for a possible reset
	 * that is occurring during a download package operation.
	 */
	for (cnt = 0; cnt < ICE_GLOBAL_CFG_LOCK_TIMEOUT +
	     ICE_PF_RESET_WAIT_COUNT; cnt++) {
		reg = rd32(hw, PFGEN_CTRL);
		if (!(reg & PFGEN_CTRL_PFSWR_M))
			break;

		mdelay(1);
	}

	if (cnt == ICE_PF_RESET_WAIT_COUNT) {
		ice_debug(hw, ICE_DBG_INIT, "PF reset polling failed to complete.\n");
		return -EIO;
	}

	return 0;
}

/**
 * ice_reset - Perform different types of reset
 * @hw: pointer to the hardware structure
 * @req: reset request
 *
 * This function triggers a reset as specified by the req parameter.
 *
 * Note:
 * If anything other than a PF reset is triggered, PXE mode is restored.
 * This has to be cleared using ice_clear_pxe_mode again, once the AQ
 * interface has been restored in the rebuild flow.
 */
int ice_reset(struct ice_hw *hw, enum ice_reset_req req)
{
	u32 val = 0;

	switch (req) {
	case ICE_RESET_PFR:
		return ice_pf_reset(hw);
	case ICE_RESET_CORER:
		ice_debug(hw, ICE_DBG_INIT, "CoreR requested\n");
		val = GLGEN_RTRIG_CORER_M;
		break;
	case ICE_RESET_GLOBR:
		ice_debug(hw, ICE_DBG_INIT, "GlobalR requested\n");
		val = GLGEN_RTRIG_GLOBR_M;
		break;
	default:
		return -EINVAL;
	}

	val |= rd32(hw, GLGEN_RTRIG);
	wr32(hw, GLGEN_RTRIG, val);
	ice_flush(hw);

	/* wait for the FW to be ready */
	return ice_check_reset(hw);
}

/**
 * ice_copy_rxq_ctx_to_hw - Copy packed Rx queue context to HW registers
 * @hw: pointer to the hardware structure
 * @rxq_ctx: pointer to the packed Rx queue context
 * @rxq_index: the index of the Rx queue
 */
static void ice_copy_rxq_ctx_to_hw(struct ice_hw *hw,
				   const ice_rxq_ctx_buf_t *rxq_ctx,
				   u32 rxq_index)
{
	/* Copy each dword separately to HW */
	for (int i = 0; i < ICE_RXQ_CTX_SIZE_DWORDS; i++) {
		u32 ctx = ((const u32 *)rxq_ctx)[i];

		wr32(hw, QRX_CONTEXT(i, rxq_index), ctx);

		ice_debug(hw, ICE_DBG_QCTX, "qrxdata[%d]: %08X\n", i, ctx);
	}
}

/**
 * ice_copy_rxq_ctx_from_hw - Copy packed Rx Queue context from HW registers
 * @hw: pointer to the hardware structure
 * @rxq_ctx: pointer to the packed Rx queue context
 * @rxq_index: the index of the Rx queue
 */
static void ice_copy_rxq_ctx_from_hw(struct ice_hw *hw,
				     ice_rxq_ctx_buf_t *rxq_ctx,
				     u32 rxq_index)
{
	u32 *ctx = (u32 *)rxq_ctx;

	/* Copy each dword separately from HW */
	for (int i = 0; i < ICE_RXQ_CTX_SIZE_DWORDS; i++, ctx++) {
		*ctx = rd32(hw, QRX_CONTEXT(i, rxq_index));

		ice_debug(hw, ICE_DBG_QCTX, "qrxdata[%d]: %08X\n", i, *ctx);
	}
}

#define ICE_CTX_STORE(struct_name, struct_field, width, lsb) \
	PACKED_FIELD((lsb) + (width) - 1, (lsb), struct struct_name, struct_field)

/* LAN Rx Queue Context */
static const struct packed_field_u8 ice_rlan_ctx_fields[] = {
				 /* Field		Width	LSB */
	ICE_CTX_STORE(ice_rlan_ctx, head,		13,	0),
	ICE_CTX_STORE(ice_rlan_ctx, cpuid,		8,	13),
	ICE_CTX_STORE(ice_rlan_ctx, base,		57,	32),
	ICE_CTX_STORE(ice_rlan_ctx, qlen,		13,	89),
	ICE_CTX_STORE(ice_rlan_ctx, dbuf,		7,	102),
	ICE_CTX_STORE(ice_rlan_ctx, hbuf,		5,	109),
	ICE_CTX_STORE(ice_rlan_ctx, dtype,		2,	114),
	ICE_CTX_STORE(ice_rlan_ctx, dsize,		1,	116),
	ICE_CTX_STORE(ice_rlan_ctx, crcstrip,		1,	117),
	ICE_CTX_STORE(ice_rlan_ctx, l2tsel,		1,	119),
	ICE_CTX_STORE(ice_rlan_ctx, hsplit_0,		4,	120),
	ICE_CTX_STORE(ice_rlan_ctx, hsplit_1,		2,	124),
	ICE_CTX_STORE(ice_rlan_ctx, showiv,		1,	127),
	ICE_CTX_STORE(ice_rlan_ctx, rxmax,		14,	174),
	ICE_CTX_STORE(ice_rlan_ctx, tphrdesc_ena,	1,	193),
	ICE_CTX_STORE(ice_rlan_ctx, tphwdesc_ena,	1,	194),
	ICE_CTX_STORE(ice_rlan_ctx, tphdata_ena,	1,	195),
	ICE_CTX_STORE(ice_rlan_ctx, tphhead_ena,	1,	196),
	ICE_CTX_STORE(ice_rlan_ctx, lrxqthresh,		3,	198),
	ICE_CTX_STORE(ice_rlan_ctx, prefena,		1,	201),
};

/**
 * ice_pack_rxq_ctx - Pack Rx queue context into a HW buffer
 * @ctx: the Rx queue context to pack
 * @buf: the HW buffer to pack into
 *
 * Pack the Rx queue context from the CPU-friendly unpacked buffer into its
 * bit-packed HW layout.
 */
static void ice_pack_rxq_ctx(const struct ice_rlan_ctx *ctx,
			     ice_rxq_ctx_buf_t *buf)
{
	pack_fields(buf, sizeof(*buf), ctx, ice_rlan_ctx_fields,
		    QUIRK_LITTLE_ENDIAN | QUIRK_LSW32_IS_FIRST);
}

/**
 * ice_unpack_rxq_ctx - Unpack Rx queue context from a HW buffer
 * @buf: the HW buffer to unpack from
 * @ctx: the Rx queue context to unpack
 *
 * Unpack the Rx queue context from the HW buffer into the CPU-friendly
 * structure.
 */
static void ice_unpack_rxq_ctx(const ice_rxq_ctx_buf_t *buf,
			       struct ice_rlan_ctx *ctx)
{
	unpack_fields(buf, sizeof(*buf), ctx, ice_rlan_ctx_fields,
		      QUIRK_LITTLE_ENDIAN | QUIRK_LSW32_IS_FIRST);
}

/**
 * ice_write_rxq_ctx - Write Rx Queue context to hardware
 * @hw: pointer to the hardware structure
 * @rlan_ctx: pointer to the unpacked Rx queue context
 * @rxq_index: the index of the Rx queue
 *
 * Pack the sparse Rx Queue context into dense hardware format and write it
 * into the HW register space.
 *
 * Return: 0 on success, or -EINVAL if the Rx queue index is invalid.
 */
int ice_write_rxq_ctx(struct ice_hw *hw, struct ice_rlan_ctx *rlan_ctx,
		      u32 rxq_index)
{
	ice_rxq_ctx_buf_t buf = {};

	if (rxq_index > QRX_CTRL_MAX_INDEX)
		return -EINVAL;

	ice_pack_rxq_ctx(rlan_ctx, &buf);
	ice_copy_rxq_ctx_to_hw(hw, &buf, rxq_index);

	return 0;
}

/**
 * ice_read_rxq_ctx - Read Rx queue context from HW
 * @hw: pointer to the hardware structure
 * @rlan_ctx: pointer to the Rx queue context
 * @rxq_index: the index of the Rx queue
 *
 * Read the Rx queue context from the hardware registers, and unpack it into
 * the sparse Rx queue context structure.
 *
 * Returns: 0 on success, or -EINVAL if the Rx queue index is invalid.
 */
int ice_read_rxq_ctx(struct ice_hw *hw, struct ice_rlan_ctx *rlan_ctx,
		     u32 rxq_index)
{
	ice_rxq_ctx_buf_t buf = {};

	if (rxq_index > QRX_CTRL_MAX_INDEX)
		return -EINVAL;

	ice_copy_rxq_ctx_from_hw(hw, &buf, rxq_index);
	ice_unpack_rxq_ctx(&buf, rlan_ctx);

	return 0;
}

/* LAN Tx Queue Context */
static const struct packed_field_u8 ice_tlan_ctx_fields[] = {
				    /* Field			Width	LSB */
	ICE_CTX_STORE(ice_tlan_ctx, base,			57,	0),
	ICE_CTX_STORE(ice_tlan_ctx, port_num,			3,	57),
	ICE_CTX_STORE(ice_tlan_ctx, cgd_num,			5,	60),
	ICE_CTX_STORE(ice_tlan_ctx, pf_num,			3,	65),
	ICE_CTX_STORE(ice_tlan_ctx, vmvf_num,			10,	68),
	ICE_CTX_STORE(ice_tlan_ctx, vmvf_type,			2,	78),
	ICE_CTX_STORE(ice_tlan_ctx, src_vsi,			10,	80),
	ICE_CTX_STORE(ice_tlan_ctx, tsyn_ena,			1,	90),
	ICE_CTX_STORE(ice_tlan_ctx, internal_usage_flag,	1,	91),
	ICE_CTX_STORE(ice_tlan_ctx, alt_vlan,			1,	92),
	ICE_CTX_STORE(ice_tlan_ctx, cpuid,			8,	93),
	ICE_CTX_STORE(ice_tlan_ctx, wb_mode,			1,	101),
	ICE_CTX_STORE(ice_tlan_ctx, tphrd_desc,			1,	102),
	ICE_CTX_STORE(ice_tlan_ctx, tphrd,			1,	103),
	ICE_CTX_STORE(ice_tlan_ctx, tphwr_desc,			1,	104),
	ICE_CTX_STORE(ice_tlan_ctx, cmpq_id,			9,	105),
	ICE_CTX_STORE(ice_tlan_ctx, qnum_in_func,		14,	114),
	ICE_CTX_STORE(ice_tlan_ctx, itr_notification_mode,	1,	128),
	ICE_CTX_STORE(ice_tlan_ctx, adjust_prof_id,		6,	129),
	ICE_CTX_STORE(ice_tlan_ctx, qlen,			13,	135),
	ICE_CTX_STORE(ice_tlan_ctx, quanta_prof_idx,		4,	148),
	ICE_CTX_STORE(ice_tlan_ctx, tso_ena,			1,	152),
	ICE_CTX_STORE(ice_tlan_ctx, tso_qnum,			11,	153),
	ICE_CTX_STORE(ice_tlan_ctx, legacy_int,			1,	164),
	ICE_CTX_STORE(ice_tlan_ctx, drop_ena,			1,	165),
	ICE_CTX_STORE(ice_tlan_ctx, cache_prof_idx,		2,	166),
	ICE_CTX_STORE(ice_tlan_ctx, pkt_shaper_prof_idx,	3,	168),
};

/**
 * ice_pack_txq_ctx - Pack Tx queue context into Admin Queue buffer
 * @ctx: the Tx queue context to pack
 * @buf: the Admin Queue HW buffer to pack into
 *
 * Pack the Tx queue context from the CPU-friendly unpacked buffer into its
 * bit-packed Admin Queue layout.
 */
void ice_pack_txq_ctx(const struct ice_tlan_ctx *ctx, ice_txq_ctx_buf_t *buf)
{
	pack_fields(buf, sizeof(*buf), ctx, ice_tlan_ctx_fields,
		    QUIRK_LITTLE_ENDIAN | QUIRK_LSW32_IS_FIRST);
}

/**
 * ice_pack_txq_ctx_full - Pack Tx queue context into a HW buffer
 * @ctx: the Tx queue context to pack
 * @buf: the HW buffer to pack into
 *
 * Pack the Tx queue context from the CPU-friendly unpacked buffer into its
 * bit-packed HW layout, including the internal data portion.
 */
static void ice_pack_txq_ctx_full(const struct ice_tlan_ctx *ctx,
				  ice_txq_ctx_buf_full_t *buf)
{
	pack_fields(buf, sizeof(*buf), ctx, ice_tlan_ctx_fields,
		    QUIRK_LITTLE_ENDIAN | QUIRK_LSW32_IS_FIRST);
}

/**
 * ice_unpack_txq_ctx_full - Unpack Tx queue context from a HW buffer
 * @buf: the HW buffer to unpack from
 * @ctx: the Tx queue context to unpack
 *
 * Unpack the Tx queue context from the HW buffer (including the full internal
 * state) into the CPU-friendly structure.
 */
static void ice_unpack_txq_ctx_full(const ice_txq_ctx_buf_full_t *buf,
				    struct ice_tlan_ctx *ctx)
{
	unpack_fields(buf, sizeof(*buf), ctx, ice_tlan_ctx_fields,
		      QUIRK_LITTLE_ENDIAN | QUIRK_LSW32_IS_FIRST);
}

/**
 * ice_copy_txq_ctx_from_hw - Copy Tx Queue context from HW registers
 * @hw: pointer to the hardware structure
 * @txq_ctx: pointer to the packed Tx queue context, including internal state
 * @txq_index: the index of the Tx queue
 *
 * Copy Tx Queue context from HW register space to dense structure
 */
static void ice_copy_txq_ctx_from_hw(struct ice_hw *hw,
				     ice_txq_ctx_buf_full_t *txq_ctx,
				     u32 txq_index)
{
	struct ice_pf *pf = container_of(hw, struct ice_pf, hw);
	u32 *ctx = (u32 *)txq_ctx;
	u32 txq_base, reg;

	/* Get Tx queue base within card space */
	txq_base = rd32(hw, PFLAN_TX_QALLOC(hw->pf_id));
	txq_base = FIELD_GET(PFLAN_TX_QALLOC_FIRSTQ_M, txq_base);

	reg = FIELD_PREP(GLCOMM_QTX_CNTX_CTL_CMD_M,
			 GLCOMM_QTX_CNTX_CTL_CMD_READ) |
	      FIELD_PREP(GLCOMM_QTX_CNTX_CTL_QUEUE_ID_M,
			 txq_base + txq_index) |
	      GLCOMM_QTX_CNTX_CTL_CMD_EXEC_M;

	/* Prevent other PFs on the same adapter from accessing the Tx queue
	 * context interface concurrently.
	 */
	spin_lock(&pf->adapter->txq_ctx_lock);

	wr32(hw, GLCOMM_QTX_CNTX_CTL, reg);
	ice_flush(hw);

	/* Copy each dword separately from HW */
	for (int i = 0; i < ICE_TXQ_CTX_FULL_SIZE_DWORDS; i++, ctx++) {
		*ctx = rd32(hw, GLCOMM_QTX_CNTX_DATA(i));

		ice_debug(hw, ICE_DBG_QCTX, "qtxdata[%d]: %08X\n", i, *ctx);
	}

	spin_unlock(&pf->adapter->txq_ctx_lock);
}

/**
 * ice_copy_txq_ctx_to_hw - Copy Tx Queue context into HW registers
 * @hw: pointer to the hardware structure
 * @txq_ctx: pointer to the packed Tx queue context, including internal state
 * @txq_index: the index of the Tx queue
 */
static void ice_copy_txq_ctx_to_hw(struct ice_hw *hw,
				   const ice_txq_ctx_buf_full_t *txq_ctx,
				   u32 txq_index)
{
	struct ice_pf *pf = container_of(hw, struct ice_pf, hw);
	u32 txq_base, reg;

	/* Get Tx queue base within card space */
	txq_base = rd32(hw, PFLAN_TX_QALLOC(hw->pf_id));
	txq_base = FIELD_GET(PFLAN_TX_QALLOC_FIRSTQ_M, txq_base);

	reg = FIELD_PREP(GLCOMM_QTX_CNTX_CTL_CMD_M,
			 GLCOMM_QTX_CNTX_CTL_CMD_WRITE_NO_DYN) |
	      FIELD_PREP(GLCOMM_QTX_CNTX_CTL_QUEUE_ID_M,
			 txq_base + txq_index) |
	      GLCOMM_QTX_CNTX_CTL_CMD_EXEC_M;

	/* Prevent other PFs on the same adapter from accessing the Tx queue
	 * context interface concurrently.
	 */
	spin_lock(&pf->adapter->txq_ctx_lock);

	/* Copy each dword separately to HW */
	for (int i = 0; i < ICE_TXQ_CTX_FULL_SIZE_DWORDS; i++) {
		u32 ctx = ((const u32 *)txq_ctx)[i];

		wr32(hw, GLCOMM_QTX_CNTX_DATA(i), ctx);

		ice_debug(hw, ICE_DBG_QCTX, "qtxdata[%d]: %08X\n", i, ctx);
	}

	wr32(hw, GLCOMM_QTX_CNTX_CTL, reg);
	ice_flush(hw);

	spin_unlock(&pf->adapter->txq_ctx_lock);
}

/**
 * ice_read_txq_ctx - Read Tx queue context from HW
 * @hw: pointer to the hardware structure
 * @tlan_ctx: pointer to the Tx queue context
 * @txq_index: the index of the Tx queue
 *
 * Read the Tx queue context from the HW registers, then unpack it into the
 * ice_tlan_ctx structure for use.
 *
 * Returns: 0 on success, or -EINVAL on an invalid Tx queue index.
 */
int ice_read_txq_ctx(struct ice_hw *hw, struct ice_tlan_ctx *tlan_ctx,
		     u32 txq_index)
{
	ice_txq_ctx_buf_full_t buf = {};

	if (txq_index > QTX_COMM_HEAD_MAX_INDEX)
		return -EINVAL;

	ice_copy_txq_ctx_from_hw(hw, &buf, txq_index);
	ice_unpack_txq_ctx_full(&buf, tlan_ctx);

	return 0;
}

/**
 * ice_write_txq_ctx - Write Tx queue context to HW
 * @hw: pointer to the hardware structure
 * @tlan_ctx: pointer to the Tx queue context
 * @txq_index: the index of the Tx queue
 *
 * Pack the Tx queue context into the dense HW layout, then write it into the
 * HW registers.
 *
 * Returns: 0 on success, or -EINVAL on an invalid Tx queue index.
 */
int ice_write_txq_ctx(struct ice_hw *hw, struct ice_tlan_ctx *tlan_ctx,
		      u32 txq_index)
{
	ice_txq_ctx_buf_full_t buf = {};

	if (txq_index > QTX_COMM_HEAD_MAX_INDEX)
		return -EINVAL;

	ice_pack_txq_ctx_full(tlan_ctx, &buf);
	ice_copy_txq_ctx_to_hw(hw, &buf, txq_index);

	return 0;
}

/* Sideband Queue command wrappers */

/**
 * ice_sbq_send_cmd - send Sideband Queue command to Sideband Queue
 * @hw: pointer to the HW struct
 * @desc: descriptor describing the command
 * @buf: buffer to use for indirect commands (NULL for direct commands)
 * @buf_size: size of buffer for indirect commands (0 for direct commands)
 * @cd: pointer to command details structure
 */
static int
ice_sbq_send_cmd(struct ice_hw *hw, struct ice_sbq_cmd_desc *desc,
		 void *buf, u16 buf_size, struct ice_sq_cd *cd)
{
	return ice_sq_send_cmd(hw, ice_get_sbq(hw),
			       (struct libie_aq_desc *)desc, buf, buf_size, cd);
}

/**
 * ice_sbq_rw_reg - Fill Sideband Queue command
 * @hw: pointer to the HW struct
 * @in: message info to be filled in descriptor
 * @flags: control queue descriptor flags
 */
int ice_sbq_rw_reg(struct ice_hw *hw, struct ice_sbq_msg_input *in, u16 flags)
{
	struct ice_sbq_cmd_desc desc = {0};
	struct ice_sbq_msg_req msg = {0};
	u16 msg_len;
	int status;

	msg_len = sizeof(msg);

	msg.dest_dev = in->dest_dev;
	msg.opcode = in->opcode;
	msg.flags = ICE_SBQ_MSG_FLAGS;
	msg.sbe_fbe = ICE_SBQ_MSG_SBE_FBE;
	msg.msg_addr_low = cpu_to_le16(in->msg_addr_low);
	msg.msg_addr_high = cpu_to_le32(in->msg_addr_high);

	if (in->opcode)
		msg.data = cpu_to_le32(in->data);
	else
		/* data read comes back in completion, so shorten the struct by
		 * sizeof(msg.data)
		 */
		msg_len -= sizeof(msg.data);

	desc.flags = cpu_to_le16(flags);
	desc.opcode = cpu_to_le16(ice_sbq_opc_neigh_dev_req);
	desc.param0.cmd_len = cpu_to_le16(msg_len);
	status = ice_sbq_send_cmd(hw, &desc, &msg, msg_len, NULL);
	if (!status && !in->opcode)
		in->data = le32_to_cpu
			(((struct ice_sbq_msg_cmpl *)&msg)->data);
	return status;
}

/* FW Admin Queue command wrappers */

/* Software lock/mutex that is meant to be held while the Global Config Lock
 * in firmware is acquired by the software to prevent most (but not all) types
 * of AQ commands from being sent to FW
 */
DEFINE_MUTEX(ice_global_cfg_lock_sw);

/**
 * ice_should_retry_sq_send_cmd
 * @opcode: AQ opcode
 *
 * Decide if we should retry the send command routine for the ATQ, depending
 * on the opcode.
 */
static bool ice_should_retry_sq_send_cmd(u16 opcode)
{
	switch (opcode) {
	case ice_aqc_opc_get_link_topo:
	case ice_aqc_opc_lldp_stop:
	case ice_aqc_opc_lldp_start:
	case ice_aqc_opc_lldp_filter_ctrl:
		return true;
	}

	return false;
}

/**
 * ice_sq_send_cmd_retry - send command to Control Queue (ATQ)
 * @hw: pointer to the HW struct
 * @cq: pointer to the specific Control queue
 * @desc: prefilled descriptor describing the command
 * @buf: buffer to use for indirect commands (or NULL for direct commands)
 * @buf_size: size of buffer for indirect commands (or 0 for direct commands)
 * @cd: pointer to command details structure
 *
 * Retry sending the FW Admin Queue command, multiple times, to the FW Admin
 * Queue if the EBUSY AQ error is returned.
 */
static int
ice_sq_send_cmd_retry(struct ice_hw *hw, struct ice_ctl_q_info *cq,
		      struct libie_aq_desc *desc, void *buf, u16 buf_size,
		      struct ice_sq_cd *cd)
{
	struct libie_aq_desc desc_cpy;
	bool is_cmd_for_retry;
	u8 idx = 0;
	u16 opcode;
	int status;

	opcode = le16_to_cpu(desc->opcode);
	is_cmd_for_retry = ice_should_retry_sq_send_cmd(opcode);
	memset(&desc_cpy, 0, sizeof(desc_cpy));

	if (is_cmd_for_retry) {
		/* All retryable cmds are direct, without buf. */
		WARN_ON(buf);

		memcpy(&desc_cpy, desc, sizeof(desc_cpy));
	}

	do {
		status = ice_sq_send_cmd(hw, cq, desc, buf, buf_size, cd);

		if (!is_cmd_for_retry || !status ||
		    hw->adminq.sq_last_status != LIBIE_AQ_RC_EBUSY)
			break;

		memcpy(desc, &desc_cpy, sizeof(desc_cpy));

		msleep(ICE_SQ_SEND_DELAY_TIME_MS);

	} while (++idx < ICE_SQ_SEND_MAX_EXECUTE);

	return status;
}

/**
 * ice_aq_send_cmd - send FW Admin Queue command to FW Admin Queue
 * @hw: pointer to the HW struct
 * @desc: descriptor describing the command
 * @buf: buffer to use for indirect commands (NULL for direct commands)
 * @buf_size: size of buffer for indirect commands (0 for direct commands)
 * @cd: pointer to command details structure
 *
 * Helper function to send FW Admin Queue commands to the FW Admin Queue.
 */
int
ice_aq_send_cmd(struct ice_hw *hw, struct libie_aq_desc *desc, void *buf,
		u16 buf_size, struct ice_sq_cd *cd)
{
	struct libie_aqc_req_res *cmd = libie_aq_raw(desc);
	bool lock_acquired = false;
	int status;

	/* When a package download is in process (i.e. when the firmware's
	 * Global Configuration Lock resource is held), only the Download
	 * Package, Get Version, Get Package Info List, Upload Section,
	 * Update Package, Set Port Parameters, Get/Set VLAN Mode Parameters,
	 * Add Recipe, Set Recipes to Profile Association, Get Recipe, and Get
	 * Recipes to Profile Association, and Release Resource (with resource
	 * ID set to Global Config Lock) AdminQ commands are allowed; all others
	 * must block until the package download completes and the Global Config
	 * Lock is released.  See also ice_acquire_global_cfg_lock().
	 */
	switch (le16_to_cpu(desc->opcode)) {
	case ice_aqc_opc_download_pkg:
	case ice_aqc_opc_get_pkg_info_list:
	case ice_aqc_opc_get_ver:
	case ice_aqc_opc_upload_section:
	case ice_aqc_opc_update_pkg:
	case ice_aqc_opc_set_port_params:
	case ice_aqc_opc_get_vlan_mode_parameters:
	case ice_aqc_opc_set_vlan_mode_parameters:
	case ice_aqc_opc_set_tx_topo:
	case ice_aqc_opc_get_tx_topo:
	case ice_aqc_opc_add_recipe:
	case ice_aqc_opc_recipe_to_profile:
	case ice_aqc_opc_get_recipe:
	case ice_aqc_opc_get_recipe_to_profile:
		break;
	case ice_aqc_opc_release_res:
		if (le16_to_cpu(cmd->res_id) == LIBIE_AQC_RES_ID_GLBL_LOCK)
			break;
		fallthrough;
	default:
		mutex_lock(&ice_global_cfg_lock_sw);
		lock_acquired = true;
		break;
	}

	status = ice_sq_send_cmd_retry(hw, &hw->adminq, desc, buf, buf_size, cd);
	if (lock_acquired)
		mutex_unlock(&ice_global_cfg_lock_sw);

	return status;
}

/**
 * ice_aq_get_fw_ver
 * @hw: pointer to the HW struct
 * @cd: pointer to command details structure or NULL
 *
 * Get the firmware version (0x0001) from the admin queue commands
 */
int ice_aq_get_fw_ver(struct ice_hw *hw, struct ice_sq_cd *cd)
{
	struct libie_aqc_get_ver *resp;
	struct libie_aq_desc desc;
	int status;

	resp = &desc.params.get_ver;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_get_ver);

	status = ice_aq_send_cmd(hw, &desc, NULL, 0, cd);

	if (!status) {
		hw->fw_branch = resp->fw_branch;
		hw->fw_maj_ver = resp->fw_major;
		hw->fw_min_ver = resp->fw_minor;
		hw->fw_patch = resp->fw_patch;
		hw->fw_build = le32_to_cpu(resp->fw_build);
		hw->api_branch = resp->api_branch;
		hw->api_maj_ver = resp->api_major;
		hw->api_min_ver = resp->api_minor;
		hw->api_patch = resp->api_patch;
	}

	return status;
}

/**
 * ice_aq_send_driver_ver
 * @hw: pointer to the HW struct
 * @dv: driver's major, minor version
 * @cd: pointer to command details structure or NULL
 *
 * Send the driver version (0x0002) to the firmware
 */
int
ice_aq_send_driver_ver(struct ice_hw *hw, struct ice_driver_ver *dv,
		       struct ice_sq_cd *cd)
{
	struct libie_aqc_driver_ver *cmd;
	struct libie_aq_desc desc;
	u16 len;

	cmd = &desc.params.driver_ver;

	if (!dv)
		return -EINVAL;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_driver_ver);

	desc.flags |= cpu_to_le16(LIBIE_AQ_FLAG_RD);
	cmd->major_ver = dv->major_ver;
	cmd->minor_ver = dv->minor_ver;
	cmd->build_ver = dv->build_ver;
	cmd->subbuild_ver = dv->subbuild_ver;

	len = 0;
	while (len < sizeof(dv->driver_string) &&
	       isascii(dv->driver_string[len]) && dv->driver_string[len])
		len++;

	return ice_aq_send_cmd(hw, &desc, dv->driver_string, len, cd);
}

/**
 * ice_aq_q_shutdown
 * @hw: pointer to the HW struct
 * @unloading: is the driver unloading itself
 *
 * Tell the Firmware that we're shutting down the AdminQ and whether
 * or not the driver is unloading as well (0x0003).
 */
int ice_aq_q_shutdown(struct ice_hw *hw, bool unloading)
{
	struct ice_aqc_q_shutdown *cmd;
	struct libie_aq_desc desc;

	cmd = libie_aq_raw(&desc);

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_q_shutdown);

	if (unloading)
		cmd->driver_unloading = ICE_AQC_DRIVER_UNLOADING;

	return ice_aq_send_cmd(hw, &desc, NULL, 0, NULL);
}

/**
 * ice_aq_req_res
 * @hw: pointer to the HW struct
 * @res: resource ID
 * @access: access type
 * @sdp_number: resource number
 * @timeout: the maximum time in ms that the driver may hold the resource
 * @cd: pointer to command details structure or NULL
 *
 * Requests common resource using the admin queue commands (0x0008).
 * When attempting to acquire the Global Config Lock, the driver can
 * learn of three states:
 *  1) 0 -         acquired lock, and can perform download package
 *  2) -EIO -      did not get lock, driver should fail to load
 *  3) -EALREADY - did not get lock, but another driver has
 *                 successfully downloaded the package; the driver does
 *                 not have to download the package and can continue
 *                 loading
 *
 * Note that if the caller is in an acquire lock, perform action, release lock
 * phase of operation, it is possible that the FW may detect a timeout and issue
 * a CORER. In this case, the driver will receive a CORER interrupt and will
 * have to determine its cause. The calling thread that is handling this flow
 * will likely get an error propagated back to it indicating the Download
 * Package, Update Package or the Release Resource AQ commands timed out.
 */
static int
ice_aq_req_res(struct ice_hw *hw, enum ice_aq_res_ids res,
	       enum ice_aq_res_access_type access, u8 sdp_number, u32 *timeout,
	       struct ice_sq_cd *cd)
{
	struct libie_aqc_req_res *cmd_resp;
	struct libie_aq_desc desc;
	int status;

	cmd_resp = &desc.params.res_owner;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_req_res);

	cmd_resp->res_id = cpu_to_le16(res);
	cmd_resp->access_type = cpu_to_le16(access);
	cmd_resp->res_number = cpu_to_le32(sdp_number);
	cmd_resp->timeout = cpu_to_le32(*timeout);
	*timeout = 0;

	status = ice_aq_send_cmd(hw, &desc, NULL, 0, cd);

	/* The completion specifies the maximum time in ms that the driver
	 * may hold the resource in the Timeout field.
	 */

	/* Global config lock response utilizes an additional status field.
	 *
	 * If the Global config lock resource is held by some other driver, the
	 * command completes with LIBIE_AQ_RES_GLBL_IN_PROG in the status field
	 * and the timeout field indicates the maximum time the current owner
	 * of the resource has to free it.
	 */
	if (res == ICE_GLOBAL_CFG_LOCK_RES_ID) {
		if (le16_to_cpu(cmd_resp->status) == LIBIE_AQ_RES_GLBL_SUCCESS) {
			*timeout = le32_to_cpu(cmd_resp->timeout);
			return 0;
		} else if (le16_to_cpu(cmd_resp->status) ==
			   LIBIE_AQ_RES_GLBL_IN_PROG) {
			*timeout = le32_to_cpu(cmd_resp->timeout);
			return -EIO;
		} else if (le16_to_cpu(cmd_resp->status) ==
			   LIBIE_AQ_RES_GLBL_DONE) {
			return -EALREADY;
		}

		/* invalid FW response, force a timeout immediately */
		*timeout = 0;
		return -EIO;
	}

	/* If the resource is held by some other driver, the command completes
	 * with a busy return value and the timeout field indicates the maximum
	 * time the current owner of the resource has to free it.
	 */
	if (!status || hw->adminq.sq_last_status == LIBIE_AQ_RC_EBUSY)
		*timeout = le32_to_cpu(cmd_resp->timeout);

	return status;
}

/**
 * ice_aq_release_res
 * @hw: pointer to the HW struct
 * @res: resource ID
 * @sdp_number: resource number
 * @cd: pointer to command details structure or NULL
 *
 * release common resource using the admin queue commands (0x0009)
 */
static int
ice_aq_release_res(struct ice_hw *hw, enum ice_aq_res_ids res, u8 sdp_number,
		   struct ice_sq_cd *cd)
{
	struct libie_aqc_req_res *cmd;
	struct libie_aq_desc desc;

	cmd = &desc.params.res_owner;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_release_res);

	cmd->res_id = cpu_to_le16(res);
	cmd->res_number = cpu_to_le32(sdp_number);

	return ice_aq_send_cmd(hw, &desc, NULL, 0, cd);
}

/**
 * ice_acquire_res
 * @hw: pointer to the HW structure
 * @res: resource ID
 * @access: access type (read or write)
 * @timeout: timeout in milliseconds
 *
 * This function will attempt to acquire the ownership of a resource.
 */
int
ice_acquire_res(struct ice_hw *hw, enum ice_aq_res_ids res,
		enum ice_aq_res_access_type access, u32 timeout)
{
#define ICE_RES_POLLING_DELAY_MS	10
	u32 delay = ICE_RES_POLLING_DELAY_MS;
	u32 time_left = timeout;
	int status;

	status = ice_aq_req_res(hw, res, access, 0, &time_left, NULL);

	/* A return code of -EALREADY means that another driver has
	 * previously acquired the resource and performed any necessary updates;
	 * in this case the caller does not obtain the resource and has no
	 * further work to do.
	 */
	if (status == -EALREADY)
		goto ice_acquire_res_exit;

	if (status)
		ice_debug(hw, ICE_DBG_RES, "resource %d acquire type %d failed.\n", res, access);

	/* If necessary, poll until the current lock owner timeouts */
	timeout = time_left;
	while (status && timeout && time_left) {
		mdelay(delay);
		timeout = (timeout > delay) ? timeout - delay : 0;
		status = ice_aq_req_res(hw, res, access, 0, &time_left, NULL);

		if (status == -EALREADY)
			/* lock free, but no work to do */
			break;

		if (!status)
			/* lock acquired */
			break;
	}
	if (status && status != -EALREADY)
		ice_debug(hw, ICE_DBG_RES, "resource acquire timed out.\n");

ice_acquire_res_exit:
	if (status == -EALREADY) {
		if (access == ICE_RES_WRITE)
			ice_debug(hw, ICE_DBG_RES, "resource indicates no work to do.\n");
		else
			ice_debug(hw, ICE_DBG_RES, "Warning: -EALREADY not expected\n");
	}
	return status;
}

/**
 * ice_release_res
 * @hw: pointer to the HW structure
 * @res: resource ID
 *
 * This function will release a resource using the proper Admin Command.
 */
void ice_release_res(struct ice_hw *hw, enum ice_aq_res_ids res)
{
	unsigned long timeout;
	int status;

	/* there are some rare cases when trying to release the resource
	 * results in an admin queue timeout, so handle them correctly
	 */
	timeout = jiffies + 10 * ICE_CTL_Q_SQ_CMD_TIMEOUT;
	do {
		status = ice_aq_release_res(hw, res, 0, NULL);
		if (status != -EIO)
			break;
		usleep_range(1000, 2000);
	} while (time_before(jiffies, timeout));
}

/**
 * ice_aq_alloc_free_res - command to allocate/free resources
 * @hw: pointer to the HW struct
 * @buf: Indirect buffer to hold data parameters and response
 * @buf_size: size of buffer for indirect commands
 * @opc: pass in the command opcode
 *
 * Helper function to allocate/free resources using the admin queue commands
 */
int ice_aq_alloc_free_res(struct ice_hw *hw,
			  struct ice_aqc_alloc_free_res_elem *buf, u16 buf_size,
			  enum ice_adminq_opc opc)
{
	struct ice_aqc_alloc_free_res_cmd *cmd;
	struct libie_aq_desc desc;

	cmd = libie_aq_raw(&desc);

	if (!buf || buf_size < flex_array_size(buf, elem, 1))
		return -EINVAL;

	ice_fill_dflt_direct_cmd_desc(&desc, opc);

	desc.flags |= cpu_to_le16(LIBIE_AQ_FLAG_RD);

	cmd->num_entries = cpu_to_le16(1);

	return ice_aq_send_cmd(hw, &desc, buf, buf_size, NULL);
}

/**
 * ice_alloc_hw_res - allocate resource
 * @hw: pointer to the HW struct
 * @type: type of resource
 * @num: number of resources to allocate
 * @btm: allocate from bottom
 * @res: pointer to array that will receive the resources
 */
int
ice_alloc_hw_res(struct ice_hw *hw, u16 type, u16 num, bool btm, u16 *res)
{
	struct ice_aqc_alloc_free_res_elem *buf;
	u16 buf_len;
	int status;

	buf_len = struct_size(buf, elem, num);
	buf = kzalloc(buf_len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	/* Prepare buffer to allocate resource. */
	buf->num_elems = cpu_to_le16(num);
	buf->res_type = cpu_to_le16(type | ICE_AQC_RES_TYPE_FLAG_DEDICATED |
				    ICE_AQC_RES_TYPE_FLAG_IGNORE_INDEX);
	if (btm)
		buf->res_type |= cpu_to_le16(ICE_AQC_RES_TYPE_FLAG_SCAN_BOTTOM);

	status = ice_aq_alloc_free_res(hw, buf, buf_len, ice_aqc_opc_alloc_res);
	if (status)
		goto ice_alloc_res_exit;

	memcpy(res, buf->elem, sizeof(*buf->elem) * num);

ice_alloc_res_exit:
	kfree(buf);
	return status;
}

/**
 * ice_free_hw_res - free allocated HW resource
 * @hw: pointer to the HW struct
 * @type: type of resource to free
 * @num: number of resources
 * @res: pointer to array that contains the resources to free
 */
int ice_free_hw_res(struct ice_hw *hw, u16 type, u16 num, u16 *res)
{
	struct ice_aqc_alloc_free_res_elem *buf;
	u16 buf_len;
	int status;

	buf_len = struct_size(buf, elem, num);
	buf = kzalloc(buf_len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	/* Prepare buffer to free resource. */
	buf->num_elems = cpu_to_le16(num);
	buf->res_type = cpu_to_le16(type);
	memcpy(buf->elem, res, sizeof(*buf->elem) * num);

	status = ice_aq_alloc_free_res(hw, buf, buf_len, ice_aqc_opc_free_res);
	if (status)
		ice_debug(hw, ICE_DBG_SW, "CQ CMD Buffer:\n");

	kfree(buf);
	return status;
}

/**
 * ice_get_num_per_func - determine number of resources per PF
 * @hw: pointer to the HW structure
 * @max: value to be evenly split between each PF
 *
 * Determine the number of valid functions by going through the bitmap returned
 * from parsing capabilities and use this to calculate the number of resources
 * per PF based on the max value passed in.
 */
static u32 ice_get_num_per_func(struct ice_hw *hw, u32 max)
{
	u8 funcs;

#define ICE_CAPS_VALID_FUNCS_M	0xFF
	funcs = hweight8(hw->dev_caps.common_cap.valid_functions &
			 ICE_CAPS_VALID_FUNCS_M);

	if (!funcs)
		return 0;

	return max / funcs;
}

/**
 * ice_parse_common_caps - parse common device/function capabilities
 * @hw: pointer to the HW struct
 * @caps: pointer to common capabilities structure
 * @elem: the capability element to parse
 * @prefix: message prefix for tracing capabilities
 *
 * Given a capability element, extract relevant details into the common
 * capability structure.
 *
 * Returns: true if the capability matches one of the common capability ids,
 * false otherwise.
 */
static bool
ice_parse_common_caps(struct ice_hw *hw, struct ice_hw_common_caps *caps,
		      struct libie_aqc_list_caps_elem *elem, const char *prefix)
{
	u32 logical_id = le32_to_cpu(elem->logical_id);
	u32 phys_id = le32_to_cpu(elem->phys_id);
	u32 number = le32_to_cpu(elem->number);
	u16 cap = le16_to_cpu(elem->cap);
	bool found = true;

	switch (cap) {
	case LIBIE_AQC_CAPS_VALID_FUNCTIONS:
		caps->valid_functions = number;
		ice_debug(hw, ICE_DBG_INIT, "%s: valid_functions (bitmap) = %d\n", prefix,
			  caps->valid_functions);
		break;
	case LIBIE_AQC_CAPS_SRIOV:
		caps->sr_iov_1_1 = (number == 1);
		ice_debug(hw, ICE_DBG_INIT, "%s: sr_iov_1_1 = %d\n", prefix,
			  caps->sr_iov_1_1);
		break;
	case LIBIE_AQC_CAPS_DCB:
		caps->dcb = (number == 1);
		caps->active_tc_bitmap = logical_id;
		caps->maxtc = phys_id;
		ice_debug(hw, ICE_DBG_INIT, "%s: dcb = %d\n", prefix, caps->dcb);
		ice_debug(hw, ICE_DBG_INIT, "%s: active_tc_bitmap = %d\n", prefix,
			  caps->active_tc_bitmap);
		ice_debug(hw, ICE_DBG_INIT, "%s: maxtc = %d\n", prefix, caps->maxtc);
		break;
	case LIBIE_AQC_CAPS_RSS:
		caps->rss_table_size = number;
		caps->rss_table_entry_width = logical_id;
		ice_debug(hw, ICE_DBG_INIT, "%s: rss_table_size = %d\n", prefix,
			  caps->rss_table_size);
		ice_debug(hw, ICE_DBG_INIT, "%s: rss_table_entry_width = %d\n", prefix,
			  caps->rss_table_entry_width);
		break;
	case LIBIE_AQC_CAPS_RXQS:
		caps->num_rxq = number;
		caps->rxq_first_id = phys_id;
		ice_debug(hw, ICE_DBG_INIT, "%s: num_rxq = %d\n", prefix,
			  caps->num_rxq);
		ice_debug(hw, ICE_DBG_INIT, "%s: rxq_first_id = %d\n", prefix,
			  caps->rxq_first_id);
		break;
	case LIBIE_AQC_CAPS_TXQS:
		caps->num_txq = number;
		caps->txq_first_id = phys_id;
		ice_debug(hw, ICE_DBG_INIT, "%s: num_txq = %d\n", prefix,
			  caps->num_txq);
		ice_debug(hw, ICE_DBG_INIT, "%s: txq_first_id = %d\n", prefix,
			  caps->txq_first_id);
		break;
	case LIBIE_AQC_CAPS_MSIX:
		caps->num_msix_vectors = number;
		caps->msix_vector_first_id = phys_id;
		ice_debug(hw, ICE_DBG_INIT, "%s: num_msix_vectors = %d\n", prefix,
			  caps->num_msix_vectors);
		ice_debug(hw, ICE_DBG_INIT, "%s: msix_vector_first_id = %d\n", prefix,
			  caps->msix_vector_first_id);
		break;
	case LIBIE_AQC_CAPS_PENDING_NVM_VER:
		caps->nvm_update_pending_nvm = true;
		ice_debug(hw, ICE_DBG_INIT, "%s: update_pending_nvm\n", prefix);
		break;
	case LIBIE_AQC_CAPS_PENDING_OROM_VER:
		caps->nvm_update_pending_orom = true;
		ice_debug(hw, ICE_DBG_INIT, "%s: update_pending_orom\n", prefix);
		break;
	case LIBIE_AQC_CAPS_PENDING_NET_VER:
		caps->nvm_update_pending_netlist = true;
		ice_debug(hw, ICE_DBG_INIT, "%s: update_pending_netlist\n", prefix);
		break;
	case LIBIE_AQC_CAPS_NVM_MGMT:
		caps->nvm_unified_update =
			(number & ICE_NVM_MGMT_UNIFIED_UPD_SUPPORT) ?
			true : false;
		ice_debug(hw, ICE_DBG_INIT, "%s: nvm_unified_update = %d\n", prefix,
			  caps->nvm_unified_update);
		break;
	case LIBIE_AQC_CAPS_RDMA:
		if (IS_ENABLED(CONFIG_INFINIBAND_IRDMA))
			caps->rdma = (number == 1);
		ice_debug(hw, ICE_DBG_INIT, "%s: rdma = %d\n", prefix, caps->rdma);
		break;
	case LIBIE_AQC_CAPS_MAX_MTU:
		caps->max_mtu = number;
		ice_debug(hw, ICE_DBG_INIT, "%s: max_mtu = %d\n",
			  prefix, caps->max_mtu);
		break;
	case LIBIE_AQC_CAPS_PCIE_RESET_AVOIDANCE:
		caps->pcie_reset_avoidance = (number > 0);
		ice_debug(hw, ICE_DBG_INIT,
			  "%s: pcie_reset_avoidance = %d\n", prefix,
			  caps->pcie_reset_avoidance);
		break;
	case LIBIE_AQC_CAPS_POST_UPDATE_RESET_RESTRICT:
		caps->reset_restrict_support = (number == 1);
		ice_debug(hw, ICE_DBG_INIT,
			  "%s: reset_restrict_support = %d\n", prefix,
			  caps->reset_restrict_support);
		break;
	case LIBIE_AQC_CAPS_FW_LAG_SUPPORT:
		caps->roce_lag = !!(number & LIBIE_AQC_BIT_ROCEV2_LAG);
		ice_debug(hw, ICE_DBG_INIT, "%s: roce_lag = %u\n",
			  prefix, caps->roce_lag);
		caps->sriov_lag = !!(number & LIBIE_AQC_BIT_SRIOV_LAG);
		ice_debug(hw, ICE_DBG_INIT, "%s: sriov_lag = %u\n",
			  prefix, caps->sriov_lag);
		break;
	case LIBIE_AQC_CAPS_TX_SCHED_TOPO_COMP_MODE:
		caps->tx_sched_topo_comp_mode_en = (number == 1);
		break;
	default:
		/* Not one of the recognized common capabilities */
		found = false;
	}

	return found;
}

/**
 * ice_recalc_port_limited_caps - Recalculate port limited capabilities
 * @hw: pointer to the HW structure
 * @caps: pointer to capabilities structure to fix
 *
 * Re-calculate the capabilities that are dependent on the number of physical
 * ports; i.e. some features are not supported or function differently on
 * devices with more than 4 ports.
 */
static void
ice_recalc_port_limited_caps(struct ice_hw *hw, struct ice_hw_common_caps *caps)
{
	/* This assumes device capabilities are always scanned before function
	 * capabilities during the initialization flow.
	 */
	if (hw->dev_caps.num_funcs > 4) {
		/* Max 4 TCs per port */
		caps->maxtc = 4;
		ice_debug(hw, ICE_DBG_INIT, "reducing maxtc to %d (based on #ports)\n",
			  caps->maxtc);
		if (caps->rdma) {
			ice_debug(hw, ICE_DBG_INIT, "forcing RDMA off\n");
			caps->rdma = 0;
		}

		/* print message only when processing device capabilities
		 * during initialization.
		 */
		if (caps == &hw->dev_caps.common_cap)
			dev_info(ice_hw_to_dev(hw), "RDMA functionality is not available with the current device configuration.\n");
	}
}

/**
 * ice_parse_vf_func_caps - Parse ICE_AQC_CAPS_VF function caps
 * @hw: pointer to the HW struct
 * @func_p: pointer to function capabilities structure
 * @cap: pointer to the capability element to parse
 *
 * Extract function capabilities for ICE_AQC_CAPS_VF.
 */
static void
ice_parse_vf_func_caps(struct ice_hw *hw, struct ice_hw_func_caps *func_p,
		       struct libie_aqc_list_caps_elem *cap)
{
	u32 logical_id = le32_to_cpu(cap->logical_id);
	u32 number = le32_to_cpu(cap->number);

	func_p->num_allocd_vfs = number;
	func_p->vf_base_id = logical_id;
	ice_debug(hw, ICE_DBG_INIT, "func caps: num_allocd_vfs = %d\n",
		  func_p->num_allocd_vfs);
	ice_debug(hw, ICE_DBG_INIT, "func caps: vf_base_id = %d\n",
		  func_p->vf_base_id);
}

/**
 * ice_parse_vsi_func_caps - Parse ICE_AQC_CAPS_VSI function caps
 * @hw: pointer to the HW struct
 * @func_p: pointer to function capabilities structure
 * @cap: pointer to the capability element to parse
 *
 * Extract function capabilities for ICE_AQC_CAPS_VSI.
 */
static void
ice_parse_vsi_func_caps(struct ice_hw *hw, struct ice_hw_func_caps *func_p,
			struct libie_aqc_list_caps_elem *cap)
{
	func_p->guar_num_vsi = ice_get_num_per_func(hw, ICE_MAX_VSI);
	ice_debug(hw, ICE_DBG_INIT, "func caps: guar_num_vsi (fw) = %d\n",
		  le32_to_cpu(cap->number));
	ice_debug(hw, ICE_DBG_INIT, "func caps: guar_num_vsi = %d\n",
		  func_p->guar_num_vsi);
}

/**
 * ice_parse_1588_func_caps - Parse ICE_AQC_CAPS_1588 function caps
 * @hw: pointer to the HW struct
 * @func_p: pointer to function capabilities structure
 * @cap: pointer to the capability element to parse
 *
 * Extract function capabilities for ICE_AQC_CAPS_1588.
 */
static void
ice_parse_1588_func_caps(struct ice_hw *hw, struct ice_hw_func_caps *func_p,
			 struct libie_aqc_list_caps_elem *cap)
{
	struct ice_ts_func_info *info = &func_p->ts_func_info;
	u32 number = le32_to_cpu(cap->number);

	info->ena = ((number & ICE_TS_FUNC_ENA_M) != 0);
	func_p->common_cap.ieee_1588 = info->ena;

	info->src_tmr_owned = ((number & ICE_TS_SRC_TMR_OWND_M) != 0);
	info->tmr_ena = ((number & ICE_TS_TMR_ENA_M) != 0);
	info->tmr_index_owned = ((number & ICE_TS_TMR_IDX_OWND_M) != 0);
	info->tmr_index_assoc = ((number & ICE_TS_TMR_IDX_ASSOC_M) != 0);

	if (hw->mac_type != ICE_MAC_GENERIC_3K_E825) {
		info->clk_freq = FIELD_GET(ICE_TS_CLK_FREQ_M, number);
		info->clk_src = ((number & ICE_TS_CLK_SRC_M) != 0);
	} else {
		info->clk_freq = ICE_TSPLL_FREQ_156_250;
		info->clk_src = ICE_CLK_SRC_TIME_REF;
	}

	if (info->clk_freq < NUM_ICE_TSPLL_FREQ) {
		info->time_ref = (enum ice_tspll_freq)info->clk_freq;
	} else {
		/* Unknown clock frequency, so assume a (probably incorrect)
		 * default to avoid out-of-bounds look ups of frequency
		 * related information.
		 */
		ice_debug(hw, ICE_DBG_INIT, "1588 func caps: unknown clock frequency %u\n",
			  info->clk_freq);
		info->time_ref = ICE_TSPLL_FREQ_25_000;
	}

	ice_debug(hw, ICE_DBG_INIT, "func caps: ieee_1588 = %u\n",
		  func_p->common_cap.ieee_1588);
	ice_debug(hw, ICE_DBG_INIT, "func caps: src_tmr_owned = %u\n",
		  info->src_tmr_owned);
	ice_debug(hw, ICE_DBG_INIT, "func caps: tmr_ena = %u\n",
		  info->tmr_ena);
	ice_debug(hw, ICE_DBG_INIT, "func caps: tmr_index_owned = %u\n",
		  info->tmr_index_owned);
	ice_debug(hw, ICE_DBG_INIT, "func caps: tmr_index_assoc = %u\n",
		  info->tmr_index_assoc);
	ice_debug(hw, ICE_DBG_INIT, "func caps: clk_freq = %u\n",
		  info->clk_freq);
	ice_debug(hw, ICE_DBG_INIT, "func caps: clk_src = %u\n",
		  info->clk_src);
}

/**
 * ice_parse_fdir_func_caps - Parse ICE_AQC_CAPS_FD function caps
 * @hw: pointer to the HW struct
 * @func_p: pointer to function capabilities structure
 *
 * Extract function capabilities for ICE_AQC_CAPS_FD.
 */
static void
ice_parse_fdir_func_caps(struct ice_hw *hw, struct ice_hw_func_caps *func_p)
{
	u32 reg_val, gsize, bsize;

	reg_val = rd32(hw, GLQF_FD_SIZE);
	switch (hw->mac_type) {
	case ICE_MAC_E830:
		gsize = FIELD_GET(E830_GLQF_FD_SIZE_FD_GSIZE_M, reg_val);
		bsize = FIELD_GET(E830_GLQF_FD_SIZE_FD_BSIZE_M, reg_val);
		break;
	case ICE_MAC_E810:
	default:
		gsize = FIELD_GET(E800_GLQF_FD_SIZE_FD_GSIZE_M, reg_val);
		bsize = FIELD_GET(E800_GLQF_FD_SIZE_FD_BSIZE_M, reg_val);
	}
	func_p->fd_fltr_guar = ice_get_num_per_func(hw, gsize);
	func_p->fd_fltr_best_effort = bsize;

	ice_debug(hw, ICE_DBG_INIT, "func caps: fd_fltr_guar = %d\n",
		  func_p->fd_fltr_guar);
	ice_debug(hw, ICE_DBG_INIT, "func caps: fd_fltr_best_effort = %d\n",
		  func_p->fd_fltr_best_effort);
}

/**
 * ice_parse_func_caps - Parse function capabilities
 * @hw: pointer to the HW struct
 * @func_p: pointer to function capabilities structure
 * @buf: buffer containing the function capability records
 * @cap_count: the number of capabilities
 *
 * Helper function to parse function (0x000A) capabilities list. For
 * capabilities shared between device and function, this relies on
 * ice_parse_common_caps.
 *
 * Loop through the list of provided capabilities and extract the relevant
 * data into the function capabilities structured.
 */
static void
ice_parse_func_caps(struct ice_hw *hw, struct ice_hw_func_caps *func_p,
		    void *buf, u32 cap_count)
{
	struct libie_aqc_list_caps_elem *cap_resp;
	u32 i;

	cap_resp = buf;

	memset(func_p, 0, sizeof(*func_p));

	for (i = 0; i < cap_count; i++) {
		u16 cap = le16_to_cpu(cap_resp[i].cap);
		bool found;

		found = ice_parse_common_caps(hw, &func_p->common_cap,
					      &cap_resp[i], "func caps");

		switch (cap) {
		case LIBIE_AQC_CAPS_VF:
			ice_parse_vf_func_caps(hw, func_p, &cap_resp[i]);
			break;
		case LIBIE_AQC_CAPS_VSI:
			ice_parse_vsi_func_caps(hw, func_p, &cap_resp[i]);
			break;
		case LIBIE_AQC_CAPS_1588:
			ice_parse_1588_func_caps(hw, func_p, &cap_resp[i]);
			break;
		case LIBIE_AQC_CAPS_FD:
			ice_parse_fdir_func_caps(hw, func_p);
			break;
		default:
			/* Don't list common capabilities as unknown */
			if (!found)
				ice_debug(hw, ICE_DBG_INIT, "func caps: unknown capability[%d]: 0x%x\n",
					  i, cap);
			break;
		}
	}

	ice_recalc_port_limited_caps(hw, &func_p->common_cap);
}

/**
 * ice_func_id_to_logical_id - map from function id to logical pf id
 * @active_function_bitmap: active function bitmap
 * @pf_id: function number of device
 *
 * Return: logical PF ID.
 */
static int ice_func_id_to_logical_id(u32 active_function_bitmap, u8 pf_id)
{
	u8 logical_id = 0;
	u8 i;

	for (i = 0; i < pf_id; i++)
		if (active_function_bitmap & BIT(i))
			logical_id++;

	return logical_id;
}

/**
 * ice_parse_valid_functions_cap - Parse ICE_AQC_CAPS_VALID_FUNCTIONS caps
 * @hw: pointer to the HW struct
 * @dev_p: pointer to device capabilities structure
 * @cap: capability element to parse
 *
 * Parse ICE_AQC_CAPS_VALID_FUNCTIONS for device capabilities.
 */
static void
ice_parse_valid_functions_cap(struct ice_hw *hw, struct ice_hw_dev_caps *dev_p,
			      struct libie_aqc_list_caps_elem *cap)
{
	u32 number = le32_to_cpu(cap->number);

	dev_p->num_funcs = hweight32(number);
	ice_debug(hw, ICE_DBG_INIT, "dev caps: num_funcs = %d\n",
		  dev_p->num_funcs);

	hw->logical_pf_id = ice_func_id_to_logical_id(number, hw->pf_id);
}

/**
 * ice_parse_vf_dev_caps - Parse ICE_AQC_CAPS_VF device caps
 * @hw: pointer to the HW struct
 * @dev_p: pointer to device capabilities structure
 * @cap: capability element to parse
 *
 * Parse ICE_AQC_CAPS_VF for device capabilities.
 */
static void
ice_parse_vf_dev_caps(struct ice_hw *hw, struct ice_hw_dev_caps *dev_p,
		      struct libie_aqc_list_caps_elem *cap)
{
	u32 number = le32_to_cpu(cap->number);

	dev_p->num_vfs_exposed = number;
	ice_debug(hw, ICE_DBG_INIT, "dev_caps: num_vfs_exposed = %d\n",
		  dev_p->num_vfs_exposed);
}

/**
 * ice_parse_vsi_dev_caps - Parse ICE_AQC_CAPS_VSI device caps
 * @hw: pointer to the HW struct
 * @dev_p: pointer to device capabilities structure
 * @cap: capability element to parse
 *
 * Parse ICE_AQC_CAPS_VSI for device capabilities.
 */
static void
ice_parse_vsi_dev_caps(struct ice_hw *hw, struct ice_hw_dev_caps *dev_p,
		       struct libie_aqc_list_caps_elem *cap)
{
	u32 number = le32_to_cpu(cap->number);

	dev_p->num_vsi_allocd_to_host = number;
	ice_debug(hw, ICE_DBG_INIT, "dev caps: num_vsi_allocd_to_host = %d\n",
		  dev_p->num_vsi_allocd_to_host);
}

/**
 * ice_parse_1588_dev_caps - Parse ICE_AQC_CAPS_1588 device caps
 * @hw: pointer to the HW struct
 * @dev_p: pointer to device capabilities structure
 * @cap: capability element to parse
 *
 * Parse ICE_AQC_CAPS_1588 for device capabilities.
 */
static void
ice_parse_1588_dev_caps(struct ice_hw *hw, struct ice_hw_dev_caps *dev_p,
			struct libie_aqc_list_caps_elem *cap)
{
	struct ice_ts_dev_info *info = &dev_p->ts_dev_info;
	u32 logical_id = le32_to_cpu(cap->logical_id);
	u32 phys_id = le32_to_cpu(cap->phys_id);
	u32 number = le32_to_cpu(cap->number);

	info->ena = ((number & ICE_TS_DEV_ENA_M) != 0);
	dev_p->common_cap.ieee_1588 = info->ena;

	info->tmr0_owner = number & ICE_TS_TMR0_OWNR_M;
	info->tmr0_owned = ((number & ICE_TS_TMR0_OWND_M) != 0);
	info->tmr0_ena = ((number & ICE_TS_TMR0_ENA_M) != 0);

	info->tmr1_owner = FIELD_GET(ICE_TS_TMR1_OWNR_M, number);
	info->tmr1_owned = ((number & ICE_TS_TMR1_OWND_M) != 0);
	info->tmr1_ena = ((number & ICE_TS_TMR1_ENA_M) != 0);

	info->ts_ll_read = ((number & ICE_TS_LL_TX_TS_READ_M) != 0);
	info->ts_ll_int_read = ((number & ICE_TS_LL_TX_TS_INT_READ_M) != 0);
	info->ll_phy_tmr_update = ((number & ICE_TS_LL_PHY_TMR_UPDATE_M) != 0);

	info->ena_ports = logical_id;
	info->tmr_own_map = phys_id;

	ice_debug(hw, ICE_DBG_INIT, "dev caps: ieee_1588 = %u\n",
		  dev_p->common_cap.ieee_1588);
	ice_debug(hw, ICE_DBG_INIT, "dev caps: tmr0_owner = %u\n",
		  info->tmr0_owner);
	ice_debug(hw, ICE_DBG_INIT, "dev caps: tmr0_owned = %u\n",
		  info->tmr0_owned);
	ice_debug(hw, ICE_DBG_INIT, "dev caps: tmr0_ena = %u\n",
		  info->tmr0_ena);
	ice_debug(hw, ICE_DBG_INIT, "dev caps: tmr1_owner = %u\n",
		  info->tmr1_owner);
	ice_debug(hw, ICE_DBG_INIT, "dev caps: tmr1_owned = %u\n",
		  info->tmr1_owned);
	ice_debug(hw, ICE_DBG_INIT, "dev caps: tmr1_ena = %u\n",
		  info->tmr1_ena);
	ice_debug(hw, ICE_DBG_INIT, "dev caps: ts_ll_read = %u\n",
		  info->ts_ll_read);
	ice_debug(hw, ICE_DBG_INIT, "dev caps: ts_ll_int_read = %u\n",
		  info->ts_ll_int_read);
	ice_debug(hw, ICE_DBG_INIT, "dev caps: ll_phy_tmr_update = %u\n",
		  info->ll_phy_tmr_update);
	ice_debug(hw, ICE_DBG_INIT, "dev caps: ieee_1588 ena_ports = %u\n",
		  info->ena_ports);
	ice_debug(hw, ICE_DBG_INIT, "dev caps: tmr_own_map = %u\n",
		  info->tmr_own_map);
}

/**
 * ice_parse_fdir_dev_caps - Parse ICE_AQC_CAPS_FD device caps
 * @hw: pointer to the HW struct
 * @dev_p: pointer to device capabilities structure
 * @cap: capability element to parse
 *
 * Parse ICE_AQC_CAPS_FD for device capabilities.
 */
static void
ice_parse_fdir_dev_caps(struct ice_hw *hw, struct ice_hw_dev_caps *dev_p,
			struct libie_aqc_list_caps_elem *cap)
{
	u32 number = le32_to_cpu(cap->number);

	dev_p->num_flow_director_fltr = number;
	ice_debug(hw, ICE_DBG_INIT, "dev caps: num_flow_director_fltr = %d\n",
		  dev_p->num_flow_director_fltr);
}

/**
 * ice_parse_sensor_reading_cap - Parse ICE_AQC_CAPS_SENSOR_READING cap
 * @hw: pointer to the HW struct
 * @dev_p: pointer to device capabilities structure
 * @cap: capability element to parse
 *
 * Parse ICE_AQC_CAPS_SENSOR_READING for device capability for reading
 * enabled sensors.
 */
static void
ice_parse_sensor_reading_cap(struct ice_hw *hw, struct ice_hw_dev_caps *dev_p,
			     struct libie_aqc_list_caps_elem *cap)
{
	dev_p->supported_sensors = le32_to_cpu(cap->number);

	ice_debug(hw, ICE_DBG_INIT,
		  "dev caps: supported sensors (bitmap) = 0x%x\n",
		  dev_p->supported_sensors);
}

/**
 * ice_parse_nac_topo_dev_caps - Parse ICE_AQC_CAPS_NAC_TOPOLOGY cap
 * @hw: pointer to the HW struct
 * @dev_p: pointer to device capabilities structure
 * @cap: capability element to parse
 *
 * Parse ICE_AQC_CAPS_NAC_TOPOLOGY for device capabilities.
 */
static void ice_parse_nac_topo_dev_caps(struct ice_hw *hw,
					struct ice_hw_dev_caps *dev_p,
					struct libie_aqc_list_caps_elem *cap)
{
	dev_p->nac_topo.mode = le32_to_cpu(cap->number);
	dev_p->nac_topo.id = le32_to_cpu(cap->phys_id) & ICE_NAC_TOPO_ID_M;

	dev_info(ice_hw_to_dev(hw),
		 "PF is configured in %s mode with IP instance ID %d\n",
		 (dev_p->nac_topo.mode & ICE_NAC_TOPO_PRIMARY_M) ?
		 "primary" : "secondary", dev_p->nac_topo.id);

	ice_debug(hw, ICE_DBG_INIT, "dev caps: nac topology is_primary = %d\n",
		  !!(dev_p->nac_topo.mode & ICE_NAC_TOPO_PRIMARY_M));
	ice_debug(hw, ICE_DBG_INIT, "dev caps: nac topology is_dual = %d\n",
		  !!(dev_p->nac_topo.mode & ICE_NAC_TOPO_DUAL_M));
	ice_debug(hw, ICE_DBG_INIT, "dev caps: nac topology id = %d\n",
		  dev_p->nac_topo.id);
}

/**
 * ice_parse_dev_caps - Parse device capabilities
 * @hw: pointer to the HW struct
 * @dev_p: pointer to device capabilities structure
 * @buf: buffer containing the device capability records
 * @cap_count: the number of capabilities
 *
 * Helper device to parse device (0x000B) capabilities list. For
 * capabilities shared between device and function, this relies on
 * ice_parse_common_caps.
 *
 * Loop through the list of provided capabilities and extract the relevant
 * data into the device capabilities structured.
 */
static void
ice_parse_dev_caps(struct ice_hw *hw, struct ice_hw_dev_caps *dev_p,
		   void *buf, u32 cap_count)
{
	struct libie_aqc_list_caps_elem *cap_resp;
	u32 i;

	cap_resp = buf;

	memset(dev_p, 0, sizeof(*dev_p));

	for (i = 0; i < cap_count; i++) {
		u16 cap = le16_to_cpu(cap_resp[i].cap);
		bool found;

		found = ice_parse_common_caps(hw, &dev_p->common_cap,
					      &cap_resp[i], "dev caps");

		switch (cap) {
		case LIBIE_AQC_CAPS_VALID_FUNCTIONS:
			ice_parse_valid_functions_cap(hw, dev_p, &cap_resp[i]);
			break;
		case LIBIE_AQC_CAPS_VF:
			ice_parse_vf_dev_caps(hw, dev_p, &cap_resp[i]);
			break;
		case LIBIE_AQC_CAPS_VSI:
			ice_parse_vsi_dev_caps(hw, dev_p, &cap_resp[i]);
			break;
		case LIBIE_AQC_CAPS_1588:
			ice_parse_1588_dev_caps(hw, dev_p, &cap_resp[i]);
			break;
		case LIBIE_AQC_CAPS_FD:
			ice_parse_fdir_dev_caps(hw, dev_p, &cap_resp[i]);
			break;
		case LIBIE_AQC_CAPS_SENSOR_READING:
			ice_parse_sensor_reading_cap(hw, dev_p, &cap_resp[i]);
			break;
		case LIBIE_AQC_CAPS_NAC_TOPOLOGY:
			ice_parse_nac_topo_dev_caps(hw, dev_p, &cap_resp[i]);
			break;
		default:
			/* Don't list common capabilities as unknown */
			if (!found)
				ice_debug(hw, ICE_DBG_INIT, "dev caps: unknown capability[%d]: 0x%x\n",
					  i, cap);
			break;
		}
	}

	ice_recalc_port_limited_caps(hw, &dev_p->common_cap);
}

/**
 * ice_is_phy_rclk_in_netlist
 * @hw: pointer to the hw struct
 *
 * Check if the PHY Recovered Clock device is present in the netlist
 */
bool ice_is_phy_rclk_in_netlist(struct ice_hw *hw)
{
	if (ice_find_netlist_node(hw, ICE_AQC_LINK_TOPO_NODE_TYPE_PHY,
				  ICE_AQC_LINK_TOPO_NODE_CTX_PORT,
				  ICE_AQC_GET_LINK_TOPO_NODE_NR_C827, NULL) &&
	    ice_find_netlist_node(hw, ICE_AQC_LINK_TOPO_NODE_TYPE_PHY,
				  ICE_AQC_LINK_TOPO_NODE_CTX_PORT,
				  ICE_AQC_GET_LINK_TOPO_NODE_NR_E822_PHY, NULL))
		return false;

	return true;
}

/**
 * ice_is_clock_mux_in_netlist
 * @hw: pointer to the hw struct
 *
 * Check if the Clock Multiplexer device is present in the netlist
 */
bool ice_is_clock_mux_in_netlist(struct ice_hw *hw)
{
	if (ice_find_netlist_node(hw, ICE_AQC_LINK_TOPO_NODE_TYPE_CLK_MUX,
				  ICE_AQC_LINK_TOPO_NODE_CTX_GLOBAL,
				  ICE_AQC_GET_LINK_TOPO_NODE_NR_GEN_CLK_MUX,
				  NULL))
		return false;

	return true;
}

/**
 * ice_is_cgu_in_netlist - check for CGU presence
 * @hw: pointer to the hw struct
 *
 * Check if the Clock Generation Unit (CGU) device is present in the netlist.
 * Save the CGU part number in the hw structure for later use.
 * Return:
 * * true - cgu is present
 * * false - cgu is not present
 */
bool ice_is_cgu_in_netlist(struct ice_hw *hw)
{
	if (!ice_find_netlist_node(hw, ICE_AQC_LINK_TOPO_NODE_TYPE_CLK_CTRL,
				   ICE_AQC_LINK_TOPO_NODE_CTX_GLOBAL,
				   ICE_AQC_GET_LINK_TOPO_NODE_NR_ZL30632_80032,
				   NULL)) {
		hw->cgu_part_number = ICE_AQC_GET_LINK_TOPO_NODE_NR_ZL30632_80032;
		return true;
	} else if (!ice_find_netlist_node(hw,
					  ICE_AQC_LINK_TOPO_NODE_TYPE_CLK_CTRL,
					  ICE_AQC_LINK_TOPO_NODE_CTX_GLOBAL,
					  ICE_AQC_GET_LINK_TOPO_NODE_NR_SI5383_5384,
					  NULL)) {
		hw->cgu_part_number = ICE_AQC_GET_LINK_TOPO_NODE_NR_SI5383_5384;
		return true;
	}

	return false;
}

/**
 * ice_is_gps_in_netlist
 * @hw: pointer to the hw struct
 *
 * Check if the GPS generic device is present in the netlist
 */
bool ice_is_gps_in_netlist(struct ice_hw *hw)
{
	if (ice_find_netlist_node(hw, ICE_AQC_LINK_TOPO_NODE_TYPE_GPS,
				  ICE_AQC_LINK_TOPO_NODE_CTX_GLOBAL,
				  ICE_AQC_GET_LINK_TOPO_NODE_NR_GEN_GPS, NULL))
		return false;

	return true;
}

/**
 * ice_aq_list_caps - query function/device capabilities
 * @hw: pointer to the HW struct
 * @buf: a buffer to hold the capabilities
 * @buf_size: size of the buffer
 * @cap_count: if not NULL, set to the number of capabilities reported
 * @opc: capabilities type to discover, device or function
 * @cd: pointer to command details structure or NULL
 *
 * Get the function (0x000A) or device (0x000B) capabilities description from
 * firmware and store it in the buffer.
 *
 * If the cap_count pointer is not NULL, then it is set to the number of
 * capabilities firmware will report. Note that if the buffer size is too
 * small, it is possible the command will return ICE_AQ_ERR_ENOMEM. The
 * cap_count will still be updated in this case. It is recommended that the
 * buffer size be set to ICE_AQ_MAX_BUF_LEN (the largest possible buffer that
 * firmware could return) to avoid this.
 */
int
ice_aq_list_caps(struct ice_hw *hw, void *buf, u16 buf_size, u32 *cap_count,
		 enum ice_adminq_opc opc, struct ice_sq_cd *cd)
{
	struct libie_aqc_list_caps *cmd;
	struct libie_aq_desc desc;
	int status;

	cmd = &desc.params.get_cap;

	if (opc != ice_aqc_opc_list_func_caps &&
	    opc != ice_aqc_opc_list_dev_caps)
		return -EINVAL;

	ice_fill_dflt_direct_cmd_desc(&desc, opc);
	status = ice_aq_send_cmd(hw, &desc, buf, buf_size, cd);

	if (cap_count)
		*cap_count = le32_to_cpu(cmd->count);

	return status;
}

/**
 * ice_discover_dev_caps - Read and extract device capabilities
 * @hw: pointer to the hardware structure
 * @dev_caps: pointer to device capabilities structure
 *
 * Read the device capabilities and extract them into the dev_caps structure
 * for later use.
 */
int
ice_discover_dev_caps(struct ice_hw *hw, struct ice_hw_dev_caps *dev_caps)
{
	u32 cap_count = 0;
	void *cbuf;
	int status;

	cbuf = kzalloc(ICE_AQ_MAX_BUF_LEN, GFP_KERNEL);
	if (!cbuf)
		return -ENOMEM;

	/* Although the driver doesn't know the number of capabilities the
	 * device will return, we can simply send a 4KB buffer, the maximum
	 * possible size that firmware can return.
	 */
	cap_count = ICE_AQ_MAX_BUF_LEN / sizeof(struct libie_aqc_list_caps_elem);

	status = ice_aq_list_caps(hw, cbuf, ICE_AQ_MAX_BUF_LEN, &cap_count,
				  ice_aqc_opc_list_dev_caps, NULL);
	if (!status)
		ice_parse_dev_caps(hw, dev_caps, cbuf, cap_count);
	kfree(cbuf);

	return status;
}

/**
 * ice_discover_func_caps - Read and extract function capabilities
 * @hw: pointer to the hardware structure
 * @func_caps: pointer to function capabilities structure
 *
 * Read the function capabilities and extract them into the func_caps structure
 * for later use.
 */
static int
ice_discover_func_caps(struct ice_hw *hw, struct ice_hw_func_caps *func_caps)
{
	u32 cap_count = 0;
	void *cbuf;
	int status;

	cbuf = kzalloc(ICE_AQ_MAX_BUF_LEN, GFP_KERNEL);
	if (!cbuf)
		return -ENOMEM;

	/* Although the driver doesn't know the number of capabilities the
	 * device will return, we can simply send a 4KB buffer, the maximum
	 * possible size that firmware can return.
	 */
	cap_count = ICE_AQ_MAX_BUF_LEN / sizeof(struct libie_aqc_list_caps_elem);

	status = ice_aq_list_caps(hw, cbuf, ICE_AQ_MAX_BUF_LEN, &cap_count,
				  ice_aqc_opc_list_func_caps, NULL);
	if (!status)
		ice_parse_func_caps(hw, func_caps, cbuf, cap_count);
	kfree(cbuf);

	return status;
}

/**
 * ice_set_safe_mode_caps - Override dev/func capabilities when in safe mode
 * @hw: pointer to the hardware structure
 */
void ice_set_safe_mode_caps(struct ice_hw *hw)
{
	struct ice_hw_func_caps *func_caps = &hw->func_caps;
	struct ice_hw_dev_caps *dev_caps = &hw->dev_caps;
	struct ice_hw_common_caps cached_caps;
	u32 num_funcs;

	/* cache some func_caps values that should be restored after memset */
	cached_caps = func_caps->common_cap;

	/* unset func capabilities */
	memset(func_caps, 0, sizeof(*func_caps));

#define ICE_RESTORE_FUNC_CAP(name) \
	func_caps->common_cap.name = cached_caps.name

	/* restore cached values */
	ICE_RESTORE_FUNC_CAP(valid_functions);
	ICE_RESTORE_FUNC_CAP(txq_first_id);
	ICE_RESTORE_FUNC_CAP(rxq_first_id);
	ICE_RESTORE_FUNC_CAP(msix_vector_first_id);
	ICE_RESTORE_FUNC_CAP(max_mtu);
	ICE_RESTORE_FUNC_CAP(nvm_unified_update);
	ICE_RESTORE_FUNC_CAP(nvm_update_pending_nvm);
	ICE_RESTORE_FUNC_CAP(nvm_update_pending_orom);
	ICE_RESTORE_FUNC_CAP(nvm_update_pending_netlist);

	/* one Tx and one Rx queue in safe mode */
	func_caps->common_cap.num_rxq = 1;
	func_caps->common_cap.num_txq = 1;

	/* two MSIX vectors, one for traffic and one for misc causes */
	func_caps->common_cap.num_msix_vectors = 2;
	func_caps->guar_num_vsi = 1;

	/* cache some dev_caps values that should be restored after memset */
	cached_caps = dev_caps->common_cap;
	num_funcs = dev_caps->num_funcs;

	/* unset dev capabilities */
	memset(dev_caps, 0, sizeof(*dev_caps));

#define ICE_RESTORE_DEV_CAP(name) \
	dev_caps->common_cap.name = cached_caps.name

	/* restore cached values */
	ICE_RESTORE_DEV_CAP(valid_functions);
	ICE_RESTORE_DEV_CAP(txq_first_id);
	ICE_RESTORE_DEV_CAP(rxq_first_id);
	ICE_RESTORE_DEV_CAP(msix_vector_first_id);
	ICE_RESTORE_DEV_CAP(max_mtu);
	ICE_RESTORE_DEV_CAP(nvm_unified_update);
	ICE_RESTORE_DEV_CAP(nvm_update_pending_nvm);
	ICE_RESTORE_DEV_CAP(nvm_update_pending_orom);
	ICE_RESTORE_DEV_CAP(nvm_update_pending_netlist);
	dev_caps->num_funcs = num_funcs;

	/* one Tx and one Rx queue per function in safe mode */
	dev_caps->common_cap.num_rxq = num_funcs;
	dev_caps->common_cap.num_txq = num_funcs;

	/* two MSIX vectors per function */
	dev_caps->common_cap.num_msix_vectors = 2 * num_funcs;
}

/**
 * ice_get_caps - get info about the HW
 * @hw: pointer to the hardware structure
 */
int ice_get_caps(struct ice_hw *hw)
{
	int status;

	status = ice_discover_dev_caps(hw, &hw->dev_caps);
	if (status)
		return status;

	return ice_discover_func_caps(hw, &hw->func_caps);
}

/**
 * ice_aq_manage_mac_write - manage MAC address write command
 * @hw: pointer to the HW struct
 * @mac_addr: MAC address to be written as LAA/LAA+WoL/Port address
 * @flags: flags to control write behavior
 * @cd: pointer to command details structure or NULL
 *
 * This function is used to write MAC address to the NVM (0x0108).
 */
int
ice_aq_manage_mac_write(struct ice_hw *hw, const u8 *mac_addr, u8 flags,
			struct ice_sq_cd *cd)
{
	struct ice_aqc_manage_mac_write *cmd;
	struct libie_aq_desc desc;

	cmd = libie_aq_raw(&desc);
	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_manage_mac_write);

	cmd->flags = flags;
	ether_addr_copy(cmd->mac_addr, mac_addr);

	return ice_aq_send_cmd(hw, &desc, NULL, 0, cd);
}

/**
 * ice_aq_clear_pxe_mode
 * @hw: pointer to the HW struct
 *
 * Tell the firmware that the driver is taking over from PXE (0x0110).
 */
static int ice_aq_clear_pxe_mode(struct ice_hw *hw)
{
	struct ice_aqc_clear_pxe *cmd;
	struct libie_aq_desc desc;

	cmd = libie_aq_raw(&desc);
	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_clear_pxe_mode);
	cmd->rx_cnt = ICE_AQC_CLEAR_PXE_RX_CNT;

	return ice_aq_send_cmd(hw, &desc, NULL, 0, NULL);
}

/**
 * ice_clear_pxe_mode - clear pxe operations mode
 * @hw: pointer to the HW struct
 *
 * Make sure all PXE mode settings are cleared, including things
 * like descriptor fetch/write-back mode.
 */
void ice_clear_pxe_mode(struct ice_hw *hw)
{
	if (ice_check_sq_alive(hw, &hw->adminq))
		ice_aq_clear_pxe_mode(hw);
}

/**
 * ice_aq_set_port_params - set physical port parameters.
 * @pi: pointer to the port info struct
 * @double_vlan: if set double VLAN is enabled
 * @cd: pointer to command details structure or NULL
 *
 * Set Physical port parameters (0x0203)
 */
int
ice_aq_set_port_params(struct ice_port_info *pi, bool double_vlan,
		       struct ice_sq_cd *cd)

{
	struct ice_aqc_set_port_params *cmd;
	struct ice_hw *hw = pi->hw;
	struct libie_aq_desc desc;
	u16 cmd_flags = 0;

	cmd = libie_aq_raw(&desc);

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_set_port_params);
	if (double_vlan)
		cmd_flags |= ICE_AQC_SET_P_PARAMS_DOUBLE_VLAN_ENA;
	cmd->cmd_flags = cpu_to_le16(cmd_flags);

	cmd->local_fwd_mode = pi->local_fwd_mode |
				ICE_AQC_SET_P_PARAMS_LOCAL_FWD_MODE_VALID;

	return ice_aq_send_cmd(hw, &desc, NULL, 0, cd);
}

/**
 * ice_is_100m_speed_supported
 * @hw: pointer to the HW struct
 *
 * returns true if 100M speeds are supported by the device,
 * false otherwise.
 */
bool ice_is_100m_speed_supported(struct ice_hw *hw)
{
	switch (hw->device_id) {
	case ICE_DEV_ID_E822C_SGMII:
	case ICE_DEV_ID_E822L_SGMII:
	case ICE_DEV_ID_E823L_1GBE:
	case ICE_DEV_ID_E823C_SGMII:
		return true;
	default:
		return false;
	}
}

/**
 * ice_get_link_speed_based_on_phy_type - returns link speed
 * @phy_type_low: lower part of phy_type
 * @phy_type_high: higher part of phy_type
 *
 * This helper function will convert an entry in PHY type structure
 * [phy_type_low, phy_type_high] to its corresponding link speed.
 * Note: In the structure of [phy_type_low, phy_type_high], there should
 * be one bit set, as this function will convert one PHY type to its
 * speed.
 *
 * Return:
 * * PHY speed for recognized PHY type
 * * If no bit gets set, ICE_AQ_LINK_SPEED_UNKNOWN will be returned
 * * If more than one bit gets set, ICE_AQ_LINK_SPEED_UNKNOWN will be returned
 */
u16 ice_get_link_speed_based_on_phy_type(u64 phy_type_low, u64 phy_type_high)
{
	u16 speed_phy_type_high = ICE_AQ_LINK_SPEED_UNKNOWN;
	u16 speed_phy_type_low = ICE_AQ_LINK_SPEED_UNKNOWN;

	switch (phy_type_low) {
	case ICE_PHY_TYPE_LOW_100BASE_TX:
	case ICE_PHY_TYPE_LOW_100M_SGMII:
		speed_phy_type_low = ICE_AQ_LINK_SPEED_100MB;
		break;
	case ICE_PHY_TYPE_LOW_1000BASE_T:
	case ICE_PHY_TYPE_LOW_1000BASE_SX:
	case ICE_PHY_TYPE_LOW_1000BASE_LX:
	case ICE_PHY_TYPE_LOW_1000BASE_KX:
	case ICE_PHY_TYPE_LOW_1G_SGMII:
		speed_phy_type_low = ICE_AQ_LINK_SPEED_1000MB;
		break;
	case ICE_PHY_TYPE_LOW_2500BASE_T:
	case ICE_PHY_TYPE_LOW_2500BASE_X:
	case ICE_PHY_TYPE_LOW_2500BASE_KX:
		speed_phy_type_low = ICE_AQ_LINK_SPEED_2500MB;
		break;
	case ICE_PHY_TYPE_LOW_5GBASE_T:
	case ICE_PHY_TYPE_LOW_5GBASE_KR:
		speed_phy_type_low = ICE_AQ_LINK_SPEED_5GB;
		break;
	case ICE_PHY_TYPE_LOW_10GBASE_T:
	case ICE_PHY_TYPE_LOW_10G_SFI_DA:
	case ICE_PHY_TYPE_LOW_10GBASE_SR:
	case ICE_PHY_TYPE_LOW_10GBASE_LR:
	case ICE_PHY_TYPE_LOW_10GBASE_KR_CR1:
	case ICE_PHY_TYPE_LOW_10G_SFI_AOC_ACC:
	case ICE_PHY_TYPE_LOW_10G_SFI_C2C:
		speed_phy_type_low = ICE_AQ_LINK_SPEED_10GB;
		break;
	case ICE_PHY_TYPE_LOW_25GBASE_T:
	case ICE_PHY_TYPE_LOW_25GBASE_CR:
	case ICE_PHY_TYPE_LOW_25GBASE_CR_S:
	case ICE_PHY_TYPE_LOW_25GBASE_CR1:
	case ICE_PHY_TYPE_LOW_25GBASE_SR:
	case ICE_PHY_TYPE_LOW_25GBASE_LR:
	case ICE_PHY_TYPE_LOW_25GBASE_KR:
	case ICE_PHY_TYPE_LOW_25GBASE_KR_S:
	case ICE_PHY_TYPE_LOW_25GBASE_KR1:
	case ICE_PHY_TYPE_LOW_25G_AUI_AOC_ACC:
	case ICE_PHY_TYPE_LOW_25G_AUI_C2C:
		speed_phy_type_low = ICE_AQ_LINK_SPEED_25GB;
		break;
	case ICE_PHY_TYPE_LOW_40GBASE_CR4:
	case ICE_PHY_TYPE_LOW_40GBASE_SR4:
	case ICE_PHY_TYPE_LOW_40GBASE_LR4:
	case ICE_PHY_TYPE_LOW_40GBASE_KR4:
	case ICE_PHY_TYPE_LOW_40G_XLAUI_AOC_ACC:
	case ICE_PHY_TYPE_LOW_40G_XLAUI:
		speed_phy_type_low = ICE_AQ_LINK_SPEED_40GB;
		break;
	case ICE_PHY_TYPE_LOW_50GBASE_CR2:
	case ICE_PHY_TYPE_LOW_50GBASE_SR2:
	case ICE_PHY_TYPE_LOW_50GBASE_LR2:
	case ICE_PHY_TYPE_LOW_50GBASE_KR2:
	case ICE_PHY_TYPE_LOW_50G_LAUI2_AOC_ACC:
	case ICE_PHY_TYPE_LOW_50G_LAUI2:
	case ICE_PHY_TYPE_LOW_50G_AUI2_AOC_ACC:
	case ICE_PHY_TYPE_LOW_50G_AUI2:
	case ICE_PHY_TYPE_LOW_50GBASE_CP:
	case ICE_PHY_TYPE_LOW_50GBASE_SR:
	case ICE_PHY_TYPE_LOW_50GBASE_FR:
	case ICE_PHY_TYPE_LOW_50GBASE_LR:
	case ICE_PHY_TYPE_LOW_50GBASE_KR_PAM4:
	case ICE_PHY_TYPE_LOW_50G_AUI1_AOC_ACC:
	case ICE_PHY_TYPE_LOW_50G_AUI1:
		speed_phy_type_low = ICE_AQ_LINK_SPEED_50GB;
		break;
	case ICE_PHY_TYPE_LOW_100GBASE_CR4:
	case ICE_PHY_TYPE_LOW_100GBASE_SR4:
	case ICE_PHY_TYPE_LOW_100GBASE_LR4:
	case ICE_PHY_TYPE_LOW_100GBASE_KR4:
	case ICE_PHY_TYPE_LOW_100G_CAUI4_AOC_ACC:
	case ICE_PHY_TYPE_LOW_100G_CAUI4:
	case ICE_PHY_TYPE_LOW_100G_AUI4_AOC_ACC:
	case ICE_PHY_TYPE_LOW_100G_AUI4:
	case ICE_PHY_TYPE_LOW_100GBASE_CR_PAM4:
	case ICE_PHY_TYPE_LOW_100GBASE_KR_PAM4:
	case ICE_PHY_TYPE_LOW_100GBASE_CP2:
	case ICE_PHY_TYPE_LOW_100GBASE_SR2:
	case ICE_PHY_TYPE_LOW_100GBASE_DR:
		speed_phy_type_low = ICE_AQ_LINK_SPEED_100GB;
		break;
	default:
		speed_phy_type_low = ICE_AQ_LINK_SPEED_UNKNOWN;
		break;
	}

	switch (phy_type_high) {
	case ICE_PHY_TYPE_HIGH_100GBASE_KR2_PAM4:
	case ICE_PHY_TYPE_HIGH_100G_CAUI2_AOC_ACC:
	case ICE_PHY_TYPE_HIGH_100G_CAUI2:
	case ICE_PHY_TYPE_HIGH_100G_AUI2_AOC_ACC:
	case ICE_PHY_TYPE_HIGH_100G_AUI2:
		speed_phy_type_high = ICE_AQ_LINK_SPEED_100GB;
		break;
	case ICE_PHY_TYPE_HIGH_200G_CR4_PAM4:
	case ICE_PHY_TYPE_HIGH_200G_SR4:
	case ICE_PHY_TYPE_HIGH_200G_FR4:
	case ICE_PHY_TYPE_HIGH_200G_LR4:
	case ICE_PHY_TYPE_HIGH_200G_DR4:
	case ICE_PHY_TYPE_HIGH_200G_KR4_PAM4:
	case ICE_PHY_TYPE_HIGH_200G_AUI4_AOC_ACC:
	case ICE_PHY_TYPE_HIGH_200G_AUI4:
		speed_phy_type_high = ICE_AQ_LINK_SPEED_200GB;
		break;
	default:
		speed_phy_type_high = ICE_AQ_LINK_SPEED_UNKNOWN;
		break;
	}

	if (speed_phy_type_low == ICE_AQ_LINK_SPEED_UNKNOWN &&
	    speed_phy_type_high == ICE_AQ_LINK_SPEED_UNKNOWN)
		return ICE_AQ_LINK_SPEED_UNKNOWN;
	else if (speed_phy_type_low != ICE_AQ_LINK_SPEED_UNKNOWN &&
		 speed_phy_type_high != ICE_AQ_LINK_SPEED_UNKNOWN)
		return ICE_AQ_LINK_SPEED_UNKNOWN;
	else if (speed_phy_type_low != ICE_AQ_LINK_SPEED_UNKNOWN &&
		 speed_phy_type_high == ICE_AQ_LINK_SPEED_UNKNOWN)
		return speed_phy_type_low;
	else
		return speed_phy_type_high;
}

/**
 * ice_update_phy_type
 * @phy_type_low: pointer to the lower part of phy_type
 * @phy_type_high: pointer to the higher part of phy_type
 * @link_speeds_bitmap: targeted link speeds bitmap
 *
 * Note: For the link_speeds_bitmap structure, you can check it at
 * [ice_aqc_get_link_status->link_speed]. Caller can pass in
 * link_speeds_bitmap include multiple speeds.
 *
 * Each entry in this [phy_type_low, phy_type_high] structure will
 * present a certain link speed. This helper function will turn on bits
 * in [phy_type_low, phy_type_high] structure based on the value of
 * link_speeds_bitmap input parameter.
 */
void
ice_update_phy_type(u64 *phy_type_low, u64 *phy_type_high,
		    u16 link_speeds_bitmap)
{
	u64 pt_high;
	u64 pt_low;
	int index;
	u16 speed;

	/* We first check with low part of phy_type */
	for (index = 0; index <= ICE_PHY_TYPE_LOW_MAX_INDEX; index++) {
		pt_low = BIT_ULL(index);
		speed = ice_get_link_speed_based_on_phy_type(pt_low, 0);

		if (link_speeds_bitmap & speed)
			*phy_type_low |= BIT_ULL(index);
	}

	/* We then check with high part of phy_type */
	for (index = 0; index <= ICE_PHY_TYPE_HIGH_MAX_INDEX; index++) {
		pt_high = BIT_ULL(index);
		speed = ice_get_link_speed_based_on_phy_type(0, pt_high);

		if (link_speeds_bitmap & speed)
			*phy_type_high |= BIT_ULL(index);
	}
}

/**
 * ice_aq_set_phy_cfg
 * @hw: pointer to the HW struct
 * @pi: port info structure of the interested logical port
 * @cfg: structure with PHY configuration data to be set
 * @cd: pointer to command details structure or NULL
 *
 * Set the various PHY configuration parameters supported on the Port.
 * One or more of the Set PHY config parameters may be ignored in an MFP
 * mode as the PF may not have the privilege to set some of the PHY Config
 * parameters. This status will be indicated by the command response (0x0601).
 */
int
ice_aq_set_phy_cfg(struct ice_hw *hw, struct ice_port_info *pi,
		   struct ice_aqc_set_phy_cfg_data *cfg, struct ice_sq_cd *cd)
{
	struct ice_aqc_set_phy_cfg *cmd;
	struct libie_aq_desc desc;
	int status;

	if (!cfg)
		return -EINVAL;

	/* Ensure that only valid bits of cfg->caps can be turned on. */
	if (cfg->caps & ~ICE_AQ_PHY_ENA_VALID_MASK) {
		ice_debug(hw, ICE_DBG_PHY, "Invalid bit is set in ice_aqc_set_phy_cfg_data->caps : 0x%x\n",
			  cfg->caps);

		cfg->caps &= ICE_AQ_PHY_ENA_VALID_MASK;
	}

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_set_phy_cfg);
	cmd = libie_aq_raw(&desc);
	cmd->lport_num = pi->lport;
	desc.flags |= cpu_to_le16(LIBIE_AQ_FLAG_RD);

	ice_debug(hw, ICE_DBG_LINK, "set phy cfg\n");
	ice_debug(hw, ICE_DBG_LINK, "	phy_type_low = 0x%llx\n",
		  (unsigned long long)le64_to_cpu(cfg->phy_type_low));
	ice_debug(hw, ICE_DBG_LINK, "	phy_type_high = 0x%llx\n",
		  (unsigned long long)le64_to_cpu(cfg->phy_type_high));
	ice_debug(hw, ICE_DBG_LINK, "	caps = 0x%x\n", cfg->caps);
	ice_debug(hw, ICE_DBG_LINK, "	low_power_ctrl_an = 0x%x\n",
		  cfg->low_power_ctrl_an);
	ice_debug(hw, ICE_DBG_LINK, "	eee_cap = 0x%x\n", cfg->eee_cap);
	ice_debug(hw, ICE_DBG_LINK, "	eeer_value = 0x%x\n", cfg->eeer_value);
	ice_debug(hw, ICE_DBG_LINK, "	link_fec_opt = 0x%x\n",
		  cfg->link_fec_opt);

	status = ice_aq_send_cmd(hw, &desc, cfg, sizeof(*cfg), cd);
	if (hw->adminq.sq_last_status == LIBIE_AQ_RC_EMODE)
		status = 0;

	if (!status)
		pi->phy.curr_user_phy_cfg = *cfg;

	return status;
}

/**
 * ice_update_link_info - update status of the HW network link
 * @pi: port info structure of the interested logical port
 */
int ice_update_link_info(struct ice_port_info *pi)
{
	struct ice_link_status *li;
	int status;

	if (!pi)
		return -EINVAL;

	li = &pi->phy.link_info;

	status = ice_aq_get_link_info(pi, true, NULL, NULL);
	if (status)
		return status;

	if (li->link_info & ICE_AQ_MEDIA_AVAILABLE) {
		struct ice_aqc_get_phy_caps_data *pcaps __free(kfree) = NULL;

		pcaps = kzalloc(sizeof(*pcaps), GFP_KERNEL);
		if (!pcaps)
			return -ENOMEM;

		status = ice_aq_get_phy_caps(pi, false, ICE_AQC_REPORT_TOPO_CAP_MEDIA,
					     pcaps, NULL);
	}

	return status;
}

/**
 * ice_aq_get_phy_equalization - function to read serdes equaliser
 * value from firmware using admin queue command.
 * @hw: pointer to the HW struct
 * @data_in: represents the serdes equalization parameter requested
 * @op_code: represents the serdes number and flag to represent tx or rx
 * @serdes_num: represents the serdes number
 * @output: pointer to the caller-supplied buffer to return serdes equaliser
 *
 * Return: non-zero status on error and 0 on success.
 */
int ice_aq_get_phy_equalization(struct ice_hw *hw, u16 data_in, u16 op_code,
				u8 serdes_num, int *output)
{
	struct ice_aqc_dnl_call_command *cmd;
	struct ice_aqc_dnl_call buf = {};
	struct libie_aq_desc desc;
	int err;

	buf.sto.txrx_equa_reqs.data_in = cpu_to_le16(data_in);
	buf.sto.txrx_equa_reqs.op_code_serdes_sel =
		cpu_to_le16(op_code | (serdes_num & 0xF));
	cmd = libie_aq_raw(&desc);
	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_dnl_call);
	desc.flags |= cpu_to_le16(LIBIE_AQ_FLAG_BUF |
				  LIBIE_AQ_FLAG_RD |
				  LIBIE_AQ_FLAG_SI);
	desc.datalen = cpu_to_le16(sizeof(struct ice_aqc_dnl_call));
	cmd->activity_id = cpu_to_le16(ICE_AQC_ACT_ID_DNL);

	err = ice_aq_send_cmd(hw, &desc, &buf, sizeof(struct ice_aqc_dnl_call),
			      NULL);
	*output = err ? 0 : buf.sto.txrx_equa_resp.val;

	return err;
}

#define FEC_REG_PORT(port) {	\
	FEC_CORR_LOW_REG_PORT##port,		\
	FEC_CORR_HIGH_REG_PORT##port,	\
	FEC_UNCORR_LOW_REG_PORT##port,	\
	FEC_UNCORR_HIGH_REG_PORT##port,	\
}

static const u32 fec_reg[][ICE_FEC_MAX] = {
	FEC_REG_PORT(0),
	FEC_REG_PORT(1),
	FEC_REG_PORT(2),
	FEC_REG_PORT(3)
};

/**
 * ice_aq_get_fec_stats - reads fec stats from phy
 * @hw: pointer to the HW struct
 * @pcs_quad: represents pcsquad of user input serdes
 * @pcs_port: represents the pcs port number part of above pcs quad
 * @fec_type: represents FEC stats type
 * @output: pointer to the caller-supplied buffer to return requested fec stats
 *
 * Return: non-zero status on error and 0 on success.
 */
int ice_aq_get_fec_stats(struct ice_hw *hw, u16 pcs_quad, u16 pcs_port,
			 enum ice_fec_stats_types fec_type, u32 *output)
{
	u16 flag = (LIBIE_AQ_FLAG_RD | LIBIE_AQ_FLAG_BUF | LIBIE_AQ_FLAG_SI);
	struct ice_sbq_msg_input msg = {};
	u32 receiver_id, reg_offset;
	int err;

	if (pcs_port > 3)
		return -EINVAL;

	reg_offset = fec_reg[pcs_port][fec_type];

	if (pcs_quad == 0)
		receiver_id = FEC_RECEIVER_ID_PCS0;
	else if (pcs_quad == 1)
		receiver_id = FEC_RECEIVER_ID_PCS1;
	else
		return -EINVAL;

	msg.msg_addr_low = lower_16_bits(reg_offset);
	msg.msg_addr_high = receiver_id;
	msg.opcode = ice_sbq_msg_rd;
	msg.dest_dev = ice_sbq_dev_phy_0;

	err = ice_sbq_rw_reg(hw, &msg, flag);
	if (err)
		return err;

	*output = msg.data;
	return 0;
}

/**
 * ice_cache_phy_user_req
 * @pi: port information structure
 * @cache_data: PHY logging data
 * @cache_mode: PHY logging mode
 *
 * Log the user request on (FC, FEC, SPEED) for later use.
 */
static void
ice_cache_phy_user_req(struct ice_port_info *pi,
		       struct ice_phy_cache_mode_data cache_data,
		       enum ice_phy_cache_mode cache_mode)
{
	if (!pi)
		return;

	switch (cache_mode) {
	case ICE_FC_MODE:
		pi->phy.curr_user_fc_req = cache_data.data.curr_user_fc_req;
		break;
	case ICE_SPEED_MODE:
		pi->phy.curr_user_speed_req =
			cache_data.data.curr_user_speed_req;
		break;
	case ICE_FEC_MODE:
		pi->phy.curr_user_fec_req = cache_data.data.curr_user_fec_req;
		break;
	default:
		break;
	}
}

/**
 * ice_caps_to_fc_mode
 * @caps: PHY capabilities
 *
 * Convert PHY FC capabilities to ice FC mode
 */
enum ice_fc_mode ice_caps_to_fc_mode(u8 caps)
{
	if (caps & ICE_AQC_PHY_EN_TX_LINK_PAUSE &&
	    caps & ICE_AQC_PHY_EN_RX_LINK_PAUSE)
		return ICE_FC_FULL;

	if (caps & ICE_AQC_PHY_EN_TX_LINK_PAUSE)
		return ICE_FC_TX_PAUSE;

	if (caps & ICE_AQC_PHY_EN_RX_LINK_PAUSE)
		return ICE_FC_RX_PAUSE;

	return ICE_FC_NONE;
}

/**
 * ice_caps_to_fec_mode
 * @caps: PHY capabilities
 * @fec_options: Link FEC options
 *
 * Convert PHY FEC capabilities to ice FEC mode
 */
enum ice_fec_mode ice_caps_to_fec_mode(u8 caps, u8 fec_options)
{
	if (caps & ICE_AQC_PHY_EN_AUTO_FEC)
		return ICE_FEC_AUTO;

	if (fec_options & (ICE_AQC_PHY_FEC_10G_KR_40G_KR4_EN |
			   ICE_AQC_PHY_FEC_10G_KR_40G_KR4_REQ |
			   ICE_AQC_PHY_FEC_25G_KR_CLAUSE74_EN |
			   ICE_AQC_PHY_FEC_25G_KR_REQ))
		return ICE_FEC_BASER;

	if (fec_options & (ICE_AQC_PHY_FEC_25G_RS_528_REQ |
			   ICE_AQC_PHY_FEC_25G_RS_544_REQ |
			   ICE_AQC_PHY_FEC_25G_RS_CLAUSE91_EN))
		return ICE_FEC_RS;

	return ICE_FEC_NONE;
}

/**
 * ice_cfg_phy_fc - Configure PHY FC data based on FC mode
 * @pi: port information structure
 * @cfg: PHY configuration data to set FC mode
 * @req_mode: FC mode to configure
 */
int
ice_cfg_phy_fc(struct ice_port_info *pi, struct ice_aqc_set_phy_cfg_data *cfg,
	       enum ice_fc_mode req_mode)
{
	struct ice_phy_cache_mode_data cache_data;
	u8 pause_mask = 0x0;

	if (!pi || !cfg)
		return -EINVAL;

	switch (req_mode) {
	case ICE_FC_FULL:
		pause_mask |= ICE_AQC_PHY_EN_TX_LINK_PAUSE;
		pause_mask |= ICE_AQC_PHY_EN_RX_LINK_PAUSE;
		break;
	case ICE_FC_RX_PAUSE:
		pause_mask |= ICE_AQC_PHY_EN_RX_LINK_PAUSE;
		break;
	case ICE_FC_TX_PAUSE:
		pause_mask |= ICE_AQC_PHY_EN_TX_LINK_PAUSE;
		break;
	default:
		break;
	}

	/* clear the old pause settings */
	cfg->caps &= ~(ICE_AQC_PHY_EN_TX_LINK_PAUSE |
		ICE_AQC_PHY_EN_RX_LINK_PAUSE);

	/* set the new capabilities */
	cfg->caps |= pause_mask;

	/* Cache user FC request */
	cache_data.data.curr_user_fc_req = req_mode;
	ice_cache_phy_user_req(pi, cache_data, ICE_FC_MODE);

	return 0;
}

/**
 * ice_set_fc
 * @pi: port information structure
 * @aq_failures: pointer to status code, specific to ice_set_fc routine
 * @ena_auto_link_update: enable automatic link update
 *
 * Set the requested flow control mode.
 */
int
ice_set_fc(struct ice_port_info *pi, u8 *aq_failures, bool ena_auto_link_update)
{
	struct ice_aqc_get_phy_caps_data *pcaps __free(kfree) = NULL;
	struct ice_aqc_set_phy_cfg_data cfg = { 0 };
	struct ice_hw *hw;
	int status;

	if (!pi || !aq_failures)
		return -EINVAL;

	*aq_failures = 0;
	hw = pi->hw;

	pcaps = kzalloc(sizeof(*pcaps), GFP_KERNEL);
	if (!pcaps)
		return -ENOMEM;

	/* Get the current PHY config */
	status = ice_aq_get_phy_caps(pi, false, ICE_AQC_REPORT_ACTIVE_CFG,
				     pcaps, NULL);
	if (status) {
		*aq_failures = ICE_SET_FC_AQ_FAIL_GET;
		goto out;
	}

	ice_copy_phy_caps_to_cfg(pi, pcaps, &cfg);

	/* Configure the set PHY data */
	status = ice_cfg_phy_fc(pi, &cfg, pi->fc.req_mode);
	if (status)
		goto out;

	/* If the capabilities have changed, then set the new config */
	if (cfg.caps != pcaps->caps) {
		int retry_count, retry_max = 10;

		/* Auto restart link so settings take effect */
		if (ena_auto_link_update)
			cfg.caps |= ICE_AQ_PHY_ENA_AUTO_LINK_UPDT;

		status = ice_aq_set_phy_cfg(hw, pi, &cfg, NULL);
		if (status) {
			*aq_failures = ICE_SET_FC_AQ_FAIL_SET;
			goto out;
		}

		/* Update the link info
		 * It sometimes takes a really long time for link to
		 * come back from the atomic reset. Thus, we wait a
		 * little bit.
		 */
		for (retry_count = 0; retry_count < retry_max; retry_count++) {
			status = ice_update_link_info(pi);

			if (!status)
				break;

			mdelay(100);
		}

		if (status)
			*aq_failures = ICE_SET_FC_AQ_FAIL_UPDATE;
	}

out:
	return status;
}

/**
 * ice_phy_caps_equals_cfg
 * @phy_caps: PHY capabilities
 * @phy_cfg: PHY configuration
 *
 * Helper function to determine if PHY capabilities matches PHY
 * configuration
 */
bool
ice_phy_caps_equals_cfg(struct ice_aqc_get_phy_caps_data *phy_caps,
			struct ice_aqc_set_phy_cfg_data *phy_cfg)
{
	u8 caps_mask, cfg_mask;

	if (!phy_caps || !phy_cfg)
		return false;

	/* These bits are not common between capabilities and configuration.
	 * Do not use them to determine equality.
	 */
	caps_mask = ICE_AQC_PHY_CAPS_MASK & ~(ICE_AQC_PHY_AN_MODE |
					      ICE_AQC_GET_PHY_EN_MOD_QUAL);
	cfg_mask = ICE_AQ_PHY_ENA_VALID_MASK & ~ICE_AQ_PHY_ENA_AUTO_LINK_UPDT;

	if (phy_caps->phy_type_low != phy_cfg->phy_type_low ||
	    phy_caps->phy_type_high != phy_cfg->phy_type_high ||
	    ((phy_caps->caps & caps_mask) != (phy_cfg->caps & cfg_mask)) ||
	    phy_caps->low_power_ctrl_an != phy_cfg->low_power_ctrl_an ||
	    phy_caps->eee_cap != phy_cfg->eee_cap ||
	    phy_caps->eeer_value != phy_cfg->eeer_value ||
	    phy_caps->link_fec_options != phy_cfg->link_fec_opt)
		return false;

	return true;
}

/**
 * ice_copy_phy_caps_to_cfg - Copy PHY ability data to configuration data
 * @pi: port information structure
 * @caps: PHY ability structure to copy date from
 * @cfg: PHY configuration structure to copy data to
 *
 * Helper function to copy AQC PHY get ability data to PHY set configuration
 * data structure
 */
void
ice_copy_phy_caps_to_cfg(struct ice_port_info *pi,
			 struct ice_aqc_get_phy_caps_data *caps,
			 struct ice_aqc_set_phy_cfg_data *cfg)
{
	if (!pi || !caps || !cfg)
		return;

	memset(cfg, 0, sizeof(*cfg));
	cfg->phy_type_low = caps->phy_type_low;
	cfg->phy_type_high = caps->phy_type_high;
	cfg->caps = caps->caps;
	cfg->low_power_ctrl_an = caps->low_power_ctrl_an;
	cfg->eee_cap = caps->eee_cap;
	cfg->eeer_value = caps->eeer_value;
	cfg->link_fec_opt = caps->link_fec_options;
	cfg->module_compliance_enforcement =
		caps->module_compliance_enforcement;
}

/**
 * ice_cfg_phy_fec - Configure PHY FEC data based on FEC mode
 * @pi: port information structure
 * @cfg: PHY configuration data to set FEC mode
 * @fec: FEC mode to configure
 */
int
ice_cfg_phy_fec(struct ice_port_info *pi, struct ice_aqc_set_phy_cfg_data *cfg,
		enum ice_fec_mode fec)
{
	struct ice_aqc_get_phy_caps_data *pcaps __free(kfree) = NULL;
	struct ice_hw *hw;
	int status;

	if (!pi || !cfg)
		return -EINVAL;

	hw = pi->hw;

	pcaps = kzalloc(sizeof(*pcaps), GFP_KERNEL);
	if (!pcaps)
		return -ENOMEM;

	status = ice_aq_get_phy_caps(pi, false,
				     (ice_fw_supports_report_dflt_cfg(hw) ?
				      ICE_AQC_REPORT_DFLT_CFG :
				      ICE_AQC_REPORT_TOPO_CAP_MEDIA), pcaps, NULL);
	if (status)
		goto out;

	cfg->caps |= pcaps->caps & ICE_AQC_PHY_EN_AUTO_FEC;
	cfg->link_fec_opt = pcaps->link_fec_options;

	switch (fec) {
	case ICE_FEC_BASER:
		/* Clear RS bits, and AND BASE-R ability
		 * bits and OR request bits.
		 */
		cfg->link_fec_opt &= ICE_AQC_PHY_FEC_10G_KR_40G_KR4_EN |
			ICE_AQC_PHY_FEC_25G_KR_CLAUSE74_EN;
		cfg->link_fec_opt |= ICE_AQC_PHY_FEC_10G_KR_40G_KR4_REQ |
			ICE_AQC_PHY_FEC_25G_KR_REQ;
		break;
	case ICE_FEC_RS:
		/* Clear BASE-R bits, and AND RS ability
		 * bits and OR request bits.
		 */
		cfg->link_fec_opt &= ICE_AQC_PHY_FEC_25G_RS_CLAUSE91_EN;
		cfg->link_fec_opt |= ICE_AQC_PHY_FEC_25G_RS_528_REQ |
			ICE_AQC_PHY_FEC_25G_RS_544_REQ;
		break;
	case ICE_FEC_NONE:
		/* Clear all FEC option bits. */
		cfg->link_fec_opt &= ~ICE_AQC_PHY_FEC_MASK;
		break;
	case ICE_FEC_AUTO:
		/* AND auto FEC bit, and all caps bits. */
		cfg->caps &= ICE_AQC_PHY_CAPS_MASK;
		cfg->link_fec_opt |= pcaps->link_fec_options;
		break;
	default:
		status = -EINVAL;
		break;
	}

	if (fec == ICE_FEC_AUTO && ice_fw_supports_link_override(hw) &&
	    !ice_fw_supports_report_dflt_cfg(hw)) {
		struct ice_link_default_override_tlv tlv = { 0 };

		status = ice_get_link_default_override(&tlv, pi);
		if (status)
			goto out;

		if (!(tlv.options & ICE_LINK_OVERRIDE_STRICT_MODE) &&
		    (tlv.options & ICE_LINK_OVERRIDE_EN))
			cfg->link_fec_opt = tlv.fec_options;
	}

out:
	return status;
}

/**
 * ice_get_link_status - get status of the HW network link
 * @pi: port information structure
 * @link_up: pointer to bool (true/false = linkup/linkdown)
 *
 * Variable link_up is true if link is up, false if link is down.
 * The variable link_up is invalid if status is non zero. As a
 * result of this call, link status reporting becomes enabled
 */
int ice_get_link_status(struct ice_port_info *pi, bool *link_up)
{
	struct ice_phy_info *phy_info;
	int status = 0;

	if (!pi || !link_up)
		return -EINVAL;

	phy_info = &pi->phy;

	if (phy_info->get_link_info) {
		status = ice_update_link_info(pi);

		if (status)
			ice_debug(pi->hw, ICE_DBG_LINK, "get link status error, status = %d\n",
				  status);
	}

	*link_up = phy_info->link_info.link_info & ICE_AQ_LINK_UP;

	return status;
}

/**
 * ice_aq_set_link_restart_an
 * @pi: pointer to the port information structure
 * @ena_link: if true: enable link, if false: disable link
 * @cd: pointer to command details structure or NULL
 *
 * Sets up the link and restarts the Auto-Negotiation over the link.
 */
int
ice_aq_set_link_restart_an(struct ice_port_info *pi, bool ena_link,
			   struct ice_sq_cd *cd)
{
	struct ice_aqc_restart_an *cmd;
	struct libie_aq_desc desc;

	cmd = libie_aq_raw(&desc);

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_restart_an);

	cmd->cmd_flags = ICE_AQC_RESTART_AN_LINK_RESTART;
	cmd->lport_num = pi->lport;
	if (ena_link)
		cmd->cmd_flags |= ICE_AQC_RESTART_AN_LINK_ENABLE;
	else
		cmd->cmd_flags &= ~ICE_AQC_RESTART_AN_LINK_ENABLE;

	return ice_aq_send_cmd(pi->hw, &desc, NULL, 0, cd);
}

/**
 * ice_aq_set_event_mask
 * @hw: pointer to the HW struct
 * @port_num: port number of the physical function
 * @mask: event mask to be set
 * @cd: pointer to command details structure or NULL
 *
 * Set event mask (0x0613)
 */
int
ice_aq_set_event_mask(struct ice_hw *hw, u8 port_num, u16 mask,
		      struct ice_sq_cd *cd)
{
	struct ice_aqc_set_event_mask *cmd;
	struct libie_aq_desc desc;

	cmd = libie_aq_raw(&desc);

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_set_event_mask);

	cmd->lport_num = port_num;

	cmd->event_mask = cpu_to_le16(mask);
	return ice_aq_send_cmd(hw, &desc, NULL, 0, cd);
}

/**
 * ice_aq_set_mac_loopback
 * @hw: pointer to the HW struct
 * @ena_lpbk: Enable or Disable loopback
 * @cd: pointer to command details structure or NULL
 *
 * Enable/disable loopback on a given port
 */
int
ice_aq_set_mac_loopback(struct ice_hw *hw, bool ena_lpbk, struct ice_sq_cd *cd)
{
	struct ice_aqc_set_mac_lb *cmd;
	struct libie_aq_desc desc;

	cmd = libie_aq_raw(&desc);

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_set_mac_lb);
	if (ena_lpbk)
		cmd->lb_mode = ICE_AQ_MAC_LB_EN;

	return ice_aq_send_cmd(hw, &desc, NULL, 0, cd);
}

/**
 * ice_aq_set_port_id_led
 * @pi: pointer to the port information
 * @is_orig_mode: is this LED set to original mode (by the net-list)
 * @cd: pointer to command details structure or NULL
 *
 * Set LED value for the given port (0x06e9)
 */
int
ice_aq_set_port_id_led(struct ice_port_info *pi, bool is_orig_mode,
		       struct ice_sq_cd *cd)
{
	struct ice_aqc_set_port_id_led *cmd;
	struct ice_hw *hw = pi->hw;
	struct libie_aq_desc desc;

	cmd = libie_aq_raw(&desc);

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_set_port_id_led);

	if (is_orig_mode)
		cmd->ident_mode = ICE_AQC_PORT_IDENT_LED_ORIG;
	else
		cmd->ident_mode = ICE_AQC_PORT_IDENT_LED_BLINK;

	return ice_aq_send_cmd(hw, &desc, NULL, 0, cd);
}

/**
 * ice_aq_get_port_options
 * @hw: pointer to the HW struct
 * @options: buffer for the resultant port options
 * @option_count: input - size of the buffer in port options structures,
 *                output - number of returned port options
 * @lport: logical port to call the command with (optional)
 * @lport_valid: when false, FW uses port owned by the PF instead of lport,
 *               when PF owns more than 1 port it must be true
 * @active_option_idx: index of active port option in returned buffer
 * @active_option_valid: active option in returned buffer is valid
 * @pending_option_idx: index of pending port option in returned buffer
 * @pending_option_valid: pending option in returned buffer is valid
 *
 * Calls Get Port Options AQC (0x06ea) and verifies result.
 */
int
ice_aq_get_port_options(struct ice_hw *hw,
			struct ice_aqc_get_port_options_elem *options,
			u8 *option_count, u8 lport, bool lport_valid,
			u8 *active_option_idx, bool *active_option_valid,
			u8 *pending_option_idx, bool *pending_option_valid)
{
	struct ice_aqc_get_port_options *cmd;
	struct libie_aq_desc desc;
	int status;
	u8 i;

	/* options buffer shall be able to hold max returned options */
	if (*option_count < ICE_AQC_PORT_OPT_COUNT_M)
		return -EINVAL;

	cmd = libie_aq_raw(&desc);
	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_get_port_options);

	if (lport_valid)
		cmd->lport_num = lport;
	cmd->lport_num_valid = lport_valid;

	status = ice_aq_send_cmd(hw, &desc, options,
				 *option_count * sizeof(*options), NULL);
	if (status)
		return status;

	/* verify direct FW response & set output parameters */
	*option_count = FIELD_GET(ICE_AQC_PORT_OPT_COUNT_M,
				  cmd->port_options_count);
	ice_debug(hw, ICE_DBG_PHY, "options: %x\n", *option_count);
	*active_option_valid = FIELD_GET(ICE_AQC_PORT_OPT_VALID,
					 cmd->port_options);
	if (*active_option_valid) {
		*active_option_idx = FIELD_GET(ICE_AQC_PORT_OPT_ACTIVE_M,
					       cmd->port_options);
		if (*active_option_idx > (*option_count - 1))
			return -EIO;
		ice_debug(hw, ICE_DBG_PHY, "active idx: %x\n",
			  *active_option_idx);
	}

	*pending_option_valid = FIELD_GET(ICE_AQC_PENDING_PORT_OPT_VALID,
					  cmd->pending_port_option_status);
	if (*pending_option_valid) {
		*pending_option_idx = FIELD_GET(ICE_AQC_PENDING_PORT_OPT_IDX_M,
						cmd->pending_port_option_status);
		if (*pending_option_idx > (*option_count - 1))
			return -EIO;
		ice_debug(hw, ICE_DBG_PHY, "pending idx: %x\n",
			  *pending_option_idx);
	}

	/* mask output options fields */
	for (i = 0; i < *option_count; i++) {
		options[i].pmd = FIELD_GET(ICE_AQC_PORT_OPT_PMD_COUNT_M,
					   options[i].pmd);
		options[i].max_lane_speed = FIELD_GET(ICE_AQC_PORT_OPT_MAX_LANE_M,
						      options[i].max_lane_speed);
		ice_debug(hw, ICE_DBG_PHY, "pmds: %x max speed: %x\n",
			  options[i].pmd, options[i].max_lane_speed);
	}

	return 0;
}

/**
 * ice_aq_set_port_option
 * @hw: pointer to the HW struct
 * @lport: logical port to call the command with
 * @lport_valid: when false, FW uses port owned by the PF instead of lport,
 *               when PF owns more than 1 port it must be true
 * @new_option: new port option to be written
 *
 * Calls Set Port Options AQC (0x06eb).
 */
int
ice_aq_set_port_option(struct ice_hw *hw, u8 lport, u8 lport_valid,
		       u8 new_option)
{
	struct ice_aqc_set_port_option *cmd;
	struct libie_aq_desc desc;

	if (new_option > ICE_AQC_PORT_OPT_COUNT_M)
		return -EINVAL;

	cmd = libie_aq_raw(&desc);
	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_set_port_option);

	if (lport_valid)
		cmd->lport_num = lport;

	cmd->lport_num_valid = lport_valid;
	cmd->selected_port_option = new_option;

	return ice_aq_send_cmd(hw, &desc, NULL, 0, NULL);
}

/**
 * ice_get_phy_lane_number - Get PHY lane number for current adapter
 * @hw: pointer to the hw struct
 *
 * Return: PHY lane number on success, negative error code otherwise.
 */
int ice_get_phy_lane_number(struct ice_hw *hw)
{
	struct ice_aqc_get_port_options_elem *options;
	unsigned int lport = 0;
	unsigned int lane;
	int err;

	options = kcalloc(ICE_AQC_PORT_OPT_MAX, sizeof(*options), GFP_KERNEL);
	if (!options)
		return -ENOMEM;

	for (lane = 0; lane < ICE_MAX_PORT_PER_PCI_DEV; lane++) {
		u8 options_count = ICE_AQC_PORT_OPT_MAX;
		u8 speed, active_idx, pending_idx;
		bool active_valid, pending_valid;

		err = ice_aq_get_port_options(hw, options, &options_count, lane,
					      true, &active_idx, &active_valid,
					      &pending_idx, &pending_valid);
		if (err)
			goto err;

		if (!active_valid)
			continue;

		speed = options[active_idx].max_lane_speed;
		/* If we don't get speed for this lane, it's unoccupied */
		if (speed > ICE_AQC_PORT_OPT_MAX_LANE_40G)
			continue;

		if (hw->pf_id == lport) {
			if (hw->mac_type == ICE_MAC_GENERIC_3K_E825 &&
			    ice_is_dual(hw) && !ice_is_primary(hw))
				lane += ICE_PORTS_PER_QUAD;
			kfree(options);
			return lane;
		}
		lport++;
	}

	/* PHY lane not found */
	err = -ENXIO;
err:
	kfree(options);
	return err;
}

/**
 * ice_aq_sff_eeprom
 * @hw: pointer to the HW struct
 * @lport: bits [7:0] = logical port, bit [8] = logical port valid
 * @bus_addr: I2C bus address of the eeprom (typically 0xA0, 0=topo default)
 * @mem_addr: I2C offset. lower 8 bits for address, 8 upper bits zero padding.
 * @page: QSFP page
 * @set_page: set or ignore the page
 * @data: pointer to data buffer to be read/written to the I2C device.
 * @length: 1-16 for read, 1 for write.
 * @write: 0 read, 1 for write.
 * @cd: pointer to command details structure or NULL
 *
 * Read/Write SFF EEPROM (0x06EE)
 */
int
ice_aq_sff_eeprom(struct ice_hw *hw, u16 lport, u8 bus_addr,
		  u16 mem_addr, u8 page, u8 set_page, u8 *data, u8 length,
		  bool write, struct ice_sq_cd *cd)
{
	struct ice_aqc_sff_eeprom *cmd;
	struct libie_aq_desc desc;
	u16 i2c_bus_addr;
	int status;

	if (!data || (mem_addr & 0xff00))
		return -EINVAL;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_sff_eeprom);
	cmd = libie_aq_raw(&desc);
	desc.flags = cpu_to_le16(LIBIE_AQ_FLAG_RD);
	cmd->lport_num = (u8)(lport & 0xff);
	cmd->lport_num_valid = (u8)((lport >> 8) & 0x01);
	i2c_bus_addr = FIELD_PREP(ICE_AQC_SFF_I2CBUS_7BIT_M, bus_addr >> 1) |
		       FIELD_PREP(ICE_AQC_SFF_SET_EEPROM_PAGE_M, set_page);
	if (write)
		i2c_bus_addr |= ICE_AQC_SFF_IS_WRITE;
	cmd->i2c_bus_addr = cpu_to_le16(i2c_bus_addr);
	cmd->i2c_mem_addr = cpu_to_le16(mem_addr & 0xff);
	cmd->eeprom_page = le16_encode_bits(page, ICE_AQC_SFF_EEPROM_PAGE_M);

	status = ice_aq_send_cmd(hw, &desc, data, length, cd);
	return status;
}

static enum ice_lut_size ice_lut_type_to_size(enum ice_lut_type type)
{
	switch (type) {
	case ICE_LUT_VSI:
		return ICE_LUT_VSI_SIZE;
	case ICE_LUT_GLOBAL:
		return ICE_LUT_GLOBAL_SIZE;
	case ICE_LUT_PF:
		return ICE_LUT_PF_SIZE;
	}
	WARN_ONCE(1, "incorrect type passed");
	return ICE_LUT_VSI_SIZE;
}

static enum ice_aqc_lut_flags ice_lut_size_to_flag(enum ice_lut_size size)
{
	switch (size) {
	case ICE_LUT_VSI_SIZE:
		return ICE_AQC_LUT_SIZE_SMALL;
	case ICE_LUT_GLOBAL_SIZE:
		return ICE_AQC_LUT_SIZE_512;
	case ICE_LUT_PF_SIZE:
		return ICE_AQC_LUT_SIZE_2K;
	}
	WARN_ONCE(1, "incorrect size passed");
	return 0;
}

/**
 * __ice_aq_get_set_rss_lut
 * @hw: pointer to the hardware structure
 * @params: RSS LUT parameters
 * @set: set true to set the table, false to get the table
 *
 * Internal function to get (0x0B05) or set (0x0B03) RSS look up table
 */
static int
__ice_aq_get_set_rss_lut(struct ice_hw *hw,
			 struct ice_aq_get_set_rss_lut_params *params, bool set)
{
	u16 opcode, vsi_id, vsi_handle = params->vsi_handle, glob_lut_idx = 0;
	enum ice_lut_type lut_type = params->lut_type;
	struct ice_aqc_get_set_rss_lut *desc_params;
	enum ice_aqc_lut_flags flags;
	enum ice_lut_size lut_size;
	struct libie_aq_desc desc;
	u8 *lut = params->lut;


	if (!lut || !ice_is_vsi_valid(hw, vsi_handle))
		return -EINVAL;

	lut_size = ice_lut_type_to_size(lut_type);
	if (lut_size > params->lut_size)
		return -EINVAL;
	else if (set && lut_size != params->lut_size)
		return -EINVAL;

	opcode = set ? ice_aqc_opc_set_rss_lut : ice_aqc_opc_get_rss_lut;
	ice_fill_dflt_direct_cmd_desc(&desc, opcode);
	if (set)
		desc.flags |= cpu_to_le16(LIBIE_AQ_FLAG_RD);

	desc_params = libie_aq_raw(&desc);
	vsi_id = ice_get_hw_vsi_num(hw, vsi_handle);
	desc_params->vsi_id = cpu_to_le16(vsi_id | ICE_AQC_RSS_VSI_VALID);

	if (lut_type == ICE_LUT_GLOBAL)
		glob_lut_idx = FIELD_PREP(ICE_AQC_LUT_GLOBAL_IDX,
					  params->global_lut_id);

	flags = lut_type | glob_lut_idx | ice_lut_size_to_flag(lut_size);
	desc_params->flags = cpu_to_le16(flags);

	return ice_aq_send_cmd(hw, &desc, lut, lut_size, NULL);
}

/**
 * ice_aq_get_rss_lut
 * @hw: pointer to the hardware structure
 * @get_params: RSS LUT parameters used to specify which RSS LUT to get
 *
 * get the RSS lookup table, PF or VSI type
 */
int
ice_aq_get_rss_lut(struct ice_hw *hw, struct ice_aq_get_set_rss_lut_params *get_params)
{
	return __ice_aq_get_set_rss_lut(hw, get_params, false);
}

/**
 * ice_aq_set_rss_lut
 * @hw: pointer to the hardware structure
 * @set_params: RSS LUT parameters used to specify how to set the RSS LUT
 *
 * set the RSS lookup table, PF or VSI type
 */
int
ice_aq_set_rss_lut(struct ice_hw *hw, struct ice_aq_get_set_rss_lut_params *set_params)
{
	return __ice_aq_get_set_rss_lut(hw, set_params, true);
}

/**
 * __ice_aq_get_set_rss_key
 * @hw: pointer to the HW struct
 * @vsi_id: VSI FW index
 * @key: pointer to key info struct
 * @set: set true to set the key, false to get the key
 *
 * get (0x0B04) or set (0x0B02) the RSS key per VSI
 */
static int
__ice_aq_get_set_rss_key(struct ice_hw *hw, u16 vsi_id,
			 struct ice_aqc_get_set_rss_keys *key, bool set)
{
	struct ice_aqc_get_set_rss_key *desc_params;
	u16 key_size = sizeof(*key);
	struct libie_aq_desc desc;

	if (set) {
		ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_set_rss_key);
		desc.flags |= cpu_to_le16(LIBIE_AQ_FLAG_RD);
	} else {
		ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_get_rss_key);
	}

	desc_params = libie_aq_raw(&desc);
	desc_params->vsi_id = cpu_to_le16(vsi_id | ICE_AQC_RSS_VSI_VALID);

	return ice_aq_send_cmd(hw, &desc, key, key_size, NULL);
}

/**
 * ice_aq_get_rss_key
 * @hw: pointer to the HW struct
 * @vsi_handle: software VSI handle
 * @key: pointer to key info struct
 *
 * get the RSS key per VSI
 */
int
ice_aq_get_rss_key(struct ice_hw *hw, u16 vsi_handle,
		   struct ice_aqc_get_set_rss_keys *key)
{
	if (!ice_is_vsi_valid(hw, vsi_handle) || !key)
		return -EINVAL;

	return __ice_aq_get_set_rss_key(hw, ice_get_hw_vsi_num(hw, vsi_handle),
					key, false);
}

/**
 * ice_aq_set_rss_key
 * @hw: pointer to the HW struct
 * @vsi_handle: software VSI handle
 * @keys: pointer to key info struct
 *
 * set the RSS key per VSI
 */
int
ice_aq_set_rss_key(struct ice_hw *hw, u16 vsi_handle,
		   struct ice_aqc_get_set_rss_keys *keys)
{
	if (!ice_is_vsi_valid(hw, vsi_handle) || !keys)
		return -EINVAL;

	return __ice_aq_get_set_rss_key(hw, ice_get_hw_vsi_num(hw, vsi_handle),
					keys, true);
}

/**
 * ice_aq_add_lan_txq
 * @hw: pointer to the hardware structure
 * @num_qgrps: Number of added queue groups
 * @qg_list: list of queue groups to be added
 * @buf_size: size of buffer for indirect command
 * @cd: pointer to command details structure or NULL
 *
 * Add Tx LAN queue (0x0C30)
 *
 * NOTE:
 * Prior to calling add Tx LAN queue:
 * Initialize the following as part of the Tx queue context:
 * Completion queue ID if the queue uses Completion queue, Quanta profile,
 * Cache profile and Packet shaper profile.
 *
 * After add Tx LAN queue AQ command is completed:
 * Interrupts should be associated with specific queues,
 * Association of Tx queue to Doorbell queue is not part of Add LAN Tx queue
 * flow.
 */
static int
ice_aq_add_lan_txq(struct ice_hw *hw, u8 num_qgrps,
		   struct ice_aqc_add_tx_qgrp *qg_list, u16 buf_size,
		   struct ice_sq_cd *cd)
{
	struct ice_aqc_add_tx_qgrp *list;
	struct ice_aqc_add_txqs *cmd;
	struct libie_aq_desc desc;
	u16 i, sum_size = 0;

	cmd = libie_aq_raw(&desc);

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_add_txqs);

	if (!qg_list)
		return -EINVAL;

	if (num_qgrps > ICE_LAN_TXQ_MAX_QGRPS)
		return -EINVAL;

	for (i = 0, list = qg_list; i < num_qgrps; i++) {
		sum_size += struct_size(list, txqs, list->num_txqs);
		list = (struct ice_aqc_add_tx_qgrp *)(list->txqs +
						      list->num_txqs);
	}

	if (buf_size != sum_size)
		return -EINVAL;

	desc.flags |= cpu_to_le16(LIBIE_AQ_FLAG_RD);

	cmd->num_qgrps = num_qgrps;

	return ice_aq_send_cmd(hw, &desc, qg_list, buf_size, cd);
}

/**
 * ice_aq_dis_lan_txq
 * @hw: pointer to the hardware structure
 * @num_qgrps: number of groups in the list
 * @qg_list: the list of groups to disable
 * @buf_size: the total size of the qg_list buffer in bytes
 * @rst_src: if called due to reset, specifies the reset source
 * @vmvf_num: the relative VM or VF number that is undergoing the reset
 * @cd: pointer to command details structure or NULL
 *
 * Disable LAN Tx queue (0x0C31)
 */
static int
ice_aq_dis_lan_txq(struct ice_hw *hw, u8 num_qgrps,
		   struct ice_aqc_dis_txq_item *qg_list, u16 buf_size,
		   enum ice_disq_rst_src rst_src, u16 vmvf_num,
		   struct ice_sq_cd *cd)
{
	struct ice_aqc_dis_txq_item *item;
	struct ice_aqc_dis_txqs *cmd;
	struct libie_aq_desc desc;
	u16 vmvf_and_timeout;
	u16 i, sz = 0;
	int status;

	cmd = libie_aq_raw(&desc);
	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_dis_txqs);

	/* qg_list can be NULL only in VM/VF reset flow */
	if (!qg_list && !rst_src)
		return -EINVAL;

	if (num_qgrps > ICE_LAN_TXQ_MAX_QGRPS)
		return -EINVAL;

	cmd->num_entries = num_qgrps;

	vmvf_and_timeout = FIELD_PREP(ICE_AQC_Q_DIS_TIMEOUT_M, 5);

	switch (rst_src) {
	case ICE_VM_RESET:
		cmd->cmd_type = ICE_AQC_Q_DIS_CMD_VM_RESET;
		vmvf_and_timeout |= vmvf_num & ICE_AQC_Q_DIS_VMVF_NUM_M;
		break;
	case ICE_VF_RESET:
		cmd->cmd_type = ICE_AQC_Q_DIS_CMD_VF_RESET;
		/* In this case, FW expects vmvf_num to be absolute VF ID */
		vmvf_and_timeout |= (vmvf_num + hw->func_caps.vf_base_id) &
				    ICE_AQC_Q_DIS_VMVF_NUM_M;
		break;
	case ICE_NO_RESET:
	default:
		break;
	}

	cmd->vmvf_and_timeout = cpu_to_le16(vmvf_and_timeout);

	/* flush pipe on time out */
	cmd->cmd_type |= ICE_AQC_Q_DIS_CMD_FLUSH_PIPE;
	/* If no queue group info, we are in a reset flow. Issue the AQ */
	if (!qg_list)
		goto do_aq;

	/* set RD bit to indicate that command buffer is provided by the driver
	 * and it needs to be read by the firmware
	 */
	desc.flags |= cpu_to_le16(LIBIE_AQ_FLAG_RD);

	for (i = 0, item = qg_list; i < num_qgrps; i++) {
		u16 item_size = struct_size(item, q_id, item->num_qs);

		/* If the num of queues is even, add 2 bytes of padding */
		if ((item->num_qs % 2) == 0)
			item_size += 2;

		sz += item_size;

		item = (struct ice_aqc_dis_txq_item *)((u8 *)item + item_size);
	}

	if (buf_size != sz)
		return -EINVAL;

do_aq:
	status = ice_aq_send_cmd(hw, &desc, qg_list, buf_size, cd);
	if (status) {
		if (!qg_list)
			ice_debug(hw, ICE_DBG_SCHED, "VM%d disable failed %d\n",
				  vmvf_num, hw->adminq.sq_last_status);
		else
			ice_debug(hw, ICE_DBG_SCHED, "disable queue %d failed %d\n",
				  le16_to_cpu(qg_list[0].q_id[0]),
				  hw->adminq.sq_last_status);
	}
	return status;
}

/**
 * ice_aq_cfg_lan_txq
 * @hw: pointer to the hardware structure
 * @buf: buffer for command
 * @buf_size: size of buffer in bytes
 * @num_qs: number of queues being configured
 * @oldport: origination lport
 * @newport: destination lport
 * @cd: pointer to command details structure or NULL
 *
 * Move/Configure LAN Tx queue (0x0C32)
 *
 * There is a better AQ command to use for moving nodes, so only coding
 * this one for configuring the node.
 */
int
ice_aq_cfg_lan_txq(struct ice_hw *hw, struct ice_aqc_cfg_txqs_buf *buf,
		   u16 buf_size, u16 num_qs, u8 oldport, u8 newport,
		   struct ice_sq_cd *cd)
{
	struct ice_aqc_cfg_txqs *cmd;
	struct libie_aq_desc desc;
	int status;

	cmd = libie_aq_raw(&desc);
	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_cfg_txqs);
	desc.flags |= cpu_to_le16(LIBIE_AQ_FLAG_RD);

	if (!buf)
		return -EINVAL;

	cmd->cmd_type = ICE_AQC_Q_CFG_TC_CHNG;
	cmd->num_qs = num_qs;
	cmd->port_num_chng = (oldport & ICE_AQC_Q_CFG_SRC_PRT_M);
	cmd->port_num_chng |= FIELD_PREP(ICE_AQC_Q_CFG_DST_PRT_M, newport);
	cmd->time_out = FIELD_PREP(ICE_AQC_Q_CFG_TIMEOUT_M, 5);
	cmd->blocked_cgds = 0;

	status = ice_aq_send_cmd(hw, &desc, buf, buf_size, cd);
	if (status)
		ice_debug(hw, ICE_DBG_SCHED, "Failed to reconfigure nodes %d\n",
			  hw->adminq.sq_last_status);
	return status;
}

/**
 * ice_aq_add_rdma_qsets
 * @hw: pointer to the hardware structure
 * @num_qset_grps: Number of RDMA Qset groups
 * @qset_list: list of Qset groups to be added
 * @buf_size: size of buffer for indirect command
 * @cd: pointer to command details structure or NULL
 *
 * Add Tx RDMA Qsets (0x0C33)
 */
static int
ice_aq_add_rdma_qsets(struct ice_hw *hw, u8 num_qset_grps,
		      struct ice_aqc_add_rdma_qset_data *qset_list,
		      u16 buf_size, struct ice_sq_cd *cd)
{
	struct ice_aqc_add_rdma_qset_data *list;
	struct ice_aqc_add_rdma_qset *cmd;
	struct libie_aq_desc desc;
	u16 i, sum_size = 0;

	cmd = libie_aq_raw(&desc);

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_add_rdma_qset);

	if (num_qset_grps > ICE_LAN_TXQ_MAX_QGRPS)
		return -EINVAL;

	for (i = 0, list = qset_list; i < num_qset_grps; i++) {
		u16 num_qsets = le16_to_cpu(list->num_qsets);

		sum_size += struct_size(list, rdma_qsets, num_qsets);
		list = (struct ice_aqc_add_rdma_qset_data *)(list->rdma_qsets +
							     num_qsets);
	}

	if (buf_size != sum_size)
		return -EINVAL;

	desc.flags |= cpu_to_le16(LIBIE_AQ_FLAG_RD);

	cmd->num_qset_grps = num_qset_grps;

	return ice_aq_send_cmd(hw, &desc, qset_list, buf_size, cd);
}

/* End of FW Admin Queue command wrappers */

/**
 * ice_get_lan_q_ctx - get the LAN queue context for the given VSI and TC
 * @hw: pointer to the HW struct
 * @vsi_handle: software VSI handle
 * @tc: TC number
 * @q_handle: software queue handle
 */
struct ice_q_ctx *
ice_get_lan_q_ctx(struct ice_hw *hw, u16 vsi_handle, u8 tc, u16 q_handle)
{
	struct ice_vsi_ctx *vsi;
	struct ice_q_ctx *q_ctx;

	vsi = ice_get_vsi_ctx(hw, vsi_handle);
	if (!vsi)
		return NULL;
	if (q_handle >= vsi->num_lan_q_entries[tc])
		return NULL;
	if (!vsi->lan_q_ctx[tc])
		return NULL;
	q_ctx = vsi->lan_q_ctx[tc];
	return &q_ctx[q_handle];
}

/**
 * ice_ena_vsi_txq
 * @pi: port information structure
 * @vsi_handle: software VSI handle
 * @tc: TC number
 * @q_handle: software queue handle
 * @num_qgrps: Number of added queue groups
 * @buf: list of queue groups to be added
 * @buf_size: size of buffer for indirect command
 * @cd: pointer to command details structure or NULL
 *
 * This function adds one LAN queue
 */
int
ice_ena_vsi_txq(struct ice_port_info *pi, u16 vsi_handle, u8 tc, u16 q_handle,
		u8 num_qgrps, struct ice_aqc_add_tx_qgrp *buf, u16 buf_size,
		struct ice_sq_cd *cd)
{
	struct ice_aqc_txsched_elem_data node = { 0 };
	struct ice_sched_node *parent;
	struct ice_q_ctx *q_ctx;
	struct ice_hw *hw;
	int status;

	if (!pi || pi->port_state != ICE_SCHED_PORT_STATE_READY)
		return -EIO;

	if (num_qgrps > 1 || buf->num_txqs > 1)
		return -ENOSPC;

	hw = pi->hw;

	if (!ice_is_vsi_valid(hw, vsi_handle))
		return -EINVAL;

	mutex_lock(&pi->sched_lock);

	q_ctx = ice_get_lan_q_ctx(hw, vsi_handle, tc, q_handle);
	if (!q_ctx) {
		ice_debug(hw, ICE_DBG_SCHED, "Enaq: invalid queue handle %d\n",
			  q_handle);
		status = -EINVAL;
		goto ena_txq_exit;
	}

	/* find a parent node */
	parent = ice_sched_get_free_qparent(pi, vsi_handle, tc,
					    ICE_SCHED_NODE_OWNER_LAN);
	if (!parent) {
		status = -EINVAL;
		goto ena_txq_exit;
	}

	buf->parent_teid = parent->info.node_teid;
	node.parent_teid = parent->info.node_teid;
	/* Mark that the values in the "generic" section as valid. The default
	 * value in the "generic" section is zero. This means that :
	 * - Scheduling mode is Bytes Per Second (BPS), indicated by Bit 0.
	 * - 0 priority among siblings, indicated by Bit 1-3.
	 * - WFQ, indicated by Bit 4.
	 * - 0 Adjustment value is used in PSM credit update flow, indicated by
	 * Bit 5-6.
	 * - Bit 7 is reserved.
	 * Without setting the generic section as valid in valid_sections, the
	 * Admin queue command will fail with error code ICE_AQ_RC_EINVAL.
	 */
	buf->txqs[0].info.valid_sections =
		ICE_AQC_ELEM_VALID_GENERIC | ICE_AQC_ELEM_VALID_CIR |
		ICE_AQC_ELEM_VALID_EIR;
	buf->txqs[0].info.generic = 0;
	buf->txqs[0].info.cir_bw.bw_profile_idx =
		cpu_to_le16(ICE_SCHED_DFLT_RL_PROF_ID);
	buf->txqs[0].info.cir_bw.bw_alloc =
		cpu_to_le16(ICE_SCHED_DFLT_BW_WT);
	buf->txqs[0].info.eir_bw.bw_profile_idx =
		cpu_to_le16(ICE_SCHED_DFLT_RL_PROF_ID);
	buf->txqs[0].info.eir_bw.bw_alloc =
		cpu_to_le16(ICE_SCHED_DFLT_BW_WT);

	/* add the LAN queue */
	status = ice_aq_add_lan_txq(hw, num_qgrps, buf, buf_size, cd);
	if (status) {
		ice_debug(hw, ICE_DBG_SCHED, "enable queue %d failed %d\n",
			  le16_to_cpu(buf->txqs[0].txq_id),
			  hw->adminq.sq_last_status);
		goto ena_txq_exit;
	}

	node.node_teid = buf->txqs[0].q_teid;
	node.data.elem_type = ICE_AQC_ELEM_TYPE_LEAF;
	q_ctx->q_handle = q_handle;
	q_ctx->q_teid = le32_to_cpu(node.node_teid);

	/* add a leaf node into scheduler tree queue layer */
	status = ice_sched_add_node(pi, hw->num_tx_sched_layers - 1, &node, NULL);
	if (!status)
		status = ice_sched_replay_q_bw(pi, q_ctx);

ena_txq_exit:
	mutex_unlock(&pi->sched_lock);
	return status;
}

/**
 * ice_dis_vsi_txq
 * @pi: port information structure
 * @vsi_handle: software VSI handle
 * @tc: TC number
 * @num_queues: number of queues
 * @q_handles: pointer to software queue handle array
 * @q_ids: pointer to the q_id array
 * @q_teids: pointer to queue node teids
 * @rst_src: if called due to reset, specifies the reset source
 * @vmvf_num: the relative VM or VF number that is undergoing the reset
 * @cd: pointer to command details structure or NULL
 *
 * This function removes queues and their corresponding nodes in SW DB
 */
int
ice_dis_vsi_txq(struct ice_port_info *pi, u16 vsi_handle, u8 tc, u8 num_queues,
		u16 *q_handles, u16 *q_ids, u32 *q_teids,
		enum ice_disq_rst_src rst_src, u16 vmvf_num,
		struct ice_sq_cd *cd)
{
	DEFINE_RAW_FLEX(struct ice_aqc_dis_txq_item, qg_list, q_id, 1);
	u16 i, buf_size = __struct_size(qg_list);
	struct ice_q_ctx *q_ctx;
	int status = -ENOENT;
	struct ice_hw *hw;

	if (!pi || pi->port_state != ICE_SCHED_PORT_STATE_READY)
		return -EIO;

	hw = pi->hw;

	if (!num_queues) {
		/* if queue is disabled already yet the disable queue command
		 * has to be sent to complete the VF reset, then call
		 * ice_aq_dis_lan_txq without any queue information
		 */
		if (rst_src)
			return ice_aq_dis_lan_txq(hw, 0, NULL, 0, rst_src,
						  vmvf_num, NULL);
		return -EIO;
	}

	mutex_lock(&pi->sched_lock);

	for (i = 0; i < num_queues; i++) {
		struct ice_sched_node *node;

		node = ice_sched_find_node_by_teid(pi->root, q_teids[i]);
		if (!node)
			continue;
		q_ctx = ice_get_lan_q_ctx(hw, vsi_handle, tc, q_handles[i]);
		if (!q_ctx) {
			ice_debug(hw, ICE_DBG_SCHED, "invalid queue handle%d\n",
				  q_handles[i]);
			continue;
		}
		if (q_ctx->q_handle != q_handles[i]) {
			ice_debug(hw, ICE_DBG_SCHED, "Err:handles %d %d\n",
				  q_ctx->q_handle, q_handles[i]);
			continue;
		}
		qg_list->parent_teid = node->info.parent_teid;
		qg_list->num_qs = 1;
		qg_list->q_id[0] = cpu_to_le16(q_ids[i]);
		status = ice_aq_dis_lan_txq(hw, 1, qg_list, buf_size, rst_src,
					    vmvf_num, cd);

		if (status)
			break;
		ice_free_sched_node(pi, node);
		q_ctx->q_handle = ICE_INVAL_Q_HANDLE;
		q_ctx->q_teid = ICE_INVAL_TEID;
	}
	mutex_unlock(&pi->sched_lock);
	return status;
}

/**
 * ice_cfg_vsi_qs - configure the new/existing VSI queues
 * @pi: port information structure
 * @vsi_handle: software VSI handle
 * @tc_bitmap: TC bitmap
 * @maxqs: max queues array per TC
 * @owner: LAN or RDMA
 *
 * This function adds/updates the VSI queues per TC.
 */
static int
ice_cfg_vsi_qs(struct ice_port_info *pi, u16 vsi_handle, u8 tc_bitmap,
	       u16 *maxqs, u8 owner)
{
	int status = 0;
	u8 i;

	if (!pi || pi->port_state != ICE_SCHED_PORT_STATE_READY)
		return -EIO;

	if (!ice_is_vsi_valid(pi->hw, vsi_handle))
		return -EINVAL;

	mutex_lock(&pi->sched_lock);

	ice_for_each_traffic_class(i) {
		/* configuration is possible only if TC node is present */
		if (!ice_sched_get_tc_node(pi, i))
			continue;

		status = ice_sched_cfg_vsi(pi, vsi_handle, i, maxqs[i], owner,
					   ice_is_tc_ena(tc_bitmap, i));
		if (status)
			break;
	}

	mutex_unlock(&pi->sched_lock);
	return status;
}

/**
 * ice_cfg_vsi_lan - configure VSI LAN queues
 * @pi: port information structure
 * @vsi_handle: software VSI handle
 * @tc_bitmap: TC bitmap
 * @max_lanqs: max LAN queues array per TC
 *
 * This function adds/updates the VSI LAN queues per TC.
 */
int
ice_cfg_vsi_lan(struct ice_port_info *pi, u16 vsi_handle, u8 tc_bitmap,
		u16 *max_lanqs)
{
	return ice_cfg_vsi_qs(pi, vsi_handle, tc_bitmap, max_lanqs,
			      ICE_SCHED_NODE_OWNER_LAN);
}

/**
 * ice_cfg_vsi_rdma - configure the VSI RDMA queues
 * @pi: port information structure
 * @vsi_handle: software VSI handle
 * @tc_bitmap: TC bitmap
 * @max_rdmaqs: max RDMA queues array per TC
 *
 * This function adds/updates the VSI RDMA queues per TC.
 */
int
ice_cfg_vsi_rdma(struct ice_port_info *pi, u16 vsi_handle, u16 tc_bitmap,
		 u16 *max_rdmaqs)
{
	return ice_cfg_vsi_qs(pi, vsi_handle, tc_bitmap, max_rdmaqs,
			      ICE_SCHED_NODE_OWNER_RDMA);
}

/**
 * ice_ena_vsi_rdma_qset
 * @pi: port information structure
 * @vsi_handle: software VSI handle
 * @tc: TC number
 * @rdma_qset: pointer to RDMA Qset
 * @num_qsets: number of RDMA Qsets
 * @qset_teid: pointer to Qset node TEIDs
 *
 * This function adds RDMA Qset
 */
int
ice_ena_vsi_rdma_qset(struct ice_port_info *pi, u16 vsi_handle, u8 tc,
		      u16 *rdma_qset, u16 num_qsets, u32 *qset_teid)
{
	struct ice_aqc_txsched_elem_data node = { 0 };
	struct ice_aqc_add_rdma_qset_data *buf;
	struct ice_sched_node *parent;
	struct ice_hw *hw;
	u16 i, buf_size;
	int ret;

	if (!pi || pi->port_state != ICE_SCHED_PORT_STATE_READY)
		return -EIO;
	hw = pi->hw;

	if (!ice_is_vsi_valid(hw, vsi_handle))
		return -EINVAL;

	buf_size = struct_size(buf, rdma_qsets, num_qsets);
	buf = kzalloc(buf_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	mutex_lock(&pi->sched_lock);

	parent = ice_sched_get_free_qparent(pi, vsi_handle, tc,
					    ICE_SCHED_NODE_OWNER_RDMA);
	if (!parent) {
		ret = -EINVAL;
		goto rdma_error_exit;
	}
	buf->parent_teid = parent->info.node_teid;
	node.parent_teid = parent->info.node_teid;

	buf->num_qsets = cpu_to_le16(num_qsets);
	for (i = 0; i < num_qsets; i++) {
		buf->rdma_qsets[i].tx_qset_id = cpu_to_le16(rdma_qset[i]);
		buf->rdma_qsets[i].info.valid_sections =
			ICE_AQC_ELEM_VALID_GENERIC | ICE_AQC_ELEM_VALID_CIR |
			ICE_AQC_ELEM_VALID_EIR;
		buf->rdma_qsets[i].info.generic = 0;
		buf->rdma_qsets[i].info.cir_bw.bw_profile_idx =
			cpu_to_le16(ICE_SCHED_DFLT_RL_PROF_ID);
		buf->rdma_qsets[i].info.cir_bw.bw_alloc =
			cpu_to_le16(ICE_SCHED_DFLT_BW_WT);
		buf->rdma_qsets[i].info.eir_bw.bw_profile_idx =
			cpu_to_le16(ICE_SCHED_DFLT_RL_PROF_ID);
		buf->rdma_qsets[i].info.eir_bw.bw_alloc =
			cpu_to_le16(ICE_SCHED_DFLT_BW_WT);
	}
	ret = ice_aq_add_rdma_qsets(hw, 1, buf, buf_size, NULL);
	if (ret) {
		ice_debug(hw, ICE_DBG_RDMA, "add RDMA qset failed\n");
		goto rdma_error_exit;
	}
	node.data.elem_type = ICE_AQC_ELEM_TYPE_LEAF;
	for (i = 0; i < num_qsets; i++) {
		node.node_teid = buf->rdma_qsets[i].qset_teid;
		ret = ice_sched_add_node(pi, hw->num_tx_sched_layers - 1,
					 &node, NULL);
		if (ret)
			break;
		qset_teid[i] = le32_to_cpu(node.node_teid);
	}
rdma_error_exit:
	mutex_unlock(&pi->sched_lock);
	kfree(buf);
	return ret;
}

/**
 * ice_dis_vsi_rdma_qset - free RDMA resources
 * @pi: port_info struct
 * @count: number of RDMA Qsets to free
 * @qset_teid: TEID of Qset node
 * @q_id: list of queue IDs being disabled
 */
int
ice_dis_vsi_rdma_qset(struct ice_port_info *pi, u16 count, u32 *qset_teid,
		      u16 *q_id)
{
	DEFINE_RAW_FLEX(struct ice_aqc_dis_txq_item, qg_list, q_id, 1);
	u16 qg_size = __struct_size(qg_list);
	struct ice_hw *hw;
	int status = 0;
	int i;

	if (!pi || pi->port_state != ICE_SCHED_PORT_STATE_READY)
		return -EIO;

	hw = pi->hw;

	mutex_lock(&pi->sched_lock);

	for (i = 0; i < count; i++) {
		struct ice_sched_node *node;

		node = ice_sched_find_node_by_teid(pi->root, qset_teid[i]);
		if (!node)
			continue;

		qg_list->parent_teid = node->info.parent_teid;
		qg_list->num_qs = 1;
		qg_list->q_id[0] =
			cpu_to_le16(q_id[i] |
				    ICE_AQC_Q_DIS_BUF_ELEM_TYPE_RDMA_QSET);

		status = ice_aq_dis_lan_txq(hw, 1, qg_list, qg_size,
					    ICE_NO_RESET, 0, NULL);
		if (status)
			break;

		ice_free_sched_node(pi, node);
	}

	mutex_unlock(&pi->sched_lock);
	return status;
}

/**
 * ice_aq_get_cgu_input_pin_measure - get input pin signal measurements
 * @hw: pointer to the HW struct
 * @dpll_idx: index of dpll to be measured
 * @meas: array to be filled with results
 * @meas_num: max number of results array can hold
 *
 * Get CGU measurements (0x0C59) of phase and frequency offsets for input
 * pins on given dpll.
 *
 * Return: 0 on success or negative value on failure.
 */
int ice_aq_get_cgu_input_pin_measure(struct ice_hw *hw, u8 dpll_idx,
				     struct ice_cgu_input_measure *meas,
				     u16 meas_num)
{
	struct ice_aqc_get_cgu_input_measure *cmd;
	struct libie_aq_desc desc;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_get_cgu_input_measure);
	cmd = libie_aq_raw(&desc);
	cmd->dpll_idx_opt = dpll_idx & ICE_AQC_GET_CGU_IN_MEAS_DPLL_IDX_M;

	return ice_aq_send_cmd(hw, &desc, meas, meas_num * sizeof(*meas), NULL);
}

/**
 * ice_aq_get_cgu_abilities - get cgu abilities
 * @hw: pointer to the HW struct
 * @abilities: CGU abilities
 *
 * Get CGU abilities (0x0C61)
 * Return: 0 on success or negative value on failure.
 */
int
ice_aq_get_cgu_abilities(struct ice_hw *hw,
			 struct ice_aqc_get_cgu_abilities *abilities)
{
	struct libie_aq_desc desc;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_get_cgu_abilities);
	return ice_aq_send_cmd(hw, &desc, abilities, sizeof(*abilities), NULL);
}

/**
 * ice_aq_set_input_pin_cfg - set input pin config
 * @hw: pointer to the HW struct
 * @input_idx: Input index
 * @flags1: Input flags
 * @flags2: Input flags
 * @freq: Frequency in Hz
 * @phase_delay: Delay in ps
 *
 * Set CGU input config (0x0C62)
 * Return: 0 on success or negative value on failure.
 */
int
ice_aq_set_input_pin_cfg(struct ice_hw *hw, u8 input_idx, u8 flags1, u8 flags2,
			 u32 freq, s32 phase_delay)
{
	struct ice_aqc_set_cgu_input_config *cmd;
	struct libie_aq_desc desc;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_set_cgu_input_config);
	cmd = libie_aq_raw(&desc);
	cmd->input_idx = input_idx;
	cmd->flags1 = flags1;
	cmd->flags2 = flags2;
	cmd->freq = cpu_to_le32(freq);
	cmd->phase_delay = cpu_to_le32(phase_delay);

	return ice_aq_send_cmd(hw, &desc, NULL, 0, NULL);
}

/**
 * ice_aq_get_input_pin_cfg - get input pin config
 * @hw: pointer to the HW struct
 * @input_idx: Input index
 * @status: Pin status
 * @type: Pin type
 * @flags1: Input flags
 * @flags2: Input flags
 * @freq: Frequency in Hz
 * @phase_delay: Delay in ps
 *
 * Get CGU input config (0x0C63)
 * Return: 0 on success or negative value on failure.
 */
int
ice_aq_get_input_pin_cfg(struct ice_hw *hw, u8 input_idx, u8 *status, u8 *type,
			 u8 *flags1, u8 *flags2, u32 *freq, s32 *phase_delay)
{
	struct ice_aqc_get_cgu_input_config *cmd;
	struct libie_aq_desc desc;
	int ret;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_get_cgu_input_config);
	cmd = libie_aq_raw(&desc);
	cmd->input_idx = input_idx;

	ret = ice_aq_send_cmd(hw, &desc, NULL, 0, NULL);
	if (!ret) {
		if (status)
			*status = cmd->status;
		if (type)
			*type = cmd->type;
		if (flags1)
			*flags1 = cmd->flags1;
		if (flags2)
			*flags2 = cmd->flags2;
		if (freq)
			*freq = le32_to_cpu(cmd->freq);
		if (phase_delay)
			*phase_delay = le32_to_cpu(cmd->phase_delay);
	}

	return ret;
}

/**
 * ice_aq_set_output_pin_cfg - set output pin config
 * @hw: pointer to the HW struct
 * @output_idx: Output index
 * @flags: Output flags
 * @src_sel: Index of DPLL block
 * @freq: Output frequency
 * @phase_delay: Output phase compensation
 *
 * Set CGU output config (0x0C64)
 * Return: 0 on success or negative value on failure.
 */
int
ice_aq_set_output_pin_cfg(struct ice_hw *hw, u8 output_idx, u8 flags,
			  u8 src_sel, u32 freq, s32 phase_delay)
{
	struct ice_aqc_set_cgu_output_config *cmd;
	struct libie_aq_desc desc;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_set_cgu_output_config);
	cmd = libie_aq_raw(&desc);
	cmd->output_idx = output_idx;
	cmd->flags = flags;
	cmd->src_sel = src_sel;
	cmd->freq = cpu_to_le32(freq);
	cmd->phase_delay = cpu_to_le32(phase_delay);

	return ice_aq_send_cmd(hw, &desc, NULL, 0, NULL);
}

/**
 * ice_aq_get_output_pin_cfg - get output pin config
 * @hw: pointer to the HW struct
 * @output_idx: Output index
 * @flags: Output flags
 * @src_sel: Internal DPLL source
 * @freq: Output frequency
 * @src_freq: Source frequency
 *
 * Get CGU output config (0x0C65)
 * Return: 0 on success or negative value on failure.
 */
int
ice_aq_get_output_pin_cfg(struct ice_hw *hw, u8 output_idx, u8 *flags,
			  u8 *src_sel, u32 *freq, u32 *src_freq)
{
	struct ice_aqc_get_cgu_output_config *cmd;
	struct libie_aq_desc desc;
	int ret;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_get_cgu_output_config);
	cmd = libie_aq_raw(&desc);
	cmd->output_idx = output_idx;

	ret = ice_aq_send_cmd(hw, &desc, NULL, 0, NULL);
	if (!ret) {
		if (flags)
			*flags = cmd->flags;
		if (src_sel)
			*src_sel = cmd->src_sel;
		if (freq)
			*freq = le32_to_cpu(cmd->freq);
		if (src_freq)
			*src_freq = le32_to_cpu(cmd->src_freq);
	}

	return ret;
}

/**
 * ice_aq_get_cgu_dpll_status - get dpll status
 * @hw: pointer to the HW struct
 * @dpll_num: DPLL index
 * @ref_state: Reference clock state
 * @config: current DPLL config
 * @dpll_state: current DPLL state
 * @phase_offset: Phase offset in ns
 * @eec_mode: EEC_mode
 *
 * Get CGU DPLL status (0x0C66)
 * Return: 0 on success or negative value on failure.
 */
int
ice_aq_get_cgu_dpll_status(struct ice_hw *hw, u8 dpll_num, u8 *ref_state,
			   u8 *dpll_state, u8 *config, s64 *phase_offset,
			   u8 *eec_mode)
{
	struct ice_aqc_get_cgu_dpll_status *cmd;
	struct libie_aq_desc desc;
	int status;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_get_cgu_dpll_status);
	cmd = libie_aq_raw(&desc);
	cmd->dpll_num = dpll_num;

	status = ice_aq_send_cmd(hw, &desc, NULL, 0, NULL);
	if (!status) {
		*ref_state = cmd->ref_state;
		*dpll_state = cmd->dpll_state;
		*config = cmd->config;
		*phase_offset = le32_to_cpu(cmd->phase_offset_h);
		*phase_offset <<= 32;
		*phase_offset += le32_to_cpu(cmd->phase_offset_l);
		*phase_offset = sign_extend64(*phase_offset, 47);
		*eec_mode = cmd->eec_mode;
	}

	return status;
}

/**
 * ice_aq_set_cgu_dpll_config - set dpll config
 * @hw: pointer to the HW struct
 * @dpll_num: DPLL index
 * @ref_state: Reference clock state
 * @config: DPLL config
 * @eec_mode: EEC mode
 *
 * Set CGU DPLL config (0x0C67)
 * Return: 0 on success or negative value on failure.
 */
int
ice_aq_set_cgu_dpll_config(struct ice_hw *hw, u8 dpll_num, u8 ref_state,
			   u8 config, u8 eec_mode)
{
	struct ice_aqc_set_cgu_dpll_config *cmd;
	struct libie_aq_desc desc;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_set_cgu_dpll_config);
	cmd = libie_aq_raw(&desc);
	cmd->dpll_num = dpll_num;
	cmd->ref_state = ref_state;
	cmd->config = config;
	cmd->eec_mode = eec_mode;

	return ice_aq_send_cmd(hw, &desc, NULL, 0, NULL);
}

/**
 * ice_aq_set_cgu_ref_prio - set input reference priority
 * @hw: pointer to the HW struct
 * @dpll_num: DPLL index
 * @ref_idx: Reference pin index
 * @ref_priority: Reference input priority
 *
 * Set CGU reference priority (0x0C68)
 * Return: 0 on success or negative value on failure.
 */
int
ice_aq_set_cgu_ref_prio(struct ice_hw *hw, u8 dpll_num, u8 ref_idx,
			u8 ref_priority)
{
	struct ice_aqc_set_cgu_ref_prio *cmd;
	struct libie_aq_desc desc;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_set_cgu_ref_prio);
	cmd = libie_aq_raw(&desc);
	cmd->dpll_num = dpll_num;
	cmd->ref_idx = ref_idx;
	cmd->ref_priority = ref_priority;

	return ice_aq_send_cmd(hw, &desc, NULL, 0, NULL);
}

/**
 * ice_aq_get_cgu_ref_prio - get input reference priority
 * @hw: pointer to the HW struct
 * @dpll_num: DPLL index
 * @ref_idx: Reference pin index
 * @ref_prio: Reference input priority
 *
 * Get CGU reference priority (0x0C69)
 * Return: 0 on success or negative value on failure.
 */
int
ice_aq_get_cgu_ref_prio(struct ice_hw *hw, u8 dpll_num, u8 ref_idx,
			u8 *ref_prio)
{
	struct ice_aqc_get_cgu_ref_prio *cmd;
	struct libie_aq_desc desc;
	int status;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_get_cgu_ref_prio);
	cmd = libie_aq_raw(&desc);
	cmd->dpll_num = dpll_num;
	cmd->ref_idx = ref_idx;

	status = ice_aq_send_cmd(hw, &desc, NULL, 0, NULL);
	if (!status)
		*ref_prio = cmd->ref_priority;

	return status;
}

/**
 * ice_aq_get_cgu_info - get cgu info
 * @hw: pointer to the HW struct
 * @cgu_id: CGU ID
 * @cgu_cfg_ver: CGU config version
 * @cgu_fw_ver: CGU firmware version
 *
 * Get CGU info (0x0C6A)
 * Return: 0 on success or negative value on failure.
 */
int
ice_aq_get_cgu_info(struct ice_hw *hw, u32 *cgu_id, u32 *cgu_cfg_ver,
		    u32 *cgu_fw_ver)
{
	struct ice_aqc_get_cgu_info *cmd;
	struct libie_aq_desc desc;
	int status;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_get_cgu_info);
	cmd = libie_aq_raw(&desc);

	status = ice_aq_send_cmd(hw, &desc, NULL, 0, NULL);
	if (!status) {
		*cgu_id = le32_to_cpu(cmd->cgu_id);
		*cgu_cfg_ver = le32_to_cpu(cmd->cgu_cfg_ver);
		*cgu_fw_ver = le32_to_cpu(cmd->cgu_fw_ver);
	}

	return status;
}

/**
 * ice_aq_set_phy_rec_clk_out - set RCLK phy out
 * @hw: pointer to the HW struct
 * @phy_output: PHY reference clock output pin
 * @enable: GPIO state to be applied
 * @freq: PHY output frequency
 *
 * Set phy recovered clock as reference (0x0630)
 * Return: 0 on success or negative value on failure.
 */
int
ice_aq_set_phy_rec_clk_out(struct ice_hw *hw, u8 phy_output, bool enable,
			   u32 *freq)
{
	struct ice_aqc_set_phy_rec_clk_out *cmd;
	struct libie_aq_desc desc;
	int status;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_set_phy_rec_clk_out);
	cmd = libie_aq_raw(&desc);
	cmd->phy_output = phy_output;
	cmd->port_num = ICE_AQC_SET_PHY_REC_CLK_OUT_CURR_PORT;
	cmd->flags = enable & ICE_AQC_SET_PHY_REC_CLK_OUT_OUT_EN;
	cmd->freq = cpu_to_le32(*freq);

	status = ice_aq_send_cmd(hw, &desc, NULL, 0, NULL);
	if (!status)
		*freq = le32_to_cpu(cmd->freq);

	return status;
}

/**
 * ice_aq_get_phy_rec_clk_out - get phy recovered signal info
 * @hw: pointer to the HW struct
 * @phy_output: PHY reference clock output pin
 * @port_num: Port number
 * @flags: PHY flags
 * @node_handle: PHY output frequency
 *
 * Get PHY recovered clock output info (0x0631)
 * Return: 0 on success or negative value on failure.
 */
int
ice_aq_get_phy_rec_clk_out(struct ice_hw *hw, u8 *phy_output, u8 *port_num,
			   u8 *flags, u16 *node_handle)
{
	struct ice_aqc_get_phy_rec_clk_out *cmd;
	struct libie_aq_desc desc;
	int status;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_get_phy_rec_clk_out);
	cmd = libie_aq_raw(&desc);
	cmd->phy_output = *phy_output;

	status = ice_aq_send_cmd(hw, &desc, NULL, 0, NULL);
	if (!status) {
		*phy_output = cmd->phy_output;
		if (port_num)
			*port_num = cmd->port_num;
		if (flags)
			*flags = cmd->flags;
		if (node_handle)
			*node_handle = le16_to_cpu(cmd->node_handle);
	}

	return status;
}

/**
 * ice_aq_get_sensor_reading
 * @hw: pointer to the HW struct
 * @data: pointer to data to be read from the sensor
 *
 * Get sensor reading (0x0632)
 */
int ice_aq_get_sensor_reading(struct ice_hw *hw,
			      struct ice_aqc_get_sensor_reading_resp *data)
{
	struct ice_aqc_get_sensor_reading *cmd;
	struct libie_aq_desc desc;
	int status;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_get_sensor_reading);
	cmd = libie_aq_raw(&desc);
#define ICE_INTERNAL_TEMP_SENSOR_FORMAT	0
#define ICE_INTERNAL_TEMP_SENSOR	0
	cmd->sensor = ICE_INTERNAL_TEMP_SENSOR;
	cmd->format = ICE_INTERNAL_TEMP_SENSOR_FORMAT;

	status = ice_aq_send_cmd(hw, &desc, NULL, 0, NULL);
	if (!status)
		memcpy(data, &desc.params.raw,
		       sizeof(*data));

	return status;
}

/**
 * ice_replay_pre_init - replay pre initialization
 * @hw: pointer to the HW struct
 *
 * Initializes required config data for VSI, FD, ACL, and RSS before replay.
 */
static int ice_replay_pre_init(struct ice_hw *hw)
{
	struct ice_switch_info *sw = hw->switch_info;
	u8 i;

	/* Delete old entries from replay filter list head if there is any */
	ice_rm_all_sw_replay_rule_info(hw);
	/* In start of replay, move entries into replay_rules list, it
	 * will allow adding rules entries back to filt_rules list,
	 * which is operational list.
	 */
	for (i = 0; i < ICE_MAX_NUM_RECIPES; i++)
		list_replace_init(&sw->recp_list[i].filt_rules,
				  &sw->recp_list[i].filt_replay_rules);
	ice_sched_replay_agg_vsi_preinit(hw);

	return 0;
}

/**
 * ice_replay_vsi - replay VSI configuration
 * @hw: pointer to the HW struct
 * @vsi_handle: driver VSI handle
 *
 * Restore all VSI configuration after reset. It is required to call this
 * function with main VSI first.
 */
int ice_replay_vsi(struct ice_hw *hw, u16 vsi_handle)
{
	int status;

	if (!ice_is_vsi_valid(hw, vsi_handle))
		return -EINVAL;

	/* Replay pre-initialization if there is any */
	if (vsi_handle == ICE_MAIN_VSI_HANDLE) {
		status = ice_replay_pre_init(hw);
		if (status)
			return status;
	}
	/* Replay per VSI all RSS configurations */
	status = ice_replay_rss_cfg(hw, vsi_handle);
	if (status)
		return status;
	/* Replay per VSI all filters */
	status = ice_replay_vsi_all_fltr(hw, vsi_handle);
	if (!status)
		status = ice_replay_vsi_agg(hw, vsi_handle);
	return status;
}

/**
 * ice_replay_post - post replay configuration cleanup
 * @hw: pointer to the HW struct
 *
 * Post replay cleanup.
 */
void ice_replay_post(struct ice_hw *hw)
{
	/* Delete old entries from replay filter list head */
	ice_rm_all_sw_replay_rule_info(hw);
	ice_sched_replay_agg(hw);
}

/**
 * ice_stat_update40 - read 40 bit stat from the chip and update stat values
 * @hw: ptr to the hardware info
 * @reg: offset of 64 bit HW register to read from
 * @prev_stat_loaded: bool to specify if previous stats are loaded
 * @prev_stat: ptr to previous loaded stat value
 * @cur_stat: ptr to current stat value
 */
void
ice_stat_update40(struct ice_hw *hw, u32 reg, bool prev_stat_loaded,
		  u64 *prev_stat, u64 *cur_stat)
{
	u64 new_data = rd64(hw, reg) & (BIT_ULL(40) - 1);

	/* device stats are not reset at PFR, they likely will not be zeroed
	 * when the driver starts. Thus, save the value from the first read
	 * without adding to the statistic value so that we report stats which
	 * count up from zero.
	 */
	if (!prev_stat_loaded) {
		*prev_stat = new_data;
		return;
	}

	/* Calculate the difference between the new and old values, and then
	 * add it to the software stat value.
	 */
	if (new_data >= *prev_stat)
		*cur_stat += new_data - *prev_stat;
	else
		/* to manage the potential roll-over */
		*cur_stat += (new_data + BIT_ULL(40)) - *prev_stat;

	/* Update the previously stored value to prepare for next read */
	*prev_stat = new_data;
}

/**
 * ice_stat_update32 - read 32 bit stat from the chip and update stat values
 * @hw: ptr to the hardware info
 * @reg: offset of HW register to read from
 * @prev_stat_loaded: bool to specify if previous stats are loaded
 * @prev_stat: ptr to previous loaded stat value
 * @cur_stat: ptr to current stat value
 */
void
ice_stat_update32(struct ice_hw *hw, u32 reg, bool prev_stat_loaded,
		  u64 *prev_stat, u64 *cur_stat)
{
	u32 new_data;

	new_data = rd32(hw, reg);

	/* device stats are not reset at PFR, they likely will not be zeroed
	 * when the driver starts. Thus, save the value from the first read
	 * without adding to the statistic value so that we report stats which
	 * count up from zero.
	 */
	if (!prev_stat_loaded) {
		*prev_stat = new_data;
		return;
	}

	/* Calculate the difference between the new and old values, and then
	 * add it to the software stat value.
	 */
	if (new_data >= *prev_stat)
		*cur_stat += new_data - *prev_stat;
	else
		/* to manage the potential roll-over */
		*cur_stat += (new_data + BIT_ULL(32)) - *prev_stat;

	/* Update the previously stored value to prepare for next read */
	*prev_stat = new_data;
}

/**
 * ice_sched_query_elem - query element information from HW
 * @hw: pointer to the HW struct
 * @node_teid: node TEID to be queried
 * @buf: buffer to element information
 *
 * This function queries HW element information
 */
int
ice_sched_query_elem(struct ice_hw *hw, u32 node_teid,
		     struct ice_aqc_txsched_elem_data *buf)
{
	u16 buf_size, num_elem_ret = 0;
	int status;

	buf_size = sizeof(*buf);
	memset(buf, 0, buf_size);
	buf->node_teid = cpu_to_le32(node_teid);
	status = ice_aq_query_sched_elems(hw, 1, buf, buf_size, &num_elem_ret,
					  NULL);
	if (status || num_elem_ret != 1)
		ice_debug(hw, ICE_DBG_SCHED, "query element failed\n");
	return status;
}

/**
 * ice_aq_read_i2c
 * @hw: pointer to the hw struct
 * @topo_addr: topology address for a device to communicate with
 * @bus_addr: 7-bit I2C bus address
 * @addr: I2C memory address (I2C offset) with up to 16 bits
 * @params: I2C parameters: bit [7] - Repeated start,
 *			    bits [6:5] data offset size,
 *			    bit [4] - I2C address type,
 *			    bits [3:0] - data size to read (0-16 bytes)
 * @data: pointer to data (0 to 16 bytes) to be read from the I2C device
 * @cd: pointer to command details structure or NULL
 *
 * Read I2C (0x06E2)
 */
int
ice_aq_read_i2c(struct ice_hw *hw, struct ice_aqc_link_topo_addr topo_addr,
		u16 bus_addr, __le16 addr, u8 params, u8 *data,
		struct ice_sq_cd *cd)
{
	struct libie_aq_desc desc = { 0 };
	struct ice_aqc_i2c *cmd;
	u8 data_size;
	int status;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_read_i2c);
	cmd = libie_aq_raw(&desc);

	if (!data)
		return -EINVAL;

	data_size = FIELD_GET(ICE_AQC_I2C_DATA_SIZE_M, params);

	cmd->i2c_bus_addr = cpu_to_le16(bus_addr);
	cmd->topo_addr = topo_addr;
	cmd->i2c_params = params;
	cmd->i2c_addr = addr;

	status = ice_aq_send_cmd(hw, &desc, NULL, 0, cd);
	if (!status) {
		struct ice_aqc_read_i2c_resp *resp;
		u8 i;

		resp = libie_aq_raw(&desc);
		for (i = 0; i < data_size; i++) {
			*data = resp->i2c_data[i];
			data++;
		}
	}

	return status;
}

/**
 * ice_aq_write_i2c
 * @hw: pointer to the hw struct
 * @topo_addr: topology address for a device to communicate with
 * @bus_addr: 7-bit I2C bus address
 * @addr: I2C memory address (I2C offset) with up to 16 bits
 * @params: I2C parameters: bit [4] - I2C address type, bits [3:0] - data size to write (0-7 bytes)
 * @data: pointer to data (0 to 4 bytes) to be written to the I2C device
 * @cd: pointer to command details structure or NULL
 *
 * Write I2C (0x06E3)
 *
 * * Return:
 * * 0             - Successful write to the i2c device
 * * -EINVAL       - Data size greater than 4 bytes
 * * -EIO          - FW error
 */
int
ice_aq_write_i2c(struct ice_hw *hw, struct ice_aqc_link_topo_addr topo_addr,
		 u16 bus_addr, __le16 addr, u8 params, const u8 *data,
		 struct ice_sq_cd *cd)
{
	struct libie_aq_desc desc = { 0 };
	struct ice_aqc_i2c *cmd;
	u8 data_size;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_write_i2c);
	cmd = libie_aq_raw(&desc);

	data_size = FIELD_GET(ICE_AQC_I2C_DATA_SIZE_M, params);

	/* data_size limited to 4 */
	if (data_size > 4)
		return -EINVAL;

	cmd->i2c_bus_addr = cpu_to_le16(bus_addr);
	cmd->topo_addr = topo_addr;
	cmd->i2c_params = params;
	cmd->i2c_addr = addr;

	memcpy(cmd->i2c_data, data, data_size);

	return ice_aq_send_cmd(hw, &desc, NULL, 0, cd);
}

/**
 * ice_get_pca9575_handle - find and return the PCA9575 controller
 * @hw: pointer to the hw struct
 * @pca9575_handle: GPIO controller's handle
 *
 * Find and return the GPIO controller's handle in the netlist.
 * When found - the value will be cached in the hw structure and following calls
 * will return cached value.
 *
 * Return: 0 on success, -ENXIO when there's no PCA9575 present.
 */
int ice_get_pca9575_handle(struct ice_hw *hw, u16 *pca9575_handle)
{
	struct ice_aqc_get_link_topo *cmd;
	struct libie_aq_desc desc;
	int err;
	u8 idx;

	/* If handle was read previously return cached value */
	if (hw->io_expander_handle) {
		*pca9575_handle = hw->io_expander_handle;
		return 0;
	}

#define SW_PCA9575_SFP_TOPO_IDX		2
#define SW_PCA9575_QSFP_TOPO_IDX	1

	/* Check if the SW IO expander controlling SMA exists in the netlist. */
	if (hw->device_id == ICE_DEV_ID_E810C_SFP)
		idx = SW_PCA9575_SFP_TOPO_IDX;
	else if (hw->device_id == ICE_DEV_ID_E810C_QSFP)
		idx = SW_PCA9575_QSFP_TOPO_IDX;
	else
		return -ENXIO;

	/* If handle was not detected read it from the netlist */
	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_get_link_topo);
	cmd = libie_aq_raw(&desc);
	cmd->addr.topo_params.node_type_ctx =
		ICE_AQC_LINK_TOPO_NODE_TYPE_GPIO_CTRL;
	cmd->addr.topo_params.index = idx;

	err = ice_aq_send_cmd(hw, &desc, NULL, 0, NULL);
	if (err)
		return -ENXIO;

	/* Verify if we found the right IO expander type */
	if (cmd->node_part_num != ICE_AQC_GET_LINK_TOPO_NODE_NR_PCA9575)
		return -ENXIO;

	/* If present save the handle and return it */
	hw->io_expander_handle =
		le16_to_cpu(cmd->addr.handle);
	*pca9575_handle = hw->io_expander_handle;

	return 0;
}

/**
 * ice_read_pca9575_reg - read the register from the PCA9575 controller
 * @hw: pointer to the hw struct
 * @offset: GPIO controller register offset
 * @data: pointer to data to be read from the GPIO controller
 *
 * Return: 0 on success, negative error code otherwise.
 */
int ice_read_pca9575_reg(struct ice_hw *hw, u8 offset, u8 *data)
{
	struct ice_aqc_link_topo_addr link_topo;
	__le16 addr;
	u16 handle;
	int err;

	memset(&link_topo, 0, sizeof(link_topo));

	err = ice_get_pca9575_handle(hw, &handle);
	if (err)
		return err;

	link_topo.handle = cpu_to_le16(handle);
	link_topo.topo_params.node_type_ctx =
		FIELD_PREP(ICE_AQC_LINK_TOPO_NODE_CTX_M,
			   ICE_AQC_LINK_TOPO_NODE_CTX_PROVIDED);

	addr = cpu_to_le16((u16)offset);

	return ice_aq_read_i2c(hw, link_topo, 0, addr, 1, data, NULL);
}

/**
 * ice_aq_set_gpio
 * @hw: pointer to the hw struct
 * @gpio_ctrl_handle: GPIO controller node handle
 * @pin_idx: IO Number of the GPIO that needs to be set
 * @value: SW provide IO value to set in the LSB
 * @cd: pointer to command details structure or NULL
 *
 * Sends 0x06EC AQ command to set the GPIO pin state that's part of the topology
 */
int
ice_aq_set_gpio(struct ice_hw *hw, u16 gpio_ctrl_handle, u8 pin_idx, bool value,
		struct ice_sq_cd *cd)
{
	struct libie_aq_desc desc;
	struct ice_aqc_gpio *cmd;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_set_gpio);
	cmd = libie_aq_raw(&desc);
	cmd->gpio_ctrl_handle = cpu_to_le16(gpio_ctrl_handle);
	cmd->gpio_num = pin_idx;
	cmd->gpio_val = value ? 1 : 0;

	return ice_aq_send_cmd(hw, &desc, NULL, 0, cd);
}

/**
 * ice_aq_get_gpio
 * @hw: pointer to the hw struct
 * @gpio_ctrl_handle: GPIO controller node handle
 * @pin_idx: IO Number of the GPIO that needs to be set
 * @value: IO value read
 * @cd: pointer to command details structure or NULL
 *
 * Sends 0x06ED AQ command to get the value of a GPIO signal which is part of
 * the topology
 */
int
ice_aq_get_gpio(struct ice_hw *hw, u16 gpio_ctrl_handle, u8 pin_idx,
		bool *value, struct ice_sq_cd *cd)
{
	struct libie_aq_desc desc;
	struct ice_aqc_gpio *cmd;
	int status;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_get_gpio);
	cmd = libie_aq_raw(&desc);
	cmd->gpio_ctrl_handle = cpu_to_le16(gpio_ctrl_handle);
	cmd->gpio_num = pin_idx;

	status = ice_aq_send_cmd(hw, &desc, NULL, 0, cd);
	if (status)
		return status;

	*value = !!cmd->gpio_val;
	return 0;
}

/**
 * ice_is_fw_api_min_ver
 * @hw: pointer to the hardware structure
 * @maj: major version
 * @min: minor version
 * @patch: patch version
 *
 * Checks if the firmware API is minimum version
 */
static bool ice_is_fw_api_min_ver(struct ice_hw *hw, u8 maj, u8 min, u8 patch)
{
	if (hw->api_maj_ver == maj) {
		if (hw->api_min_ver > min)
			return true;
		if (hw->api_min_ver == min && hw->api_patch >= patch)
			return true;
	} else if (hw->api_maj_ver > maj) {
		return true;
	}

	return false;
}

/**
 * ice_fw_supports_link_override
 * @hw: pointer to the hardware structure
 *
 * Checks if the firmware supports link override
 */
bool ice_fw_supports_link_override(struct ice_hw *hw)
{
	return ice_is_fw_api_min_ver(hw, ICE_FW_API_LINK_OVERRIDE_MAJ,
				     ICE_FW_API_LINK_OVERRIDE_MIN,
				     ICE_FW_API_LINK_OVERRIDE_PATCH);
}

/**
 * ice_get_link_default_override
 * @ldo: pointer to the link default override struct
 * @pi: pointer to the port info struct
 *
 * Gets the link default override for a port
 */
int
ice_get_link_default_override(struct ice_link_default_override_tlv *ldo,
			      struct ice_port_info *pi)
{
	u16 i, tlv, tlv_len, tlv_start, buf, offset;
	struct ice_hw *hw = pi->hw;
	int status;

	status = ice_get_pfa_module_tlv(hw, &tlv, &tlv_len,
					ICE_SR_LINK_DEFAULT_OVERRIDE_PTR);
	if (status) {
		ice_debug(hw, ICE_DBG_INIT, "Failed to read link override TLV.\n");
		return status;
	}

	/* Each port has its own config; calculate for our port */
	tlv_start = tlv + pi->lport * ICE_SR_PFA_LINK_OVERRIDE_WORDS +
		ICE_SR_PFA_LINK_OVERRIDE_OFFSET;

	/* link options first */
	status = ice_read_sr_word(hw, tlv_start, &buf);
	if (status) {
		ice_debug(hw, ICE_DBG_INIT, "Failed to read override link options.\n");
		return status;
	}
	ldo->options = FIELD_GET(ICE_LINK_OVERRIDE_OPT_M, buf);
	ldo->phy_config = (buf & ICE_LINK_OVERRIDE_PHY_CFG_M) >>
		ICE_LINK_OVERRIDE_PHY_CFG_S;

	/* link PHY config */
	offset = tlv_start + ICE_SR_PFA_LINK_OVERRIDE_FEC_OFFSET;
	status = ice_read_sr_word(hw, offset, &buf);
	if (status) {
		ice_debug(hw, ICE_DBG_INIT, "Failed to read override phy config.\n");
		return status;
	}
	ldo->fec_options = buf & ICE_LINK_OVERRIDE_FEC_OPT_M;

	/* PHY types low */
	offset = tlv_start + ICE_SR_PFA_LINK_OVERRIDE_PHY_OFFSET;
	for (i = 0; i < ICE_SR_PFA_LINK_OVERRIDE_PHY_WORDS; i++) {
		status = ice_read_sr_word(hw, (offset + i), &buf);
		if (status) {
			ice_debug(hw, ICE_DBG_INIT, "Failed to read override link options.\n");
			return status;
		}
		/* shift 16 bits at a time to fill 64 bits */
		ldo->phy_type_low |= ((u64)buf << (i * 16));
	}

	/* PHY types high */
	offset = tlv_start + ICE_SR_PFA_LINK_OVERRIDE_PHY_OFFSET +
		ICE_SR_PFA_LINK_OVERRIDE_PHY_WORDS;
	for (i = 0; i < ICE_SR_PFA_LINK_OVERRIDE_PHY_WORDS; i++) {
		status = ice_read_sr_word(hw, (offset + i), &buf);
		if (status) {
			ice_debug(hw, ICE_DBG_INIT, "Failed to read override link options.\n");
			return status;
		}
		/* shift 16 bits at a time to fill 64 bits */
		ldo->phy_type_high |= ((u64)buf << (i * 16));
	}

	return status;
}

/**
 * ice_is_phy_caps_an_enabled - check if PHY capabilities autoneg is enabled
 * @caps: get PHY capability data
 */
bool ice_is_phy_caps_an_enabled(struct ice_aqc_get_phy_caps_data *caps)
{
	if (caps->caps & ICE_AQC_PHY_AN_MODE ||
	    caps->low_power_ctrl_an & (ICE_AQC_PHY_AN_EN_CLAUSE28 |
				       ICE_AQC_PHY_AN_EN_CLAUSE73 |
				       ICE_AQC_PHY_AN_EN_CLAUSE37))
		return true;

	return false;
}

/**
 * ice_is_fw_health_report_supported - checks if firmware supports health events
 * @hw: pointer to the hardware structure
 *
 * Return: true if firmware supports health status reports,
 * false otherwise
 */
bool ice_is_fw_health_report_supported(struct ice_hw *hw)
{
	return ice_is_fw_api_min_ver(hw, ICE_FW_API_HEALTH_REPORT_MAJ,
				     ICE_FW_API_HEALTH_REPORT_MIN,
				     ICE_FW_API_HEALTH_REPORT_PATCH);
}

/**
 * ice_aq_set_health_status_cfg - Configure FW health events
 * @hw: pointer to the HW struct
 * @event_source: type of diagnostic events to enable
 *
 * Configure the health status event types that the firmware will send to this
 * PF. The supported event types are: PF-specific, all PFs, and global.
 *
 * Return: 0 on success, negative error code otherwise.
 */
int ice_aq_set_health_status_cfg(struct ice_hw *hw, u8 event_source)
{
	struct ice_aqc_set_health_status_cfg *cmd;
	struct libie_aq_desc desc;

	cmd = libie_aq_raw(&desc);

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_set_health_status_cfg);

	cmd->event_source = event_source;

	return ice_aq_send_cmd(hw, &desc, NULL, 0, NULL);
}

/**
 * ice_aq_set_lldp_mib - Set the LLDP MIB
 * @hw: pointer to the HW struct
 * @mib_type: Local, Remote or both Local and Remote MIBs
 * @buf: pointer to the caller-supplied buffer to store the MIB block
 * @buf_size: size of the buffer (in bytes)
 * @cd: pointer to command details structure or NULL
 *
 * Set the LLDP MIB. (0x0A08)
 */
int
ice_aq_set_lldp_mib(struct ice_hw *hw, u8 mib_type, void *buf, u16 buf_size,
		    struct ice_sq_cd *cd)
{
	struct ice_aqc_lldp_set_local_mib *cmd;
	struct libie_aq_desc desc;

	cmd = libie_aq_raw(&desc);

	if (buf_size == 0 || !buf)
		return -EINVAL;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_lldp_set_local_mib);

	desc.flags |= cpu_to_le16((u16)LIBIE_AQ_FLAG_RD);
	desc.datalen = cpu_to_le16(buf_size);

	cmd->type = mib_type;
	cmd->length = cpu_to_le16(buf_size);

	return ice_aq_send_cmd(hw, &desc, buf, buf_size, cd);
}

/**
 * ice_fw_supports_lldp_fltr_ctrl - check NVM version supports lldp_fltr_ctrl
 * @hw: pointer to HW struct
 */
bool ice_fw_supports_lldp_fltr_ctrl(struct ice_hw *hw)
{
	if (hw->mac_type != ICE_MAC_E810)
		return false;

	return ice_is_fw_api_min_ver(hw, ICE_FW_API_LLDP_FLTR_MAJ,
				     ICE_FW_API_LLDP_FLTR_MIN,
				     ICE_FW_API_LLDP_FLTR_PATCH);
}

/**
 * ice_lldp_fltr_add_remove - add or remove a LLDP Rx switch filter
 * @hw: pointer to HW struct
 * @vsi: VSI to add the filter to
 * @add: boolean for if adding or removing a filter
 *
 * Return: 0 on success, -EOPNOTSUPP if the operation cannot be performed
 *	   with this HW or VSI, otherwise an error corresponding to
 *	   the AQ transaction result.
 */
int ice_lldp_fltr_add_remove(struct ice_hw *hw, struct ice_vsi *vsi, bool add)
{
	struct ice_aqc_lldp_filter_ctrl *cmd;
	struct libie_aq_desc desc;

	if (vsi->type != ICE_VSI_PF || !ice_fw_supports_lldp_fltr_ctrl(hw))
		return -EOPNOTSUPP;

	cmd = libie_aq_raw(&desc);

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_lldp_filter_ctrl);

	if (add)
		cmd->cmd_flags = ICE_AQC_LLDP_FILTER_ACTION_ADD;
	else
		cmd->cmd_flags = ICE_AQC_LLDP_FILTER_ACTION_DELETE;

	cmd->vsi_num = cpu_to_le16(vsi->vsi_num);

	return ice_aq_send_cmd(hw, &desc, NULL, 0, NULL);
}

/**
 * ice_lldp_execute_pending_mib - execute LLDP pending MIB request
 * @hw: pointer to HW struct
 */
int ice_lldp_execute_pending_mib(struct ice_hw *hw)
{
	struct libie_aq_desc desc;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_lldp_execute_pending_mib);

	return ice_aq_send_cmd(hw, &desc, NULL, 0, NULL);
}

/**
 * ice_fw_supports_report_dflt_cfg
 * @hw: pointer to the hardware structure
 *
 * Checks if the firmware supports report default configuration
 */
bool ice_fw_supports_report_dflt_cfg(struct ice_hw *hw)
{
	return ice_is_fw_api_min_ver(hw, ICE_FW_API_REPORT_DFLT_CFG_MAJ,
				     ICE_FW_API_REPORT_DFLT_CFG_MIN,
				     ICE_FW_API_REPORT_DFLT_CFG_PATCH);
}

/* each of the indexes into the following array match the speed of a return
 * value from the list of AQ returned speeds like the range:
 * ICE_AQ_LINK_SPEED_10MB .. ICE_AQ_LINK_SPEED_100GB excluding
 * ICE_AQ_LINK_SPEED_UNKNOWN which is BIT(15) and maps to BIT(14) in this
 * array. The array is defined as 15 elements long because the link_speed
 * returned by the firmware is a 16 bit * value, but is indexed
 * by [fls(speed) - 1]
 */
static const u32 ice_aq_to_link_speed[] = {
	SPEED_10,	/* BIT(0) */
	SPEED_100,
	SPEED_1000,
	SPEED_2500,
	SPEED_5000,
	SPEED_10000,
	SPEED_20000,
	SPEED_25000,
	SPEED_40000,
	SPEED_50000,
	SPEED_100000,	/* BIT(10) */
	SPEED_200000,
};

/**
 * ice_get_link_speed - get integer speed from table
 * @index: array index from fls(aq speed) - 1
 *
 * Returns: u32 value containing integer speed
 */
u32 ice_get_link_speed(u16 index)
{
	if (index >= ARRAY_SIZE(ice_aq_to_link_speed))
		return 0;

	return ice_aq_to_link_speed[index];
}

/**
 * ice_read_cgu_reg - Read a CGU register
 * @hw: Pointer to the HW struct
 * @addr: Register address to read
 * @val: Storage for register value read
 *
 * Read the contents of a register of the Clock Generation Unit. Only
 * applicable to E82X devices.
 *
 * Return: 0 on success, other error codes when failed to read from CGU.
 */
int ice_read_cgu_reg(struct ice_hw *hw, u32 addr, u32 *val)
{
	struct ice_sbq_msg_input cgu_msg = {
		.opcode = ice_sbq_msg_rd,
		.dest_dev = ice_sbq_dev_cgu,
		.msg_addr_low = addr
	};
	int err;

	err = ice_sbq_rw_reg(hw, &cgu_msg, LIBIE_AQ_FLAG_RD);
	if (err) {
		ice_debug(hw, ICE_DBG_PTP, "Failed to read CGU register 0x%04x, err %d\n",
			  addr, err);
		return err;
	}

	*val = cgu_msg.data;

	return 0;
}

/**
 * ice_write_cgu_reg - Write a CGU register
 * @hw: Pointer to the HW struct
 * @addr: Register address to write
 * @val: Value to write into the register
 *
 * Write the specified value to a register of the Clock Generation Unit. Only
 * applicable to E82X devices.
 *
 * Return: 0 on success, other error codes when failed to write to CGU.
 */
int ice_write_cgu_reg(struct ice_hw *hw, u32 addr, u32 val)
{
	struct ice_sbq_msg_input cgu_msg = {
		.opcode = ice_sbq_msg_wr,
		.dest_dev = ice_sbq_dev_cgu,
		.msg_addr_low = addr,
		.data = val
	};
	int err;

	err = ice_sbq_rw_reg(hw, &cgu_msg, LIBIE_AQ_FLAG_RD);
	if (err)
		ice_debug(hw, ICE_DBG_PTP, "Failed to write CGU register 0x%04x, err %d\n",
			  addr, err);

	return err;
}
