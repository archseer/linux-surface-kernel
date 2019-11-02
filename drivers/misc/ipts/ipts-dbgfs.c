/*
 * Intel Precise Touch & Stylus device driver
 * Copyright (c) 2016, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */
#include <linux/debugfs.h>
#include <linux/ctype.h>
#include <linux/uaccess.h>

#include "ipts.h"
#include "ipts-sensor-regs.h"
#include "ipts-msg-handler.h"
#include "ipts-state.h"
#include "../mei/mei_dev.h"

const char sensor_mode_fmt[] = "sensor mode : %01d\n";
const char ipts_status_fmt[] = "sensor mode : %01d\nipts state : %01d\n";
const char ipts_debug_fmt[] = ">> tdt : fw status : %s\n"
							  ">> == DB s:%x, c:%x ==\n"
							  ">> == WQ h:%u, t:%u ==\n";

static ssize_t ipts_dbgfs_mode_read(struct file *fp, char __user *ubuf,
						size_t cnt, loff_t *ppos)
{
	ipts_info_t *ipts = fp->private_data;
	char mode[80];
	int len = 0;

	if (cnt < sizeof(sensor_mode_fmt) - 3)
		return -EINVAL;

	len = scnprintf(mode, 80, sensor_mode_fmt, ipts->sensor_mode);
	if (len < 0)
		return -EIO;

	return simple_read_from_buffer(ubuf, cnt, ppos, mode, len);
}

static ssize_t ipts_dbgfs_mode_write(struct file *fp, const char __user *ubuf,
						size_t cnt, loff_t *ppos)
{
	ipts_info_t *ipts = fp->private_data;
	ipts_state_t state;
	int sensor_mode, len;
	char mode[3];

	if (cnt == 0 || cnt > 3)
		return -EINVAL;

	state = ipts_get_state(ipts);
	if (state != IPTS_STA_RAW_DATA_STARTED && state != IPTS_STA_HID_STARTED) {
		return -EIO;
	}

	len = cnt;
	if (copy_from_user(mode, ubuf, len))
		return -EFAULT;

	while(len > 0 && (isspace(mode[len-1]) || mode[len-1] == '\n'))
		len--;
	mode[len] = '\0';

	if (sscanf(mode, "%d", &sensor_mode) != 1)
		return -EINVAL;

	if (sensor_mode != TOUCH_SENSOR_MODE_RAW_DATA &&
					sensor_mode != TOUCH_SENSOR_MODE_HID) {
		return -EINVAL;
	}

	if (sensor_mode == ipts->sensor_mode)
		return 0;

	ipts_switch_sensor_mode(ipts, sensor_mode);

	return cnt;
}

static const struct file_operations ipts_mode_dbgfs_fops = {
        .open = simple_open,
        .read = ipts_dbgfs_mode_read,
        .write = ipts_dbgfs_mode_write,
        .llseek = generic_file_llseek,
};

static ssize_t ipts_dbgfs_status_read(struct file *fp, char __user *ubuf,
						size_t cnt, loff_t *ppos)
{
	ipts_info_t *ipts = fp->private_data;
	char status[256];
	int len = 0;

	if (cnt < sizeof(ipts_status_fmt) - 3)
		return -EINVAL;

	len = scnprintf(status, 256, ipts_status_fmt, ipts->sensor_mode,
						     ipts->state);
	if (len < 0)
		return -EIO;

	return simple_read_from_buffer(ubuf, cnt, ppos, status, len);
}

static const struct file_operations ipts_status_dbgfs_fops = {
        .open = simple_open,
        .read = ipts_dbgfs_status_read,
        .llseek = generic_file_llseek,
};

static ssize_t ipts_dbgfs_quiesce_io_cmd_write(struct file *fp, const char __user *ubuf,
						size_t cnt, loff_t *ppos)
{
	ipts_info_t *ipts = fp->private_data;
	bool result;
	int rc;

	rc = kstrtobool_from_user(ubuf, cnt, &result);
	if (rc)
		return rc;

	if (!result)
		return -EINVAL;

	ipts_send_sensor_quiesce_io_cmd(ipts);

	return cnt;
}

static const struct file_operations ipts_quiesce_io_cmd_dbgfs_fops = {
        .open = simple_open,
        .write = ipts_dbgfs_quiesce_io_cmd_write,
        .llseek = generic_file_llseek,
};

static ssize_t ipts_dbgfs_clear_mem_window_cmd_write(struct file *fp, const char __user *ubuf,
						size_t cnt, loff_t *ppos)
{
	ipts_info_t *ipts = fp->private_data;
	bool result;
	int rc;

	rc = kstrtobool_from_user(ubuf, cnt, &result);
	if (rc)
		return rc;

	if (!result)
		return -EINVAL;

	ipts_send_sensor_clear_mem_window_cmd(ipts);

	return cnt;
}

static const struct file_operations ipts_clear_mem_window_cmd_dbgfs_fops = {
        .open = simple_open,
        .write = ipts_dbgfs_clear_mem_window_cmd_write,
        .llseek = generic_file_llseek,
};

