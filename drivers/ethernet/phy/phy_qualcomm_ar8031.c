/*
 * Copyright 2024 NXP
 *
 * Inspiration from phy_mii.c, which is:
 * Copyright (c) 2021 IP-Logix Inc.
 * Copyright 2022 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT qca_ar8031

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/mdio.h>
#include <zephyr/net/phy.h>
#include <zephyr/net/mdio.h>
#include <zephyr/net/mii.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(phy_qc_ar8031, CONFIG_PHY_LOG_LEVEL);

#include "phy_mii.h"

#define AR8031_PHY_ID1           0x004DU
#define PHY_READID_TIMEOUT_COUNT 1000U

/* PHY Specific Status Register */
#define PHY_SPECIFIC_STATUS_REG     0x11
#define SPEC_STATUS_REG_LINK_MASK   (1U << 10)
#define SPEC_STATUS_REG_DUPLEX_MASK (1U << 13)
#define PHY_DUPLEX_HALF             (0U << 13)
#define PHY_DUPLEX_FULL             (1U << 13)
#define SPEC_STATUS_REG_SPEED_MASK  (0x3U << 14)
#define PHY_SPEED_10M               (0U << 14)
#define PHY_SPEED_100M              (1U << 14)
#define PHY_SPEED_1000M             (2U << 14)

/* The PHY Debug port address register */
#define PHY_DEBUGPORT_ADDR_REG 0x1DU
/* The PHY Debug port data register */
#define PHY_DEBUGPORT_DATA_REG 0x1EU

/* PCS Register: smartEEE control 3 Register */
#define MDIO_PCS_SMARTEEE_CTRL3            0x805DU
#define MDIO_PCS_SMARTEEE_CTRL3_LPI_EN     (1U << 8)

/* Debug port registers */
/* Analog Test Control */
#define PHY_DEBUGPORT_ANALOG_CTRL          0x0
#define PHY_DEBUGPORT_ANALOG_CTRL_RX_DELAY (1U << 15)
/* SerDes Test and System Mode Control */
#define PHY_DEBUGPORT_SD_SM_CTRL           0x5
#define PHY_DEBUGPORT_SD_SM_CTRL_TX_DELAY  (1U << 8)

struct qc_ar8031_config {
	uint8_t addr;
	bool enable_eee;
	enum phy_link_speed default_speeds;
	const struct device *mdio_dev;
};

struct qc_ar8031_data {
	const struct device *dev;
	phy_callback_t cb;
	void *cb_data;
	struct k_work_delayable monitor_work;
	struct phy_link_state state;
	struct k_sem sem;
};

static int qc_ar8031_get_link_state(const struct device *dev, struct phy_link_state *state);

static int qc_ar8031_read(const struct device *dev, uint16_t reg_addr, uint32_t *data)
{
	const struct qc_ar8031_config *config = dev->config;
	int ret;

	/* Make sure excessive bits 16-31 are reset */
	*data = 0U;

	/* Read the PHY register */
	ret = mdio_read(config->mdio_dev, config->addr, reg_addr, (uint16_t *)data);
	if (ret) {
		return ret;
	}

	return 0;
}

static int qc_ar8031_write(const struct device *dev, uint16_t reg_addr, uint32_t data)
{
	const struct qc_ar8031_config *config = dev->config;
	int ret;

	ret = mdio_write(config->mdio_dev, config->addr, reg_addr, (uint16_t)data);
	if (ret) {
		return ret;
	}

	return 0;
}

static int qc_ar8031_mmd_set_device(const struct device *dev, uint32_t device, uint32_t addr,
				    uint16_t mode)
{
	uint16_t reg_value = (device & MII_MMD_ACR_DEVAD_MASK) | MII_MMD_ACR_ADDR;

	if (qc_ar8031_write(dev, MII_MMD_ACR, reg_value) < 0) {
		return -EIO;
	}
	if (qc_ar8031_write(dev, MII_MMD_AADR, addr) < 0) {
		return -EIO;
	}
	/* Set Function mode of data access(b01~11) and device address. */
	if (qc_ar8031_write(dev, MII_MMD_ACR, (device & MII_MMD_ACR_DEVAD_MASK) | mode) < 0) {
		return -EIO;
	}

	return 0;
}

