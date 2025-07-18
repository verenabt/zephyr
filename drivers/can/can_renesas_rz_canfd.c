/*
 * Copyright (c) 2025 Renesas Electronics Corporation
 * SPDX-License-Identifier: Apache-2.0
 */

#include <soc.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/can.h>
#include <zephyr/drivers/can/transceiver.h>
#include "r_can_api.h"
#include "r_canfd.h"
#include <zephyr/drivers/clock_control.h>

LOG_MODULE_REGISTER(can_renesas_rz_canfd, CONFIG_CAN_LOG_LEVEL);

#define DT_DRV_COMPAT renesas_rz_canfd

#define CAN_RENESAS_RZ_TIMING_MAX                                                                  \
	{                                                                                          \
		.sjw = 32,                                                                         \
		.prop_seg = 1,                                                                     \
		.phase_seg1 = 127,                                                                 \
		.phase_seg2 = 32,                                                                  \
		.prescaler = 1024,                                                                 \
	}

#define CAN_RENESAS_RZ_TIMING_MIN                                                                  \
	{                                                                                          \
		.sjw = 1,                                                                          \
		.prop_seg = 1,                                                                     \
		.phase_seg1 = 2,                                                                   \
		.phase_seg2 = 2,                                                                   \
		.prescaler = 1,                                                                    \
	}

#ifdef CONFIG_CAN_FD_MODE
#define CAN_RENESAS_RZ_TIMING_DATA_MAX                                                             \
	{                                                                                          \
		.sjw = 16,                                                                         \
		.prop_seg = 1,                                                                     \
		.phase_seg1 = 31,                                                                  \
		.phase_seg2 = 16,                                                                  \
		.prescaler = 256,                                                                  \
	}
#define CAN_RENESAS_RZ_TIMING_DATA_MIN                                                             \
	{                                                                                          \
		.sjw = 1,                                                                          \
		.prop_seg = 1,                                                                     \
		.phase_seg1 = 2,                                                                   \
		.phase_seg2 = 2,                                                                   \
		.prescaler = 1,                                                                    \
	}
#endif

#define CAN_RENESAS_RZ_CHN0 0
#define CAN_RENESAS_RZ_CHN1 1

/* This frame ID will be reserved. Any filter using this ID may cause undefined behavior. */
#define CAN_RENESAS_RZ_RESERVED_ID (CAN_EXT_ID_MASK)

/*
 * TX Message Buffer Interrupt Enable Configuration: refer to '30.2.53 TX Message Buffer
 * Transmission Interrupt Enable Register n (CFDTMIECn)' - RZ/G3S MCU group HWM
 */
#define CANFD_CFG_TXMB_TXI_ENABLE (BIT(0)) /* Enable TXMB0 interrupt */

/*
 * Channel Error IRQ configurations: refer to '30.2.2 Channel n Control Register (CFDCnCTR)' -
 * RZ/G3S MCU group HWM
 */
#define CANFD_CFG_ERR_IRQ                                                                          \
	(BIT(R_CANFD_CFDC_CTR_EWIE_Pos) |  /* Error Warning Interrupt Enable */                    \
	 BIT(R_CANFD_CFDC_CTR_EPIE_Pos) |  /* Error Passive Interrupt Enable */                    \
	 BIT(R_CANFD_CFDC_CTR_BOEIE_Pos) | /* Bus-Off Entry Interrupt Enable */                    \
	 BIT(R_CANFD_CFDC_CTR_BORIE_Pos) | /* Bus-Off Recovery Interrupt Enable */                 \
	 BIT(R_CANFD_CFDC_CTR_OLIE_Pos))   /* Overload Interrupt Enable */

#define RULE_NUM_PER_PAGE (16)