static ssize_t ipts_dbgfs_debug_read(struct file *fp, char __user *ubuf,
						size_t cnt, loff_t *ppos)
{
	ipts_info_t *ipts = fp->private_data;
	char dbg_info[1024];
	int len = 0;

	char fw_sts_str[MEI_FW_STATUS_STR_SZ];
	u32 *db, *head, *tail;
	intel_ipts_wq_info_t* wq_info;

	wq_info = &ipts->resource.wq_info;

	mei_fw_status_str(ipts->cldev->bus, fw_sts_str, MEI_FW_STATUS_STR_SZ);
	// pr_info(">> tdt : fw status : %s\n", fw_sts_str);

	db = (u32*)wq_info->db_addr;
	head = (u32*)wq_info->wq_head_addr;
	tail = (u32*)wq_info->wq_tail_addr;
	// pr_info(">> == DB s:%x, c:%x ==\n", *db, *(db+1));
	// pr_info(">> == WQ h:%u, t:%u ==\n", *head, *tail);

	if (cnt < sizeof(ipts_debug_fmt) - 3)
		return -EINVAL;

	len = scnprintf(dbg_info, 1024, ipts_debug_fmt,
							fw_sts_str,
							*db, *(db+1),
							*head, *tail);
	if (len < 0)
		return -EIO;

	return simple_read_from_buffer(ubuf, cnt, ppos, dbg_info, len);
}

static const struct file_operations ipts_debug_dbgfs_fops = {
        .open = simple_open,
        .read = ipts_dbgfs_debug_read,
        .llseek = generic_file_llseek,
};

static ssize_t ipts_dbgfs_ipts_restart_write(struct file *fp, const char __user *ubuf,
						size_t cnt, loff_t *ppos)
{
	ipts_info_t *ipts = fp->private_data;
	bool result;
	int rc;

	rc = kstrtobool_from_user(ubuf, cnt, &result);
	if (rc)
		return rc;

	if (!result)
		return -EINVAL;

	ipts_restart(ipts);

	return cnt;
}

static const struct file_operations ipts_ipts_restart_dbgfs_fops = {
        .open = simple_open,
        .write = ipts_dbgfs_ipts_restart_write,
        .llseek = generic_file_llseek,
};

static ssize_t ipts_dbgfs_ipts_stop_write(struct file *fp, const char __user *ubuf,
						size_t cnt, loff_t *ppos)
{
	ipts_info_t *ipts = fp->private_data;
	bool result;
	int rc;

	rc = kstrtobool_from_user(ubuf, cnt, &result);
	if (rc)
		return rc;

	if (!result)
		return -EINVAL;

	ipts_stop(ipts);

	return cnt;
}

static const struct file_operations ipts_ipts_stop_dbgfs_fops = {
        .open = simple_open,
        .write = ipts_dbgfs_ipts_stop_write,
        .llseek = generic_file_llseek,
};

static ssize_t ipts_dbgfs_ipts_start_write(struct file *fp, const char __user *ubuf,
						size_t cnt, loff_t *ppos)
{
	ipts_info_t *ipts = fp->private_data;
	bool result;
	int rc;

	rc = kstrtobool_from_user(ubuf, cnt, &result);
	if (rc)
		return rc;

	if (!result)
		return -EINVAL;

	ipts_start(ipts);

	return cnt;
}

static const struct file_operations ipts_ipts_start_dbgfs_fops = {
        .open = simple_open,
        .write = ipts_dbgfs_ipts_start_write,
        .llseek = generic_file_llseek,
};

void ipts_dbgfs_deregister(ipts_info_t* ipts)
{
	if (!ipts->dbgfs_dir)
		return;

	debugfs_remove_recursive(ipts->dbgfs_dir);
	ipts->dbgfs_dir = NULL;
}

int ipts_dbgfs_register(ipts_info_t* ipts, const char *name)
{
	struct dentry *dir, *f;

	dir = debugfs_create_dir(name, NULL);
	if (!dir)
		return -ENOMEM;

        f = debugfs_create_file("mode", S_IRUSR | S_IWUSR, dir,
                                ipts, &ipts_mode_dbgfs_fops);
        if (!f) {
                ipts_err(ipts, "debugfs mode creation failed\n");
                goto err;
        }

        f = debugfs_create_file("status", S_IRUSR, dir,
                                ipts, &ipts_status_dbgfs_fops);
        if (!f) {
                ipts_err(ipts, "debugfs status creation failed\n");
                goto err;
        }

        f = debugfs_create_file("quiesce_io_cmd", S_IWUSR, dir,
                                ipts, &ipts_quiesce_io_cmd_dbgfs_fops);
        if (!f) {
                ipts_err(ipts, "debugfs quiesce_io_cmd creation failed\n");
                goto err;
        }

        f = debugfs_create_file("clear_mem_window_cmd", S_IWUSR, dir,
                                ipts, &ipts_clear_mem_window_cmd_dbgfs_fops);
        if (!f) {
                ipts_err(ipts, "debugfs clear_mem_window_cmd creation failed\n");
                goto err;
        }

        f = debugfs_create_file("debug", S_IRUSR, dir,
                                ipts, &ipts_debug_dbgfs_fops);
        if (!f) {
                ipts_err(ipts, "debugfs debug creation failed\n");
                goto err;
        }

        f = debugfs_create_file("ipts_restart", S_IWUSR, dir,
                                ipts, &ipts_ipts_restart_dbgfs_fops);
        if (!f) {
                ipts_err(ipts, "debugfs ipts_restart creation failed\n");
                goto err;
        }

        f = debugfs_create_file("ipts_stop", S_IWUSR, dir,
                                ipts, &ipts_ipts_stop_dbgfs_fops);
        if (!f) {
                ipts_err(ipts, "debugfs ipts_stop creation failed\n");
                goto err;
        }

        f = debugfs_create_file("ipts_start", S_IWUSR, dir,
                                ipts, &ipts_ipts_start_dbgfs_fops);
        if (!f) {
                ipts_err(ipts, "debugfs ipts_start creation failed\n");
                goto err;
        }

	ipts->dbgfs_dir = dir;

	return 0;
err:
	ipts_dbgfs_deregister(ipts);
	return -ENODEV;
}
