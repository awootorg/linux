/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/*
 * Copyright (C) 2005-2014, 2018-2021, 2024-2025 Intel Corporation
 * Copyright (C) 2013-2014 Intel Mobile Communications GmbH
 * Copyright (C) 2015 Intel Deutschland GmbH
 */
#ifndef __iwl_op_mode_h__
#define __iwl_op_mode_h__

#include <linux/netdevice.h>
#include <linux/debugfs.h>
#include "iwl-dbg-tlv.h"

struct iwl_op_mode;
struct iwl_trans;
struct sk_buff;
struct iwl_device_cmd;
struct iwl_rx_cmd_buffer;
struct iwl_fw;
struct iwl_rf_cfg;

/**
 * DOC: Operational mode - what is it ?
 *
 * The operational mode (a.k.a. op_mode) is the layer that implements
 * mac80211's handlers. It knows two APIs: mac80211's and the fw's. It uses
 * the transport API to access the HW. The op_mode doesn't need to know how the
 * underlying HW works, since the transport layer takes care of that.
 *
 * There can be several op_mode: i.e. different fw APIs will require two
 * different op_modes. This is why the op_mode is virtualized.
 */

/**
 * DOC: Life cycle of the Operational mode
 *
 * The operational mode has a very simple life cycle.
 *
 *	1) The driver layer (iwl-drv.c) chooses the op_mode based on the
 *	   capabilities advertised by the fw file (in TLV format).
 *	2) The driver layer starts the op_mode (ops->start)
 *	3) The op_mode registers mac80211
 *	4) The op_mode is governed by mac80211
 *	5) The driver layer stops the op_mode
 */

/**
 * enum iwl_fw_error_type - FW error types/sources
 * @IWL_ERR_TYPE_IRQ: "normal" FW error through an IRQ
 * @IWL_ERR_TYPE_NMI_FORCED: NMI was forced by driver
 * @IWL_ERR_TYPE_RESET_HS_TIMEOUT: reset handshake timed out,
 *	any debug collection must happen synchronously as
 *	the device will be shut down
 * @IWL_ERR_TYPE_CMD_QUEUE_FULL: command queue was full
 * @IWL_ERR_TYPE_TOP_RESET_BY_BT: TOP reset initiated by BT
 * @IWL_ERR_TYPE_TOP_FATAL_ERROR: TOP fatal error
 * @IWL_ERR_TYPE_TOP_RESET_FAILED: TOP reset failed
 * @IWL_ERR_TYPE_DEBUGFS: error/reset indication from debugfs
 */
enum iwl_fw_error_type {
	IWL_ERR_TYPE_IRQ,
	IWL_ERR_TYPE_NMI_FORCED,
	IWL_ERR_TYPE_RESET_HS_TIMEOUT,
	IWL_ERR_TYPE_CMD_QUEUE_FULL,
	IWL_ERR_TYPE_TOP_RESET_BY_BT,
	IWL_ERR_TYPE_TOP_FATAL_ERROR,
	IWL_ERR_TYPE_TOP_RESET_FAILED,
	IWL_ERR_TYPE_DEBUGFS,
};

/**
 * enum iwl_fw_error_context - error dump context
 * @IWL_ERR_CONTEXT_WORKER: regular from worker context,
 *	opmode must acquire locks and must also check
 *	for @IWL_ERR_CONTEXT_ABORT after acquiring locks
 * @IWL_ERR_CONTEXT_FROM_OPMODE: context is in a call
 *	originating from the opmode, e.g. while resetting
 *	or stopping the device, so opmode must not acquire
 *	any locks
 * @IWL_ERR_CONTEXT_ABORT: after lock acquisition, indicates
 *	that the dump already happened via another callback
 *	(currently only while stopping the device) via the
 *	@IWL_ERR_CONTEXT_FROM_OPMODE context, and this call
 *	must be aborted
 */
enum iwl_fw_error_context {
	IWL_ERR_CONTEXT_WORKER,
	IWL_ERR_CONTEXT_FROM_OPMODE,
	IWL_ERR_CONTEXT_ABORT,
};

