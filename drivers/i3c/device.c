// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2017 Cadence Design Systems Inc.
 *
 * Author: Boris Brezillon <boris.brezillon@free-electrons.com>
 */

#include <linux/slab.h>

#include "internals.h"

/**
 * i3c_device_do_priv_xfers() - do I3C SDR private transfers directed to a
 *				specific device
 *
 * @dev: device with which the transfers should be done
 * @xfers: array of transfers
 * @nxfers: number of transfers
 *
 * Initiate one or several private SDR transfers with @dev.
 *
 * This function can sleep and thus cannot be called in atomic context.
 *
 * Return: 0 in case of success, a negative error core otherwise.
 */
int i3c_device_do_priv_xfers(struct i3c_device *dev,
			     struct i3c_priv_xfer *xfers,
			     int nxfers)
{
	struct i3c_master_controller *master;
	int i, ret;

	master = i3c_device_get_master(dev);
	if (!master)
		return -EINVAL;

	i3c_bus_normaluse_lock(master->bus);
	for (i = 0; i < nxfers; i++)
		xfers[i].addr = dev->info.dyn_addr;

	ret = i3c_master_do_priv_xfers_locked(master, xfers, nxfers);
	i3c_bus_normaluse_unlock(master->bus);

	return ret;
}
EXPORT_SYMBOL_GPL(i3c_device_do_priv_xfers);

/**
 * i3c_device_send_hdr_cmds() - send HDR commands to a specific device
 *
 * @dev: device to which these commands should be sent
 * @cmds: array of commands
 * @ncmds: number of commands
 *
 * Send one or several HDR commands to @dev.
 *
 * This function can sleep and thus cannot be called in atomic context.
 *
 * Return: 0 in case of success, a negative error core otherwise.
 */
int i3c_device_send_hdr_cmds(struct i3c_device *dev,
			     struct i3c_hdr_cmd *cmds,
			     int ncmds)
{
	struct i3c_master_controller *master;
	enum i3c_hdr_mode mode;
	int ret, i;

	if (ncmds < 1)
		return 0;

	mode = cmds[0].mode;
	for (i = 1; i < ncmds; i++) {
		if (mode != cmds[i].mode)
			return -EINVAL;
	}

	master = i3c_device_get_master(dev);
	if (!master)
		return -EINVAL;

	i3c_bus_normaluse_lock(master->bus);
	for (i = 0; i < ncmds; i++)
		cmds[i].addr = dev->info.dyn_addr;

	ret = i3c_master_send_hdr_cmds_locked(master, cmds, ncmds);
	i3c_bus_normaluse_unlock(master->bus);

	return ret;
}
EXPORT_SYMBOL_GPL(i3c_device_send_hdr_cmds);

void i3c_device_get_info(struct i3c_device *dev,
			 struct i3c_device_info *info)
{
	if (info)
		*info = dev->info;
}
EXPORT_SYMBOL_GPL(i3c_device_get_info);

/**
 * i3c_device_disable_ibi() - Disable IBIs coming from a specific device
 * @dev: device on which IBIs should be disabled
 *
 * This function disable IBIs coming from a specific device and wait for
 * all pending IBIs to be processed.
 *
 * Return: 0 in case of success, a negative error core otherwise.
 */