static int qc_ar8031_mmd_read(const struct device *dev, uint32_t device, uint32_t addr,
			      uint32_t *data)
{
	int ret;

	*data = 0U;

	ret = qc_ar8031_mmd_set_device(dev, device, addr, MII_MMD_ACR_DATA_NO_POS_INC);
	if (ret) {
		return -EIO;
	}

	ret = qc_ar8031_read(dev, MII_MMD_AADR, data);

	return ret;
}

static int qc_ar8031_mmd_write(const struct device *dev, uint32_t device, uint32_t addr,
			       uint32_t data)
{
	int ret;

	ret = qc_ar8031_mmd_set_device(dev, device, addr, MII_MMD_ACR_DATA_NO_POS_INC);
	if (ret) {
		return -EIO;
	}

	ret = qc_ar8031_write(dev, MII_MMD_AADR, data);

	return ret;
}

static int qc_ar8031_update_link_state(const struct device *dev)
{
	const struct qc_ar8031_config *const cfg = dev->config;
	struct qc_ar8031_data *const data = dev->data;
	bool link_up;
	uint32_t reg_value;
	uint16_t speed, duplex;

	if (qc_ar8031_read(dev, PHY_SPECIFIC_STATUS_REG, &reg_value) < 0) {
		return -EIO;
	}

	link_up = (uint16_t)reg_value & SPEC_STATUS_REG_LINK_MASK;

	/* If there is no change in link state don't proceed. */
	if (link_up == data->state.is_up) {
		return -EAGAIN;
	}

	data->state.is_up = link_up;

	/* If link is down, there is nothing more to be done */
	if (data->state.is_up == false) {
		return 0;
	}

	if (qc_ar8031_read(dev, PHY_SPECIFIC_STATUS_REG, &reg_value) < 0) {
		return -EIO;
	}

	speed = reg_value & SPEC_STATUS_REG_SPEED_MASK;
	duplex = reg_value & SPEC_STATUS_REG_DUPLEX_MASK;

	switch (speed | duplex) {
	case PHY_SPEED_10M | PHY_DUPLEX_FULL:
		data->state.speed = LINK_FULL_10BASE;
		break;
	case PHY_SPEED_10M | PHY_DUPLEX_HALF:
		data->state.speed = LINK_HALF_10BASE;
		break;
	case PHY_SPEED_100M | PHY_DUPLEX_FULL:
		data->state.speed = LINK_FULL_100BASE;
		break;
	case PHY_SPEED_100M | PHY_DUPLEX_HALF:
		data->state.speed = LINK_HALF_100BASE;
		break;
	case PHY_SPEED_1000M | PHY_DUPLEX_FULL:
		data->state.speed = LINK_FULL_1000BASE;
		break;
	case PHY_SPEED_1000M | PHY_DUPLEX_HALF:
		data->state.speed = LINK_HALF_1000BASE;
		break;
	}

	LOG_DBG("PHY (%d) Link speed %s Mb, %s duplex", cfg->addr,
		PHY_LINK_IS_SPEED_1000M(data->state.speed)
			? "1000"
			: (PHY_LINK_IS_SPEED_100M(data->state.speed) ? "100" : "10"),
		PHY_LINK_IS_FULL_DUPLEX(data->state.speed) ? "full" : "half");

	return 0;
}

static void invoke_link_cb(const struct device *dev)
{
	struct qc_ar8031_data *const data = dev->data;
	struct phy_link_state state;

	if (data->cb == NULL) {
		return;
	}

	qc_ar8031_get_link_state(dev, &state);

	data->cb(data->dev, &state, data->cb_data);
}