/**
 * struct iwl_fw_error_dump_mode - error dump mode for callback
 * @type: The reason for the dump, per &enum iwl_fw_error_type.
 * @context: The context for the dump, may also indicate this
 *	call needs to be skipped. This MUST be checked before
 *	and after acquiring any locks in the op-mode!
 */
struct iwl_fw_error_dump_mode {
	enum iwl_fw_error_type type;
	enum iwl_fw_error_context context;
};

/**
 * struct iwl_op_mode_ops - op_mode specific operations
 *
 * The op_mode exports its ops so that external components can start it and
 * interact with it. The driver layer typically calls the start and stop
 * handlers, the transport layer calls the others.
 *
 * All the handlers MUST be implemented, except @rx_rss which can be left
 * out *iff* the opmode will never run on hardware with multi-queue capability.
 *
 * @start: start the op_mode. The transport layer is already allocated.
 *	May sleep
 * @stop: stop the op_mode. Must free all the memory allocated.
 *	May sleep
 * @rx: Rx notification to the op_mode. rxb is the Rx buffer itself. Cmd is the
 *	HCMD this Rx responds to. Can't sleep.
 * @rx_rss: data queue RX notification to the op_mode, for (data) notifications
 *	received on the RSS queue(s). The queue parameter indicates which of the
 *	RSS queues received this frame; it will always be non-zero.
 *	This method must not sleep.
 * @queue_full: notifies that a HW queue is full.
 *	Must be atomic and called with BH disabled.
 * @queue_not_full: notifies that a HW queue is not full any more.
 *	Must be atomic and called with BH disabled.
 * @hw_rf_kill: notifies of a change in the HW rf kill switch. True means that
 *	the radio is killed. Return %true if the device should be stopped by
 *	the transport immediately after the call. May sleep.
 *	Note that this must not return %true for newer devices using gen2 PCIe
 *	transport.
 * @free_skb: allows the transport layer to free skbs that haven't been
 *	reclaimed by the op_mode. This can happen when the driver is freed and
 *	there are Tx packets pending in the transport layer.
 *	Must be atomic
 * @nic_error: error notification. Must be atomic, the op mode should handle
 *	the error (e.g. abort notification waiters) and print the error if
 *	applicable
 * @dump_error: NIC error dump collection (can sleep, synchronous)
 * @sw_reset: (maybe) initiate a software reset, return %true if started
 * @nic_config: configure NIC, called before firmware is started.
 *	May sleep
 * @wimax_active: invoked when WiMax becomes active. May sleep
 * @time_point: called when transport layer wants to collect debug data
 * @device_powered_off: called upon resume from hibernation but not only.
 *	Op_mode needs to reset its internal state because the device did not
 *	survive the system state transition. The firmware is no longer running,
 *	etc...
 * @dump: Op_mode needs to collect the firmware dump upon this handler
 *	being called.
 */
struct iwl_op_mode_ops {
	struct iwl_op_mode *(*start)(struct iwl_trans *trans,
				     const struct iwl_rf_cfg *cfg,
				     const struct iwl_fw *fw,
				     struct dentry *dbgfs_dir);
	void (*stop)(struct iwl_op_mode *op_mode);
	void (*rx)(struct iwl_op_mode *op_mode, struct napi_struct *napi,
		   struct iwl_rx_cmd_buffer *rxb);
	void (*rx_rss)(struct iwl_op_mode *op_mode, struct napi_struct *napi,
		       struct iwl_rx_cmd_buffer *rxb, unsigned int queue);
	void (*queue_full)(struct iwl_op_mode *op_mode, int queue);
	void (*queue_not_full)(struct iwl_op_mode *op_mode, int queue);
	bool (*hw_rf_kill)(struct iwl_op_mode *op_mode, bool state);
	void (*free_skb)(struct iwl_op_mode *op_mode, struct sk_buff *skb);
	void (*nic_error)(struct iwl_op_mode *op_mode,
			  enum iwl_fw_error_type type);
	void (*dump_error)(struct iwl_op_mode *op_mode,
			   struct iwl_fw_error_dump_mode *mode);
	bool (*sw_reset)(struct iwl_op_mode *op_mode,
			 enum iwl_fw_error_type type);
	void (*nic_config)(struct iwl_op_mode *op_mode);
	void (*wimax_active)(struct iwl_op_mode *op_mode);
	void (*time_point)(struct iwl_op_mode *op_mode,
			   enum iwl_fw_ini_time_point tp_id,
			   union iwl_dbg_tlv_tp_data *tp_data);
	void (*device_powered_off)(struct iwl_op_mode *op_mode);
	void (*dump)(struct iwl_op_mode *op_mode);
};

