#include <linux/firmware.h>
#include <linux/ipts.h>
#include <linux/ipts-binary.h>
#include <linux/ipts-companion.h>
#include <linux/mutex.h>

#include "ipts.h"
#include "ipts-companion.h"
#include "ipts-params.h"

#define IPTS_FW_PATH_FMT "intel/ipts/%s"
#define IPTS_FW_CONFIG_FILE "ipts_fw_config.bin"

ipts_companion_t *ipts_companion;
DEFINE_MUTEX(ipts_companion_lock);

bool ipts_companion_available(void)
{
	bool ret;
	mutex_lock(&ipts_companion_lock);

	ret = ipts_companion != NULL;

	mutex_unlock(&ipts_companion_lock);
	return ret;
}

/*
 * General purpose API for adding or removing a companion driver
 * A companion driver is a driver that implements hardware specific
 * behaviour into IPTS, so it doesn't have to be hardcoded into the
 * main driver. All requests to the companion driver should be wrapped,
 * with a fallback in case a companion driver cannot be found.
 */

int ipts_add_companion(ipts_companion_t *companion)
{
	int ret;
	mutex_lock(&ipts_companion_lock);

	if (ipts_companion == NULL) {
		ret = 0;
		ipts_companion = companion;
	} else {
		ret = -EBUSY;
	}

	mutex_unlock(&ipts_companion_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(ipts_add_companion);

int ipts_remove_companion(ipts_companion_t *companion)
{
	int ret;
	mutex_lock(&ipts_companion_lock);

	if (ipts_companion != NULL && companion != NULL &&
			ipts_companion->name != companion->name) {
		ret = -EPERM;
	} else {
		ret = 0;
		ipts_companion = NULL;
	}

	mutex_unlock(&ipts_companion_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(ipts_remove_companion);

/*
 * Utility functions for IPTS. These functions replace codepaths in the IPTS
 * driver, and redirect them to the companion driver, if one was found.
 * Otherwise the legacy code gets executed as a fallback.
 */

int ipts_request_firmware(const struct firmware **fw, const char *name,
	struct device *device)
{
	int ret = 0;
	char fw_path[MAX_IOCL_FILE_PATH_LEN];
	mutex_lock(&ipts_companion_lock);

	// Check if a companion was registered. If not, skip
	// forward and try to load the firmware from the legacy path
	if (ipts_companion == NULL || ipts_modparams.ignore_companion) {
		goto request_firmware_fallback;
	}

	ret = ipts_companion->firmware_request(ipts_companion, fw, name, device);
	if (!ret) {
		goto request_firmware_return;
	}

request_firmware_fallback:

	// If fallback loading for firmware was disabled, abort.
	// Return -ENOENT as no firmware file was found.
	if (ipts_modparams.ignore_fw_fallback) {
		ret = -ENOENT;
		goto request_firmware_return;
	}

	// No firmware was found by the companion driver, try the generic path.
	snprintf(fw_path, MAX_IOCL_FILE_PATH_LEN, IPTS_FW_PATH_FMT, name);
	ret = request_firmware(fw, fw_path, device);

request_firmware_return:

	mutex_unlock(&ipts_companion_lock);
	return ret;
}

static ipts_bin_fw_list_t *ipts_alloc_fw_list(ipts_bin_fw_info_t **fw)
{
	int size, len, i, j;
	ipts_bin_fw_list_t *fw_list;
	char *itr;

	// Figure out the amount of firmware files inside of the array
	len = 0;
	while (fw[len] != NULL) {
		len++;
	}

	// Determine the size that the final list will need in memory
	size = sizeof(ipts_bin_fw_list_t);
	for (i = 0; i < len; i++) {
		size += sizeof(ipts_bin_fw_info_t);
		size += sizeof(ipts_bin_data_file_info_t) *
				fw[i]->num_of_data_files;
	}

	fw_list = kmalloc(size, GFP_KERNEL);
	fw_list->num_of_fws = len;
	itr = (char *)fw_list->fw_info;
	for (i = 0; i < len; i++) {
		*(ipts_bin_fw_info_t *)itr = *fw[i];
		itr += sizeof(ipts_bin_fw_info_t);

		for (j = 0; j < fw[i]->num_of_data_files; j++) {
			*(ipts_bin_data_file_info_t *)itr = fw[i]->data_file[j];
			itr += sizeof(ipts_bin_data_file_info_t);
		}
	}

	return fw_list;
}

int ipts_request_firmware_config(ipts_info_t *ipts, ipts_bin_fw_list_t **cfg)
{
	int ret;
	const struct firmware *config_fw = NULL;
	mutex_lock(&ipts_companion_lock);

	// Check if a companion was registered. If not, skip
	// forward and try to load the firmware config from a file
	if (ipts_modparams.ignore_companion || ipts_companion == NULL) {
		mutex_unlock(&ipts_companion_lock);
		goto config_fallback;
	}

	if (ipts_companion->firmware_config != NULL) {
		*cfg = ipts_alloc_fw_list(ipts_companion->firmware_config);
		mutex_unlock(&ipts_companion_lock);
		return 0;
	}

config_fallback:

	// If fallback loading for the firmware config was disabled, abort.
	// Return -ENOENT as no config file was found.
	if (ipts_modparams.ignore_config_fallback) {
		return -ENOENT;
	}

	// No firmware config was found by the companion driver,
	// try loading it from a file now
	ret = ipts_request_firmware(&config_fw, IPTS_FW_CONFIG_FILE,
			&ipts->cldev->dev);

	if (!ret) {
		*cfg = (ipts_bin_fw_list_t *)config_fw->data;
	} else {
		release_firmware(config_fw);
	}

	return ret;

}