static void monitor_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct qc_ar8031_data *const data =
		CONTAINER_OF(dwork, struct qc_ar8031_data, monitor_work);
	const struct device *dev = data->dev;
	int rc;

	k_sem_take(&data->sem, K_FOREVER);

	rc = qc_ar8031_update_link_state(dev);

	k_sem_give(&data->sem);

	/* If link state has changed and a callback is set, invoke callback */
	if (rc == 0) {
		invoke_link_cb(dev);
	}

	/* Submit delayed work */
	k_work_reschedule(&data->monitor_work, K_MSEC(CONFIG_PHY_MONITOR_PERIOD));
}

static int qc_ar8031_cfg_link(const struct device *dev, enum phy_link_speed speeds,
			      enum phy_cfg_link_flag flags)
{
	uint32_t bmcr_reg;
	int ret;

	if (flags & PHY_FLAG_AUTO_NEGOTIATION_DISABLED) {
		LOG_ERR("Disabling auto-negotiation is not supported by this driver");
		return -ENOTSUP;
	}

	if (qc_ar8031_read(dev, MII_BMCR, &bmcr_reg) < 0) {
		return -EIO;
	}

	ret = phy_mii_set_anar_reg(dev, speeds);
	if ((ret < 0) && (ret != -EALREADY)) {
		return -EIO;
	}

	ret = phy_mii_set_c1kt_reg(dev, speeds);
	if ((ret < 0) && (ret != -EALREADY)) {
		return -EIO;
	}

	bmcr_reg |= MII_BMCR_AUTONEG_ENABLE | MII_BMCR_AUTONEG_RESTART;

	if (qc_ar8031_write(dev, MII_BMCR, bmcr_reg) < 0) {
		return -EIO;
	}

	return 0;
}

static int qc_ar8031_get_link_state(const struct device *dev, struct phy_link_state *state)
{
	struct qc_ar8031_data *const data = dev->data;

	k_sem_take(&data->sem, K_FOREVER);

	memcpy(state, &data->state, sizeof(struct phy_link_state));

	k_sem_give(&data->sem);

	return 0;
}

static int qc_ar8031_link_cb_set(const struct device *dev, phy_callback_t cb, void *user_data)
{
	struct qc_ar8031_data *const data = dev->data;

	data->cb = cb;
	data->cb_data = user_data;

	/**
	 * Immediately invoke the callback to notify the caller of the
	 * current link status.
	 */
	invoke_link_cb(dev);

	return 0;
}