int i3c_device_disable_ibi(struct i3c_device *dev)
{
	struct i3c_master_controller *master = i3c_device_get_master(dev);
	int ret;

	mutex_lock(&dev->ibi_lock);
	if (!dev->ibi) {
		ret = -EINVAL;
		goto out;
	}

	ret = master->ops->disable_ibi(master, dev);
	if (ret)
		goto out;

	reinit_completion(&dev->ibi->all_ibis_handled);
	if (atomic_read(&dev->ibi->pending_ibis))
		wait_for_completion(&dev->ibi->all_ibis_handled);

	dev->ibi->enabled = false;

out:
	mutex_unlock(&dev->ibi_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(i3c_device_disable_ibi);

/**
 * i3c_device_enable_ibi() - Enable IBIs coming from a specific device
 * @dev: device on which IBIs should be enabled
 *
 * This function enable IBIs coming from a specific device and wait for
 * all pending IBIs to be processed. This should be called on a device
 * where i3c_device_request_ibi() has succeeded.
 *
 * Note that IBIs from this device might be received before this function
 * returns to its caller.
 *
 * Return: 0 in case of success, a negative error core otherwise.
 */
int i3c_device_enable_ibi(struct i3c_device *dev)
{
	struct i3c_master_controller *master = i3c_device_get_master(dev);
	int ret;

	mutex_lock(&dev->ibi_lock);
	if (!dev->ibi) {
		ret = -EINVAL;
		goto out;
	}

	ret = master->ops->enable_ibi(master, dev);
	if (!ret)
		dev->ibi->enabled = true;

out:
	mutex_unlock(&dev->ibi_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(i3c_device_enable_ibi);

/**
 * i3c_device_request_ibi() - Request an IBI
 * @dev: device for which we should enable IBIs
 * @req: setup requested for this IBI
 *
 * This function is responsible for pre-allocating all resources needed to
 * process IBIs coming from @dev. When this function returns, the IBI is not
 * enabled until i3c_device_enable_ibi() is called.
 *
 * Return: 0 in case of success, a negative error core otherwise.
 */
int i3c_device_request_ibi(struct i3c_device *dev,
			   const struct i3c_ibi_setup *req)
{
	struct i3c_master_controller *master = dev->common.master;
	struct i3c_device_ibi_info *ibi;
	int ret;

	if (!master->ops->request_ibi)
		return -ENOTSUPP;

	if (!req->handler || !req->num_slots)
		return -EINVAL;

	mutex_lock(&dev->ibi_lock);
	if (dev->ibi) {
		ret = -EBUSY;
		goto err_unlock_dev;
	}

	ibi = kzalloc(sizeof(*ibi), GFP_KERNEL);
	if (!ibi) {
		ret = -ENOMEM;
		goto err_unlock_dev;
	}

	atomic_set(&ibi->pending_ibis, 0);
	init_completion(&ibi->all_ibis_handled);
	ibi->handler = req->handler;
	ibi->max_payload_len = req->max_payload_len;

	dev->ibi = ibi;
	ret = master->ops->request_ibi(master, dev, req);
	if (ret)
		goto err_free_ibi;

	mutex_unlock(&dev->ibi_lock);

	return 0;

err_free_ibi:
	mutex_unlock(&dev->ibi_lock);
	kfree(ibi);
	dev->ibi = NULL;

err_unlock_dev:
	mutex_unlock(&dev->ibi_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(i3c_device_request_ibi);

/**
 * i3c_device_free_ibi() - Free all resources needed for IBI handling
 * @dev: device on which you want to release IBI resources
 *
 * This function is responsible for de-allocating resources previously
 * allocated by i3c_device_request_ibi(). It should be called after disabling
 * IBIs with i3c_device_disable_ibi().
 */
void i3c_device_free_ibi(struct i3c_device *dev)
{
	struct i3c_master_controller *master = dev->common.master;

	mutex_lock(&dev->ibi_lock);
	if (!dev->ibi)
		goto out;

	if (WARN_ON(dev->ibi->enabled))
		BUG_ON(i3c_device_disable_ibi(dev));

	master->ops->free_ibi(master, dev);
	kfree(dev->ibi);
	dev->ibi = NULL;

out:
	mutex_unlock(&dev->ibi_lock);
}
EXPORT_SYMBOL_GPL(i3c_device_free_ibi);

/**
 * i3c_device_match_id() - Find the I3C device ID entry matching an I3C dev
 * @i3cdev: the I3C device we're searching a match for
 * @id_table: the I3C device ID table
 *
 * Return: a pointer to the first entry matching @i3cdev, or NULL if there's
 *	   no match.
 */
const struct i3c_device_id *
i3c_device_match_id(struct i3c_device *i3cdev,
		    const struct i3c_device_id *id_table)
{
	const struct i3c_device_id *id;

	/*
	 * The lower 32bits of the provisional ID is just filled with a random
	 * value, try to match using DCR info.
	 */
	if (!I3C_PID_RND_LOWER_32BITS(i3cdev->info.pid)) {
		u16 manuf = I3C_PID_MANUF_ID(i3cdev->info.pid);
		u16 part = I3C_PID_PART_ID(i3cdev->info.pid);
		u16 ext_info = I3C_PID_EXTRA_INFO(i3cdev->info.pid);

		/* First try to match by manufacturer/part ID. */
		for (id = id_table; id->match_flags != 0; id++) {
			if ((id->match_flags & I3C_MATCH_MANUF_AND_PART) !=
			    I3C_MATCH_MANUF_AND_PART)
				continue;

			if (manuf != id->manuf_id || part != id->part_id)
				continue;

			if ((id->match_flags & I3C_MATCH_EXTRA_INFO) &&
			    ext_info != id->extra_info)
				continue;

			return id;
		}
	}

	/* Fallback to DCR match. */
	for (id = id_table; id->match_flags != 0; id++) {
		if ((id->match_flags & I3C_MATCH_DCR) &&
		    id->dcr == i3cdev->info.dcr)
			return id;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(i3c_device_match_id);

/**
 * i3c_driver_register_with_owner() - register an I3C device driver
 *
 * @drv: driver to register
 * @owner: module that owns this driver
 *
 * Register @drv to the core.
 *
 * Return: 0 in case of success, a negative error core otherwise.
 */
int i3c_driver_register_with_owner(struct i3c_driver *drv, struct module *owner)
{
	drv->driver.owner = owner;
	drv->driver.bus = &i3c_bus_type;

	return driver_register(&drv->driver);
}
EXPORT_SYMBOL_GPL(i3c_driver_register_with_owner);

/**
 * i3c_driver_unregister() - unregister an I3C device driver
 *
 * @drv: driver to unregister
 *
 * Unregister @drv.
 */
void i3c_driver_unregister(struct i3c_driver *drv)
{
	driver_unregister(&drv->driver);
}
EXPORT_SYMBOL_GPL(i3c_driver_unregister);