int iwl_opmode_register(const char *name, const struct iwl_op_mode_ops *ops);
void iwl_opmode_deregister(const char *name);

/**
 * struct iwl_op_mode - operational mode
 * @ops: pointer to its own ops
 *
 * This holds an implementation of the mac80211 / fw API.
 */
struct iwl_op_mode {
	const struct iwl_op_mode_ops *ops;

	char op_mode_specific[] __aligned(sizeof(void *));
};

static inline void iwl_op_mode_stop(struct iwl_op_mode *op_mode)
{
	might_sleep();
	op_mode->ops->stop(op_mode);
}

static inline void iwl_op_mode_rx(struct iwl_op_mode *op_mode,
				  struct napi_struct *napi,
				  struct iwl_rx_cmd_buffer *rxb)
{
	return op_mode->ops->rx(op_mode, napi, rxb);
}

static inline void iwl_op_mode_rx_rss(struct iwl_op_mode *op_mode,
				      struct napi_struct *napi,
				      struct iwl_rx_cmd_buffer *rxb,
				      unsigned int queue)
{
	op_mode->ops->rx_rss(op_mode, napi, rxb, queue);
}

static inline void iwl_op_mode_queue_full(struct iwl_op_mode *op_mode,
					  int queue)
{
	op_mode->ops->queue_full(op_mode, queue);
}

static inline void iwl_op_mode_queue_not_full(struct iwl_op_mode *op_mode,
					      int queue)
{
	op_mode->ops->queue_not_full(op_mode, queue);
}

static inline bool __must_check
iwl_op_mode_hw_rf_kill(struct iwl_op_mode *op_mode, bool state)
{
	might_sleep();
	return op_mode->ops->hw_rf_kill(op_mode, state);
}

static inline void iwl_op_mode_free_skb(struct iwl_op_mode *op_mode,
					struct sk_buff *skb)
{
	if (WARN_ON_ONCE(!op_mode))
		return;
	op_mode->ops->free_skb(op_mode, skb);
}

static inline void iwl_op_mode_nic_error(struct iwl_op_mode *op_mode,
					 enum iwl_fw_error_type type)
{
	op_mode->ops->nic_error(op_mode, type);
}

static inline void iwl_op_mode_dump_error(struct iwl_op_mode *op_mode,
					  struct iwl_fw_error_dump_mode *mode)
{
	might_sleep();

	if (WARN_ON(mode->type == IWL_ERR_TYPE_TOP_RESET_BY_BT))
		return;

	if (op_mode->ops->dump_error)
		op_mode->ops->dump_error(op_mode, mode);
}

static inline void iwl_op_mode_nic_config(struct iwl_op_mode *op_mode)
{
	might_sleep();
	if (op_mode->ops->nic_config)
		op_mode->ops->nic_config(op_mode);
}

static inline void iwl_op_mode_wimax_active(struct iwl_op_mode *op_mode)
{
	might_sleep();
	op_mode->ops->wimax_active(op_mode);
}

static inline void iwl_op_mode_time_point(struct iwl_op_mode *op_mode,
					  enum iwl_fw_ini_time_point tp_id,
					  union iwl_dbg_tlv_tp_data *tp_data)
{
	if (!op_mode || !op_mode->ops || !op_mode->ops->time_point)
		return;
	op_mode->ops->time_point(op_mode, tp_id, tp_data);
}

static inline void iwl_op_mode_device_powered_off(struct iwl_op_mode *op_mode)
{
	if (!op_mode || !op_mode->ops || !op_mode->ops->device_powered_off)
		return;
	op_mode->ops->device_powered_off(op_mode);
}

static inline void iwl_op_mode_dump(struct iwl_op_mode *op_mode)
{
	if (!op_mode || !op_mode->ops || !op_mode->ops->dump)
		return;
	op_mode->ops->dump(op_mode);
}

#endif /* __iwl_op_mode_h__ */