static int qc_ar8031_init(const struct device *dev)
{
	const struct qc_ar8031_config *const cfg = dev->config;
	struct qc_ar8031_data *const data = dev->data;
	uint32_t counter = PHY_READID_TIMEOUT_COUNT;
	uint32_t reg_value = 0;
	int ret;

	k_sem_init(&data->sem, 1, 1);

	mdio_bus_enable(cfg->mdio_dev);

	data->state.is_up = false;

	data->dev = dev;
	data->cb = NULL;

	do {
		if (qc_ar8031_read(dev, MII_PHYID1R, &reg_value) < 0) {
			return -EIO;
		}
	} while (reg_value != AR8031_PHY_ID1 && counter-- > 0);
	if (counter == 0U) {
		LOG_ERR("PHY (%d) can't read ID", cfg->addr);
		return -EIO;
	}

	/* Reset PHY */
	ret = qc_ar8031_write(dev, MII_BMCR, MII_BMCR_RESET);
	if (ret) {
		return -EIO;
	}

	/* Close smartEEE */
	ret = qc_ar8031_mmd_set_device(dev, MDIO_MMD_PCS, MDIO_PCS_SMARTEEE_CTRL3,
				       MII_MMD_ACR_DATA_NO_POS_INC);
	if (ret) {
		return -EIO;
	}
	ret = qc_ar8031_read(dev, MII_MMD_AADR, &reg_value);
	if (ret) {
		return -EIO;
	}
	ret = qc_ar8031_write(dev, MII_MMD_AADR, reg_value & ~(MDIO_PCS_SMARTEEE_CTRL3_LPI_EN));
	if (ret) {
		return -EIO;
	}

	/* Enable Tx clock delay */
	ret = qc_ar8031_write(dev, PHY_DEBUGPORT_ADDR_REG, PHY_DEBUGPORT_SD_SM_CTRL);
	if (ret) {
		return -EIO;
	}
	ret = qc_ar8031_read(dev, PHY_DEBUGPORT_DATA_REG, &reg_value);
	if (ret) {
		return -EIO;
	}
	ret = qc_ar8031_write(dev, PHY_DEBUGPORT_DATA_REG,
			      reg_value | PHY_DEBUGPORT_SD_SM_CTRL_TX_DELAY);
	if (ret) {
		return -EIO;
	}

	/* Enable Rx clock delay */
	ret = qc_ar8031_write(dev, PHY_DEBUGPORT_ADDR_REG, PHY_DEBUGPORT_ANALOG_CTRL);
	if (ret) {
		return -EIO;
	}
	ret = qc_ar8031_read(dev, PHY_DEBUGPORT_DATA_REG, &reg_value);
	if (ret) {
		return -EIO;
	}
	ret = qc_ar8031_write(dev, PHY_DEBUGPORT_DATA_REG,
			      reg_value | PHY_DEBUGPORT_ANALOG_CTRL_RX_DELAY);
	if (ret) {
		return -EIO;
	}

	/* Energy Efficient Ethernet configuration */
	if (cfg->enable_eee) {
		ret = qc_ar8031_mmd_read(dev, MDIO_MMD_PCS, MDIO_PCS_EEE_CAP, &reg_value);
		if (ret) {
			return -EIO;
		}
		ret = qc_ar8031_mmd_write(dev, MDIO_MMD_AN, MDIO_AN_EEE_ADV,
			reg_value & (MDIO_AN_EEE_ADV_1000T | MDIO_AN_EEE_ADV_100TX));
		if (ret) {
			return -EIO;
		}
	} else {
		ret = qc_ar8031_mmd_write(dev, MDIO_MMD_AN, MDIO_AN_EEE_ADV, 0);
		if (ret) {
			return -EIO;
		}
	}

	/* Advertise default speeds */
	qc_ar8031_cfg_link(dev, cfg->default_speeds, 0);

	k_work_init_delayable(&data->monitor_work, monitor_work_handler);

	monitor_work_handler(&data->monitor_work.work);

	return 0;
}

static DEVICE_API(ethphy, ar8031_driver_api) = {
	.get_link = qc_ar8031_get_link_state,
	.cfg_link = qc_ar8031_cfg_link,
	.link_cb_set = qc_ar8031_link_cb_set,
	.read = qc_ar8031_read,
	.write = qc_ar8031_write,
};

#define AR8031_CONFIG(n)                                                                           \
	static const struct qc_ar8031_config qc_ar8031_config_##n = {                              \
		.addr = DT_INST_REG_ADDR(n),                                                       \
		.mdio_dev = DEVICE_DT_GET(DT_INST_BUS(n)),                                         \
		.default_speeds = PHY_INST_GENERATE_DEFAULT_SPEEDS(n),				   \
		.enable_eee = DT_INST_NODE_HAS_PROP(n, eee_en),                                    \
	};

#define AR8031_DEVICE(n)                                                                           \
	AR8031_CONFIG(n);                                                                          \
	static struct qc_ar8031_data qc_ar8031_data_##n;                                           \
	DEVICE_DT_INST_DEFINE(n, &qc_ar8031_init, NULL, &qc_ar8031_data_##n,                       \
			      &qc_ar8031_config_##n, POST_KERNEL, CONFIG_PHY_INIT_PRIORITY,        \
			      &ar8031_driver_api);

DT_INST_FOREACH_STATUS_OKAY(AR8031_DEVICE)