#define CANFD_CFG_RXFIFO_DEFAULT(x)                                                                \
	((CANFD_CFG_RXFIFO##x##_INT_THRESHOLD << R_CANFD_CFDRFCC_RFIGCV_Pos) |                     \
	 (CANFD_CFG_RXFIFO##x##_DEPTH << R_CANFD_CFDRFCC_RFDC_Pos) |                               \
	 (CANFD_CFG_RXFIFO##x##_PAYLOAD << R_CANFD_CFDRFCC_RFPLS_Pos) |                            \
	 (CANFD_CFG_RXFIFO##x##_INT_MODE) | (CANFD_CFG_RXFIFO##x##_ENABLE))

/* Default Dataphase bitrate configuration in case classic mode is enabled */
static const can_bit_timing_cfg_t classic_can_data_timing_default = {
	.baud_rate_prescaler = 1,
	.time_segment_1 = 3,
	.time_segment_2 = 2,
	.synchronization_jump_width = 1,
};

struct can_renesas_rz_filter {
	bool set;
	struct can_filter filter;
	can_rx_callback_t rx_cb;
	void *rx_usr_data;
};

struct can_renesas_rz_cfg {
	struct can_driver_config common;
	const struct pinctrl_dev_config *pin_config;
	const struct device *global_dev;
	const uint32_t rx_filter_num;
	const can_api_t *fsp_api;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	uint32_t clock_ext_rate;
};

struct can_renesas_rz_data {
	struct can_driver_data common;
	struct k_mutex inst_mutex;
	can_tx_callback_t tx_cb;
	struct k_sem tx_sem;
	void *tx_usr_data;
	struct can_renesas_rz_filter *rx_filter;
	can_bit_timing_cfg_t data_timing;

	canfd_instance_ctrl_t *fsp_ctrl;
	can_cfg_t *fsp_cfg;
};

extern void canfd_error_isr(void);
extern void canfd_rx_fifo_isr(void);
extern void canfd_channel_tx_isr(void);
static inline int can_renesas_rz_apply_default_config(const struct device *dev);

static inline int set_hw_timing_configuration(const struct device *dev,
					      can_bit_timing_cfg_t *f_timing,
					      const struct can_timing *z_timing)
{
	struct can_renesas_rz_data *data = dev->data;

	if (f_timing == NULL || z_timing == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&data->inst_mutex, K_FOREVER);

	*f_timing = (can_bit_timing_cfg_t){
		.baud_rate_prescaler = z_timing->prescaler,
		.time_segment_1 = z_timing->prop_seg + z_timing->phase_seg1,
		.time_segment_2 = z_timing->phase_seg2,
		.synchronization_jump_width = z_timing->sjw,
	};

	k_mutex_unlock(&data->inst_mutex);

	return 0;
}

static inline int get_free_filter_id(const struct device *dev)
{
	struct can_renesas_rz_data *data = dev->data;
	const struct can_renesas_rz_cfg *cfg = dev->config;
	int ret = 0;

	k_mutex_lock(&data->inst_mutex, K_FOREVER);

	for (uint32_t filter_id = 0; filter_id < cfg->rx_filter_num; filter_id++) {
		if (!data->rx_filter[filter_id].set) {
			ret = filter_id;
			k_mutex_unlock(&data->inst_mutex);
			return ret;
		}
	}

	/* there are no free filter */
	ret = -ENOSPC;
	k_mutex_unlock(&data->inst_mutex);
	return ret;
}

static void set_afl_rule(const struct device *dev, const struct can_filter *filter,
			 uint32_t afl_offset)
{
	struct can_renesas_rz_data *data = dev->data;
	canfd_extended_cfg_t const *p_extend = data->fsp_cfg->p_extend;
	uint32_t channel = data->fsp_cfg->channel;
	canfd_afl_entry_t *afl = (canfd_afl_entry_t *)&p_extend->p_afl[afl_offset];

	*afl = (canfd_afl_entry_t){.id = {.id = filter->id,
#ifndef CONFIG_CAN_ACCEPT_RTR
					  .frame_type = CAN_FRAME_TYPE_DATA,
#endif
					  .id_mode = ((filter->flags & CAN_FILTER_IDE) != 0)
							     ? CAN_ID_MODE_EXTENDED
							     : CAN_ID_MODE_STANDARD},
				   .mask = {.mask_id = filter->mask,
#ifdef CONFIG_CAN_ACCEPT_RTR
					    /* Accept all types of frames */
					    .mask_frame_type = 0,
#else
					    /* Only accept frames with the configured frametype */
					    .mask_frame_type = 1,
#endif
					    .mask_id_mode = ((filter->flags & CAN_FILTER_IDE) != 0)
								    ? CAN_ID_MODE_EXTENDED
								    : CAN_ID_MODE_STANDARD},
				   .destination = {.fifo_select_flags = CANFD_RX_FIFO_0}};

	if (data->common.started) {
		/* These steps help update AFL rules while CAN module running */
		canfd_instance_ctrl_t *p_ctrl = (canfd_instance_ctrl_t *)data->fsp_ctrl;
		R_CANFD_Type *reg = p_ctrl->p_reg;

		/* Unlock AFL entry access */
		reg->CFDGAFLECTR_b.AFLDAE = true;

		/** set AFL page number */
		if (channel == CAN_RENESAS_RZ_CHN1) {
			reg->CFDGAFLECTR_b.AFLPN =
				(CANFD_CFG_AFL_CH0_RULE_NUM + afl_offset) / RULE_NUM_PER_PAGE;
		} else {
			reg->CFDGAFLECTR_b.AFLPN = afl_offset / RULE_NUM_PER_PAGE;
		}

		reg->CFDGAFL[afl_offset % RULE_NUM_PER_PAGE] = *(R_CANFD_CFDGAFL_Type *)afl;

		reg->CFDGAFL[afl_offset % RULE_NUM_PER_PAGE].P0_b.GAFLIFL0 =
			channel & CAN_RENESAS_RZ_CHN1;
		/* Lock AFL entry access */
		reg->CFDGAFLECTR_b.AFLDAE = false;
	}
}

static void remove_afl_rule(const struct device *dev, uint32_t afl_offset)
{
	struct can_renesas_rz_data *data = dev->data;
	uint32_t channel = data->fsp_cfg->channel;
	canfd_extended_cfg_t const *p_extend = data->fsp_cfg->p_extend;
	canfd_afl_entry_t *afl = (canfd_afl_entry_t *)&p_extend->p_afl[afl_offset];

	/* Set the AFL ID to reserved ID */
	*afl = (canfd_afl_entry_t){
		.id = {.id = CAN_RENESAS_RZ_RESERVED_ID, .id_mode = CAN_ID_MODE_EXTENDED},
		.mask = {.mask_id = CAN_RENESAS_RZ_RESERVED_ID,
			 .mask_id_mode = CAN_ID_MODE_EXTENDED}};

	if (data->common.started) {
		/* These steps help update AFL rules while CAN module running */
		canfd_instance_ctrl_t *p_ctrl = (canfd_instance_ctrl_t *)data->fsp_ctrl;
		R_CANFD_Type *reg = p_ctrl->p_reg;

		/* Unlock AFL entry access */
		reg->CFDGAFLECTR_b.AFLDAE = true;

		/** set AFL page number */
		if (channel == CAN_RENESAS_RZ_CHN1) {
			reg->CFDGAFLECTR_b.AFLPN =
				(CANFD_CFG_AFL_CH0_RULE_NUM + afl_offset) / RULE_NUM_PER_PAGE;
		} else {
			reg->CFDGAFLECTR_b.AFLPN = afl_offset / RULE_NUM_PER_PAGE;
		}

		/* Change configurations for AFL entry */
		reg->CFDGAFL[afl_offset % RULE_NUM_PER_PAGE] = *(R_CANFD_CFDGAFL_Type *)(afl);
		reg->CFDGAFL[afl_offset % RULE_NUM_PER_PAGE].P0_b.GAFLIFL0 =
			channel & CAN_RENESAS_RZ_CHN1;

		/* Lock AFL entry access */
		reg->CFDGAFLECTR_b.AFLDAE = false;
	}
}

#ifdef CONFIG_CAN_MANUAL_RECOVERY_MODE
static int recover_bus(const struct device *dev, k_timeout_t timeout)
{
	struct can_renesas_rz_data *data = dev->data;
	canfd_instance_ctrl_t *p_ctrl = data->fsp_ctrl;
	R_CANFD_Type *reg = p_ctrl->p_reg;
	uint32_t cfdcnctr = reg->CFDC->CTR;
	int ret = 0;

	if (reg->CFDC->ERFL_b.BOEF == 0) {
		/* Channel bus-off entry not detected */
		return 0;
	}

	reg->CFDC->CTR_b.BOM = 0x00; /* Switch to Normal Bus-Off mode (comply with ISO 11898-1) */
	reg->CFDC->CTR_b.RTBO = 1;   /* Force channel state to return from bus-off */

	int64_t start_ticks = k_uptime_ticks();

	while (reg->CFDC->ERFL_b.BORF == 0) {
		if ((k_uptime_ticks() - start_ticks) > timeout.ticks) {
			ret = -EAGAIN;
			break;
		}
	}
	reg->CFDC->CTR = cfdcnctr; /* Restore channel configuration */

	return ret;
}
#endif

static inline void can_renesas_rz_call_tx_cb(const struct device *dev, int err)
{
	struct can_renesas_rz_data *data = dev->data;
	can_tx_callback_t cb = data->tx_cb;

	if (cb != NULL) {
		data->tx_cb = NULL;
		cb(dev, err, data->tx_usr_data);
		k_sem_give(&data->tx_sem);
	}
}

static inline void can_renesas_rz_call_rx_cb(const struct device *dev, can_callback_args_t *p_args)
{
	struct can_renesas_rz_data *data = dev->data;
	struct can_renesas_rz_filter *rx_filter = data->rx_filter;
	const struct can_renesas_rz_cfg *cfg = dev->config;
	struct can_frame frame = {
		.dlc = can_bytes_to_dlc(p_args->frame.data_length_code),
		.id = p_args->frame.id,
		.flags = (((p_args->frame.id_mode == CAN_ID_MODE_EXTENDED) ? CAN_FRAME_IDE : 0UL) |
			  ((p_args->frame.type == CAN_FRAME_TYPE_REMOTE) ? CAN_FRAME_RTR : 0UL) |
			  ((p_args->frame.options & CANFD_FRAME_OPTION_FD) != 0 ? CAN_FRAME_FDF
										: 0UL) |
			  ((p_args->frame.options & CANFD_FRAME_OPTION_ERROR) != 0 ? CAN_FRAME_ESI
										   : 0UL) |
			  ((p_args->frame.options & CANFD_FRAME_OPTION_BRS) != 0 ? CAN_FRAME_BRS
										 : 0UL)),
	};

	memcpy(frame.data, p_args->frame.data, p_args->frame.data_length_code);

	for (uint32_t i = 0; i < cfg->rx_filter_num; i++) {
		can_rx_callback_t cb = rx_filter->rx_cb;
		void *usr_data = rx_filter->rx_usr_data;

		if (cb == NULL) {
			rx_filter++;
			continue; /* this filter has not set yet */
		}

		if (!can_frame_matches_filter(&frame, &rx_filter->filter)) {
			rx_filter++;
			continue; /* filter did not match */
		}

		cb(dev, &frame, usr_data);
		break;
	}
}

static inline void can_renesas_rz_call_state_change_cb(const struct device *dev,
						       enum can_state state)
{
	struct can_renesas_rz_data *data = dev->data;
	const struct can_renesas_rz_cfg *cfg = dev->config;
	can_info_t can_info;

	if (FSP_SUCCESS != cfg->fsp_api->infoGet(data->fsp_ctrl, &can_info)) {
		LOG_DBG("get state info failed");
		return;
	}

	struct can_bus_err_cnt err_cnt = {
		.rx_err_cnt = can_info.error_count_receive,
		.tx_err_cnt = can_info.error_count_transmit,
	};

	data->common.state_change_cb(dev, state, err_cnt, data->common.state_change_cb_user_data);
}

static int can_renesas_rz_get_capabilities(const struct device *dev, can_mode_t *cap)
{
	ARG_UNUSED(dev);
	*cap = CAN_MODE_NORMAL | CAN_MODE_LOOPBACK;

#ifdef CONFIG_CAN_FD_MODE
	*cap |= CAN_MODE_FD;
#endif

#ifdef CONFIG_CAN_MANUAL_RECOVERY_MODE
	*cap |= CAN_MODE_MANUAL_RECOVERY;
#endif

	return 0;
}

static int can_renesas_rz_start(const struct device *dev)
{
	struct can_renesas_rz_data *data = dev->data;
	const struct can_renesas_rz_cfg *cfg = dev->config;

	const struct device *transceiver_dev = can_get_transceiver(dev);
	int ret = 0;

	if (!device_is_ready(dev)) {
		return -EIO;
	}

	if (data->common.started) {
		return -EALREADY;
	}

	if ((transceiver_dev != NULL) &&
	    can_transceiver_enable(transceiver_dev, data->common.mode)) {
		LOG_DBG("CAN transceiver enable failed");
		return -EIO;
	}

	k_mutex_lock(&data->inst_mutex, K_FOREVER);
	canfd_extended_cfg_t *p_extend = (canfd_extended_cfg_t *)data->fsp_cfg->p_extend;

	p_extend->p_data_timing =
		(can_bit_timing_cfg_t *)((data->common.mode & CAN_MODE_FD) != 0
						 ? &data->data_timing
						 : &classic_can_data_timing_default);
	if (FSP_SUCCESS != cfg->fsp_api->close(data->fsp_ctrl)) {
		LOG_DBG("CAN close failed");
		ret = -EIO;
		goto end;
	}

	if (FSP_SUCCESS != cfg->fsp_api->open(data->fsp_ctrl, data->fsp_cfg)) {
		LOG_DBG("CAN open failed");
		ret = -EIO;
		goto end;
	}

	if ((data->common.mode & CAN_MODE_LOOPBACK) != 0) {
		if (FSP_SUCCESS != cfg->fsp_api->modeTransition(data->fsp_ctrl,
								CAN_OPERATION_MODE_NORMAL,
								CAN_TEST_MODE_LOOPBACK_INTERNAL)) {
			LOG_DBG("CAN mode change failed");
			ret = -EIO;
			goto end;
		}
	}

	data->common.started = true;
end:
	k_mutex_unlock(&data->inst_mutex);
	return ret;
}

static int can_renesas_rz_stop(const struct device *dev)
{
	struct can_renesas_rz_data *data = dev->data;
	const struct can_renesas_rz_cfg *cfg = dev->config;
	const struct device *transceiver_dev = can_get_transceiver(dev);
	int ret = 0;

	/** START body of stop function  */
	if (!data->common.started) {
		return -EALREADY;
	}

	k_mutex_lock(&data->inst_mutex, K_FOREVER);

	if (FSP_SUCCESS != cfg->fsp_api->modeTransition(data->fsp_ctrl, CAN_OPERATION_MODE_HALT,
							CAN_TEST_MODE_DISABLED)) {
		LOG_DBG("CAN stop failed");
		ret = -EIO;
		goto end;
	}

	if (((transceiver_dev != NULL) && can_transceiver_disable(transceiver_dev))) {
		LOG_DBG("CAN transceiver disable failed");
		ret = -EIO;
		goto end;
	}

	if (data->tx_cb != NULL) {
		data->tx_cb = NULL;
		k_sem_give(&data->tx_sem);
	}

	data->common.started = false;

end:
	k_mutex_unlock(&data->inst_mutex);
	return ret;
}

static int can_renesas_rz_set_mode(const struct device *dev, can_mode_t mode)
{
	struct can_renesas_rz_data *data = dev->data;
	int ret = 0;

	if (data->common.started) {
		/* CAN controller should be in stopped state */
		return -EBUSY;
	}

	k_mutex_lock(&data->inst_mutex, K_FOREVER);

	can_mode_t caps = 0;

	ret = can_renesas_rz_get_capabilities(dev, &caps);
	if (ret != 0) {
		goto end;
	}

	if ((mode & ~caps) != 0) {
		ret = -ENOTSUP;
		goto end;
	}

	data->common.mode = mode;

end:
	k_mutex_unlock(&data->inst_mutex);
	return ret;
}

static int can_renesas_rz_set_timing(const struct device *dev, const struct can_timing *timing)
{
	struct can_renesas_rz_data *data = dev->data;

	if (data->common.started) {
		/* Device is not in stopped state */
		return -EBUSY;
	}

	return set_hw_timing_configuration(dev, data->fsp_cfg->p_bit_timing, timing);
}

static int can_renesas_rz_send(const struct device *dev, const struct can_frame *frame,
			       k_timeout_t timeout, can_tx_callback_t callback, void *user_data)
{
	struct can_renesas_rz_data *data = dev->data;
	const struct can_renesas_rz_cfg *cfg = dev->config;

	if (!data->common.started) {
		return -ENETDOWN;
	}
	if ((data->common.mode & CAN_MODE_FD) != 0) {
		if (frame->dlc > CANFD_MAX_DLC) {
			return -EINVAL;
		}
		if ((frame->flags & CAN_FRAME_ESI) != 0) {
			return -ENOTSUP;
		}
	} else {
		if ((frame->flags & CAN_FRAME_FDF) != 0) {
			return -ENOTSUP;
		}
		if (frame->dlc > CAN_MAX_DLC) {
			return -EINVAL;
		}
	}

	if (k_sem_take(&data->tx_sem, timeout) != 0) {
		return -EAGAIN;
	}

	k_mutex_lock(&data->inst_mutex, K_FOREVER);
	int ret = 0;

	data->tx_cb = callback;
	data->tx_usr_data = user_data;

	can_frame_t fsp_frame = {
		.id = frame->id,
		.id_mode = ((frame->flags & CAN_FRAME_IDE) != 0) ? CAN_ID_MODE_EXTENDED
								 : CAN_ID_MODE_STANDARD,
		.type = ((frame->flags & CAN_FRAME_RTR) != 0) ? CAN_FRAME_TYPE_REMOTE
							      : CAN_FRAME_TYPE_DATA,
		.data_length_code = can_dlc_to_bytes(frame->dlc),
		.options =
			((((frame->flags & CAN_FRAME_FDF) != 0) ? CANFD_FRAME_OPTION_FD : 0UL) |
			 (((frame->flags & CAN_FRAME_BRS) != 0) ? CANFD_FRAME_OPTION_BRS : 0UL) |
			 (((frame->flags & CAN_FRAME_ESI) != 0) ? CANFD_FRAME_OPTION_ERROR : 0UL)),
	};
	memcpy(fsp_frame.data, frame->data, fsp_frame.data_length_code);

	if (FSP_SUCCESS != cfg->fsp_api->write(data->fsp_ctrl, CANFD_TX_MB_0, &fsp_frame)) {
		LOG_DBG("CAN transmit failed");
		data->tx_cb = NULL;
		data->tx_usr_data = NULL;
		k_sem_give(&data->tx_sem);
		ret = -EIO;
	}

	k_mutex_unlock(&data->inst_mutex);
	return ret;
}

static int can_renesas_rz_add_rx_filter(const struct device *dev, can_rx_callback_t callback,
					void *user_data, const struct can_filter *filter)
{
	struct can_renesas_rz_data *data = dev->data;

	k_mutex_lock(&data->inst_mutex, K_FOREVER);

	int filter_id = get_free_filter_id(dev);

	if (filter_id == -ENOSPC) {
		goto end;
	}

	set_afl_rule(dev, filter, filter_id);

	memcpy(&data->rx_filter[filter_id].filter, filter, sizeof(struct can_filter));
	data->rx_filter[filter_id].rx_cb = callback;
	data->rx_filter[filter_id].rx_usr_data = user_data;
	data->rx_filter[filter_id].set = true;

end:
	k_mutex_unlock(&data->inst_mutex);

	return filter_id;
}

static void can_renesas_rz_remove_rx_filter(const struct device *dev, int filter_id)
{
	struct can_renesas_rz_data *data = dev->data;
	const struct can_renesas_rz_cfg *cfg = dev->config;

	if ((filter_id < 0) || (filter_id >= cfg->rx_filter_num)) {
		return;
	}

	k_mutex_lock(&data->inst_mutex, K_FOREVER);

	remove_afl_rule(dev, filter_id);

	data->rx_filter[filter_id].rx_cb = NULL;
	data->rx_filter[filter_id].rx_usr_data = NULL;
	data->rx_filter[filter_id].set = false;

	k_mutex_unlock(&data->inst_mutex);
}

#ifdef CONFIG_CAN_MANUAL_RECOVERY_MODE
static int can_renesas_rz_recover(const struct device *dev, k_timeout_t timeout)
{
	struct can_renesas_rz_data *data = dev->data;

	if (!data->common.started) {
		return -ENETDOWN;
	}

	if ((data->common.mode & CAN_MODE_MANUAL_RECOVERY) == 0) {
		return -ENOTSUP;
	}

	return recover_bus(dev, timeout);
}
#endif

static int can_renesas_rz_get_state(const struct device *dev, enum can_state *state,
				    struct can_bus_err_cnt *err_cnt)
{
	struct can_renesas_rz_data *data = dev->data;
	const struct can_renesas_rz_cfg *cfg = dev->config;

	if (!data->common.started) {
		if (state != NULL) {
			*state = CAN_STATE_STOPPED;
		}
	} else {
		can_info_t fsp_info = {0};

		if (FSP_SUCCESS != cfg->fsp_api->infoGet(data->fsp_ctrl, &fsp_info)) {
			LOG_DBG("CAN get state info failed");
			return -EIO;
		}

		if (err_cnt != NULL) {
			err_cnt->tx_err_cnt = fsp_info.error_count_transmit;
			err_cnt->rx_err_cnt = fsp_info.error_count_receive;
		}

		if (state != NULL) {
			if ((fsp_info.error_code & R_CANFD_CFDC_ERFL_BOEF_Msk) != 0) {
				*state = CAN_STATE_BUS_OFF;
			} else if ((fsp_info.error_code & R_CANFD_CFDC_ERFL_EPF_Msk) != 0) {
				*state = CAN_STATE_ERROR_PASSIVE;
			} else if ((fsp_info.error_code & R_CANFD_CFDC_ERFL_EWF_Msk) != 0) {
				*state = CAN_STATE_ERROR_WARNING;
			} else if ((fsp_info.error_code & R_CANFD_CFDC_ERFL_BEF_Msk) != 0) {
				*state = CAN_STATE_ERROR_ACTIVE;
			}
		}
	}

	return 0;
}

static void can_renesas_rz_set_state_change_callback(const struct device *dev,
						     can_state_change_callback_t callback,
						     void *user_data)
{
	struct can_renesas_rz_data *data = dev->data;
	canfd_instance_ctrl_t *p_ctrl = data->fsp_ctrl;
	int key = irq_lock();

	k_mutex_lock(&data->inst_mutex, K_FOREVER);

	if (callback != NULL) {
		/* Enable state change interrupt */
		p_ctrl->p_reg->CFDC->CTR |= (uint32_t)CANFD_CFG_ERR_IRQ;
	} else {
		/* Disable state change interrupt */
		p_ctrl->p_reg->CFDC->CTR &= (uint32_t)~CANFD_CFG_ERR_IRQ;

		/* Clear error flags */
		p_ctrl->p_reg->CFDC->ERFL &=
			~(BIT(R_CANFD_CFDC_ERFL_BOEF_Pos) | BIT(R_CANFD_CFDC_ERFL_EWF_Pos) |
			BIT(R_CANFD_CFDC_ERFL_EPF_Pos) | BIT(R_CANFD_CFDC_ERFL_BEF_Pos));
	}

	data->common.state_change_cb = callback;
	data->common.state_change_cb_user_data = user_data;

	k_mutex_unlock(&data->inst_mutex);

	irq_unlock(key);
}

static int can_renesas_rz_get_core_clock(const struct device *dev, uint32_t *rate)
{
	const struct can_renesas_rz_cfg *cfg = dev->config;
	int ret = 0;

	if (cfg->clock_ext_rate) {
		*rate = cfg->clock_ext_rate;
	} else {
		uint32_t canfd_pclk = 0;

		ret = clock_control_get_rate(cfg->clock_dev, cfg->clock_subsys, &canfd_pclk);

		if (ret < 0) {
			return ret;
		}

		*rate = canfd_pclk / 2;
	}

	return ret;
}

static int can_renesas_rz_get_max_filters(const struct device *dev, bool ide)
{
	ARG_UNUSED(ide);
	const struct can_renesas_rz_cfg *cfg = dev->config;

	return cfg->rx_filter_num;
}

#ifdef CONFIG_CAN_FD_MODE
static int can_renesas_rz_set_timing_data(const struct device *dev,
					  const struct can_timing *timing_data)
{
	struct can_renesas_rz_data *data = dev->data;

	if (data->common.started) {
		return -EBUSY;
	}

	return set_hw_timing_configuration(dev, &data->data_timing, timing_data);
}
#endif /* CONFIG_CAN_FD_MODE */

void can_renesas_rz_fsp_cb(can_callback_args_t *p_args)
{
	const struct device *dev = p_args->p_context;

	switch (p_args->event) {
	case CAN_EVENT_RX_COMPLETE: {
		can_renesas_rz_call_rx_cb(dev, p_args);
		break;
	}

	case CAN_EVENT_TX_COMPLETE: {
		can_renesas_rz_call_tx_cb(dev, 0);
		break;
	}

	case CAN_EVENT_ERR_CHANNEL: {
		if ((p_args->error & R_CANFD_CFDC_ERFL_BEF_Msk) != 0) {
			can_renesas_rz_call_state_change_cb(dev, CAN_STATE_ERROR_ACTIVE);
		}
		if ((p_args->error & R_CANFD_CFDC_ERFL_EWF_Msk) != 0) {
			can_renesas_rz_call_state_change_cb(dev, CAN_STATE_ERROR_WARNING);
		}
		if ((p_args->error & R_CANFD_CFDC_ERFL_EPF_Msk) != 0) {
			can_renesas_rz_call_state_change_cb(dev, CAN_STATE_ERROR_PASSIVE);
		}
		if ((p_args->error & R_CANFD_CFDC_ERFL_BOEF_Msk) != 0) {
			can_renesas_rz_call_state_change_cb(dev, CAN_STATE_BUS_OFF);
		}

		if ((p_args->error & R_CANFD_CFDC_ERFL_ALF_Msk) != 0) { /* Arbitration Lost Error */
			can_renesas_rz_call_tx_cb(dev, -EBUSY);
		}
		if ((p_args->error & (R_CANFD_CFDC_ERFL_AERR_Msk |  /* ACK Error */
				      R_CANFD_CFDC_ERFL_ADERR_Msk | /* ACK Delimiter Error */
				      R_CANFD_CFDC_ERFL_B1ERR_Msk | /* Bit 1 Error */
				      R_CANFD_CFDC_ERFL_B0ERR_Msk)) /* Bit 0 Error */
		    != 0) {
			can_renesas_rz_call_tx_cb(dev, -EIO);
		}
	}

	default:
		break;
	}
}

static inline int can_renesas_rz_apply_default_config(const struct device *dev)
{
	struct can_renesas_rz_cfg *cfg = (struct can_renesas_rz_cfg *)dev->config;
	int ret;
	struct can_timing timing = {0};

	ret = can_calc_timing(dev, &timing, cfg->common.bitrate, cfg->common.sample_point);

	if (ret < 0) {
		return ret;
	}

	ret = can_renesas_rz_set_timing(dev, &timing);
	if (ret != 0) {
		return ret;
	}
#ifdef CONFIG_CAN_FD_MODE
	can_calc_timing_data(dev, &timing, cfg->common.bitrate_data, cfg->common.sample_point_data);

	ret = can_renesas_rz_set_timing_data(dev, &timing);
	if (ret < 0) {
		return ret;
	}
#endif
	for (uint32_t filter_id = 0; filter_id < cfg->rx_filter_num; filter_id++) {
		remove_afl_rule(dev, filter_id);
	}

	return 0;
}

static int can_renesas_rz_init(const struct device *dev)
{
	const struct can_renesas_rz_cfg *cfg = dev->config;
	struct can_renesas_rz_data *data = dev->data;
	canfd_extended_cfg_t *p_extend = (canfd_extended_cfg_t *)data->fsp_cfg->p_extend;
	int ret = 0;

	k_mutex_init(&data->inst_mutex);
	k_sem_init(&data->tx_sem, 1, 1);
	data->common.started = false;

	/* Check external clock is used */
	if (cfg->clock_ext_rate) {
		p_extend->p_global_cfg->global_config |= R_CANFD_CFDGCFG_DCS_Msk;
	}

	/* Configure dt provided device signals when available */
	ret = pinctrl_apply_state(cfg->pin_config, PINCTRL_STATE_DEFAULT);
	if (ret) {
		LOG_ERR("pin function initial failed");
		return ret;
	}

	/* Apply config and setting for CAN controller HW */
	ret = can_renesas_rz_apply_default_config(dev);
	if (ret) {
		LOG_ERR("invalid default configuration");
		return ret;
	}

	ret = cfg->fsp_api->open(data->fsp_ctrl, data->fsp_cfg);
	if (ret != FSP_SUCCESS) {
		LOG_ERR("CAN bus initialize failed");
		return -EIO;
	}

	/* Put CAN controller into stopped state */
	ret = cfg->fsp_api->modeTransition(data->fsp_ctrl, CAN_OPERATION_MODE_HALT,
					   CAN_TEST_MODE_DISABLED);
	if (ret != FSP_SUCCESS) {
		cfg->fsp_api->close(data->fsp_ctrl);
		LOG_ERR("CAN stop failed");
		return -EIO;
	}

	return 0;
}

static DEVICE_API(can, can_renesas_rz_driver_api) = {
	.get_capabilities = can_renesas_rz_get_capabilities,
	.start = can_renesas_rz_start,
	.stop = can_renesas_rz_stop,
	.set_mode = can_renesas_rz_set_mode,
	.set_timing = can_renesas_rz_set_timing,
	.send = can_renesas_rz_send,
	.add_rx_filter = can_renesas_rz_add_rx_filter,
	.remove_rx_filter = can_renesas_rz_remove_rx_filter,
#if defined(CONFIG_CAN_MANUAL_RECOVERY_MODE)
	.recover = can_renesas_rz_recover,
#endif /* CONFIG_CAN_MANUAL_RECOVERY_MODE */
	.get_state = can_renesas_rz_get_state,
	.set_state_change_callback = can_renesas_rz_set_state_change_callback,
	.get_core_clock = can_renesas_rz_get_core_clock,
	.get_max_filters = can_renesas_rz_get_max_filters,
	.timing_min = CAN_RENESAS_RZ_TIMING_MIN,
	.timing_max = CAN_RENESAS_RZ_TIMING_MAX,
#if defined(CONFIG_CAN_FD_MODE)
	.set_timing_data = can_renesas_rz_set_timing_data,
	.timing_data_min = CAN_RENESAS_RZ_TIMING_DATA_MIN,
	.timing_data_max = CAN_RENESAS_RZ_TIMING_DATA_MAX,
#endif /* CONFIG_CAN_FD_MODE */
};

#define CAN_RENESAS_RZ_GLOBAL_IRQ_INIT()                                                           \
	IRQ_CONNECT(DT_IRQ_BY_NAME(DT_COMPAT_GET_ANY_STATUS_OKAY(renesas_rz_canfd_global), g_err,  \
				   irq),                                                           \
		    DT_IRQ_BY_NAME(DT_COMPAT_GET_ANY_STATUS_OKAY(renesas_rz_canfd_global), g_err,  \
				   priority),                                                      \
		    canfd_error_isr, NULL, 0);                                                     \
	IRQ_CONNECT(DT_IRQ_BY_NAME(DT_COMPAT_GET_ANY_STATUS_OKAY(renesas_rz_canfd_global), g_recc, \
				   irq),                                                           \
		    DT_IRQ_BY_NAME(DT_COMPAT_GET_ANY_STATUS_OKAY(renesas_rz_canfd_global), g_recc, \
				   priority),                                                      \
		    canfd_rx_fifo_isr, NULL, 0);                                                   \
	irq_enable(DT_IRQ_BY_NAME(DT_COMPAT_GET_ANY_STATUS_OKAY(renesas_rz_canfd_global), g_err,   \
				  irq));                                                           \
	irq_enable(DT_IRQ_BY_NAME(DT_COMPAT_GET_ANY_STATUS_OKAY(renesas_rz_canfd_global), g_recc,  \
				  irq));

static int can_renesas_rz_global_init(const struct device *dev)
{
	CAN_RENESAS_RZ_GLOBAL_IRQ_INIT();

	return 0;
}

DEVICE_DT_DEFINE(DT_COMPAT_GET_ANY_STATUS_OKAY(renesas_rz_canfd_global), can_renesas_rz_global_init,
		 NULL, NULL, NULL, PRE_KERNEL_2, CONFIG_CAN_INIT_PRIORITY, NULL)

#define CAN_RENESAS_RZ_CHANNEL_IRQ_INIT(index)                                                     \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQ_BY_NAME(index, ch_rec, irq),                               \
			    DT_INST_IRQ_BY_NAME(index, ch_rec, priority), canfd_rx_fifo_isr, NULL, \
			    0);                                                                    \
		IRQ_CONNECT(DT_INST_IRQ_BY_NAME(index, ch_trx, irq),                               \
			    DT_INST_IRQ_BY_NAME(index, ch_trx, priority), canfd_channel_tx_isr,    \
			    NULL, 0);                                                              \
		IRQ_CONNECT(DT_INST_IRQ_BY_NAME(index, ch_err, irq),                               \
			    DT_INST_IRQ_BY_NAME(index, ch_err, priority), canfd_error_isr, NULL,   \
			    0);                                                                    \
	                                                                                           \
		irq_enable(DT_INST_IRQ_BY_NAME(index, ch_rec, irq));                               \
		irq_enable(DT_INST_IRQ_BY_NAME(index, ch_trx, irq));                               \
		irq_enable(DT_INST_IRQ_BY_NAME(index, ch_err, irq));                               \
	}

#define CAN_RENESAS_RZG_INIT(index)                                                                \
	PINCTRL_DT_INST_DEFINE(index);                                                             \
	static canfd_global_cfg_t g_canfd_global_cfg = {                                           \
		.global_interrupts = CANFD_CFG_GLOBAL_ERR_SOURCES,                                 \
		.global_config = (CANFD_CFG_TX_PRIORITY | CANFD_CFG_DLC_CHECK |                    \
				  CANFD_CFD_CLOCK_SOURCE | CANFD_CFG_FD_OVERFLOW),                 \
		.rx_mb_config = (CANFD_CFG_RXMB_NUMBER |                                           \
				 (CANFD_CFG_RXMB_SIZE << R_CANFD_CFDRMNB_RMPLS_Pos)),              \
		.global_err_ipl = DT_IRQ_BY_NAME(                                                  \
			DT_COMPAT_GET_ANY_STATUS_OKAY(renesas_rz_canfd_global), g_err, priority),  \
		.rx_fifo_ipl = DT_IRQ_BY_NAME(                                                     \
			DT_COMPAT_GET_ANY_STATUS_OKAY(renesas_rz_canfd_global), g_recc, priority), \
		.rx_fifo_config =                                                                  \
			{                                                                          \
				CANFD_CFG_RXFIFO_DEFAULT(0),                                       \
				CANFD_CFG_RXFIFO_DEFAULT(1),                                       \
				CANFD_CFG_RXFIFO_DEFAULT(2),                                       \
				CANFD_CFG_RXFIFO_DEFAULT(3),                                       \
				CANFD_CFG_RXFIFO_DEFAULT(4),                                       \
				CANFD_CFG_RXFIFO_DEFAULT(5),                                       \
				CANFD_CFG_RXFIFO_DEFAULT(6),                                       \
				CANFD_CFG_RXFIFO_DEFAULT(7),                                       \
			},                                                                         \
	};                                                                                         \
	static canfd_afl_entry_t p_canfd_ch##index##_afl[DT_INST_PROP(index, rx_max_filters)];     \
	struct can_renesas_rz_filter                                                               \
		can_renesas_rz_rx_filter##index[DT_INST_PROP(index, rx_max_filters)];              \
	static can_bit_timing_cfg_t g_canfd_ch##index##_bit_timing_cfg;                            \
	uint32_t clock_subsys##index = DT_INST_CLOCKS_CELL(index, clk_id);                         \
	static const struct can_renesas_rz_cfg can_renesas_rz_cfg##index = {                       \
		.common = CAN_DT_DRIVER_CONFIG_INST_GET(index, 0, 8000000),                        \
		.global_dev = DEVICE_DT_GET(DT_INST_PARENT(index)),                                \
		.pin_config = PINCTRL_DT_INST_DEV_CONFIG_GET(index),                               \
		.rx_filter_num = DT_INST_PROP(index, rx_max_filters),                              \
		.fsp_api = &g_canfd_on_canfd,                                                      \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(index)),                            \
		.clock_subsys = (clock_control_subsys_t)(&clock_subsys##index),                    \
		.clock_ext_rate = DT_PROP_OR(DT_NODELABEL(can_clk), clock_frequency, 0),           \
	};                                                                                         \
	static canfd_instance_ctrl_t g_canfd_ch##index##_ctrl;                                     \
	static canfd_extended_cfg_t g_canfd_ch##index##_extern_cfg = {                             \
		.p_afl = p_canfd_ch##index##_afl,                                                  \
		.txmb_txi_enable = CANFD_CFG_TXMB_TXI_ENABLE,                                      \
		.error_interrupts = 0U,                                                            \
		.p_global_cfg = &g_canfd_global_cfg,                                               \
	};                                                                                         \
	static can_cfg_t g_canfd_ch##index##cfg = {                                                \
		.channel = DT_INST_PROP(index, channel),                                           \
		.ipl = DT_INST_IRQ_BY_NAME(index, ch_err, priority),                               \
		.error_irq = DT_INST_IRQ_BY_NAME(index, ch_err, irq),                              \
		.rx_irq = DT_INST_IRQ_BY_NAME(index, ch_rec, irq),                                 \
		.tx_irq = DT_INST_IRQ_BY_NAME(index, ch_trx, irq),                                 \
		.p_extend = &g_canfd_ch##index##_extern_cfg,                                       \
		.p_bit_timing = &g_canfd_ch##index##_bit_timing_cfg,                               \
		.p_context = DEVICE_DT_INST_GET(index),                                            \
		.p_callback = can_renesas_rz_fsp_cb,                                               \
	};                                                                                         \
	static struct can_renesas_rz_data can_renesas_rz_data##index = {                           \
		.fsp_ctrl = &g_canfd_ch##index##_ctrl,                                             \
		.fsp_cfg = &g_canfd_ch##index##cfg,                                                \
		.rx_filter = can_renesas_rz_rx_filter##index,                                      \
	};                                                                                         \
	static int can_renesas_rz_init##index(const struct device *dev)                            \
	{                                                                                          \
		const struct device *global_canfd = DEVICE_DT_GET(DT_INST_PARENT(index));          \
		if (!device_is_ready(global_canfd)) {                                              \
			return -EIO;                                                               \
		}                                                                                  \
		CAN_RENESAS_RZ_CHANNEL_IRQ_INIT(index)                                             \
		return can_renesas_rz_init(dev);                                                   \
	}                                                                                          \
	CAN_DEVICE_DT_INST_DEFINE(index, can_renesas_rz_init##index, NULL,                         \
				  &can_renesas_rz_data##index, &can_renesas_rz_cfg##index,         \
				  POST_KERNEL, CONFIG_CAN_INIT_PRIORITY,                           \
				  &can_renesas_rz_driver_api);

DT_INST_FOREACH_STATUS_OKAY(CAN_RENESAS_RZG_INIT)
