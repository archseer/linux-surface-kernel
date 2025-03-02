#include <linux/acpi.h>
#include <linux/firmware.h>
#include <linux/ipts.h>
#include <linux/ipts-companion.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#define IPTS_SURFACE_FW_PATH_FMT "intel/ipts/%s/%s"

#define IPTS_SURFACE_FIRMWARE(X)					\
	MODULE_FIRMWARE("intel/ipts/" X "/config.bin");			\
	MODULE_FIRMWARE("intel/ipts/" X "/intel_desc.bin");		\
	MODULE_FIRMWARE("intel/ipts/" X "/vendor_desc.bin");		\
	MODULE_FIRMWARE("intel/ipts/" X "/vendor_kernel.bin");		\

int ipts_surface_request_firmware(ipts_companion_t *companion,
		const struct firmware **fw, const char *name,
		struct device *device)
{
	char fw_path[MAX_IOCL_FILE_PATH_LEN];

	if (companion == NULL || companion->data == NULL) {
		return -ENOENT;
	}

	snprintf(fw_path, MAX_IOCL_FILE_PATH_LEN, IPTS_SURFACE_FW_PATH_FMT,
			(const char *)companion->data, name);
	return request_firmware(fw, fw_path, device);
}

static ipts_bin_fw_info_t ipts_surface_vendor_kernel = {
	.fw_name = "vendor_kernel.bin",
	.vendor_output = -1,
	.num_of_data_files = 3,
	.data_file = {
		{
			.io_buffer_type = IPTS_CONFIGURATION,
			.flags = IPTS_DATA_FILE_FLAG_NONE,
			.file_name = "config.bin",
		},

		// The following files are part of the config, but they don't
		// exist, and the driver never requests them.
		{
			.io_buffer_type = IPTS_CALIBRATION,
			.flags = IPTS_DATA_FILE_FLAG_NONE,
			.file_name = "calib.bin",
		},
		{
			.io_buffer_type = IPTS_FEATURE,
			.flags = IPTS_DATA_FILE_FLAG_SHARE,
			.file_name = "feature.bin",
		},
	},
};

static ipts_bin_fw_info_t *ipts_surface_fw_config[] = {
	&ipts_surface_vendor_kernel,
	NULL,
};

static ipts_companion_t ipts_surface_companion = {
	.firmware_request = &ipts_surface_request_firmware,
	.firmware_config = ipts_surface_fw_config,
	.name = "ipts_surface",
};

static int ipts_surface_probe(struct platform_device *pdev)
{
	int ret;
	struct acpi_device *adev = ACPI_COMPANION(&pdev->dev);

	if (!adev) {
		dev_err(&pdev->dev, "Unable to find ACPI info for device\n");
		return -ENODEV;
	}

	ipts_surface_companion.data = (void *)acpi_device_hid(adev);
	ret = ipts_add_companion(&ipts_surface_companion);
	if (ret) {
		dev_warn(&pdev->dev, "Adding IPTS companion failed, "
				"error: %d\n", ret);
		return ret;
	}

	return 0;
}

static int ipts_surface_remove(struct platform_device *pdev)
{
	int ret = ipts_remove_companion(&ipts_surface_companion);
	if (ret) {
		dev_warn(&pdev->dev, "Removing IPTS companion failed, "
				"error: %d\n", ret);
	}

	return 0;
}

static const struct acpi_device_id ipts_surface_acpi_match[] = {
	{ "MSHW0076", 0 },	// Surface Book 1 / Surface Studio
	{ "MSHW0078", 0 },	// Surface Pro 4
	{ "MSHW0079", 0 },	// Surface Laptop 1 / 2
	{ "MSHW0101", 0 },	// Surface Book 2 15"
	{ "MSHW0102", 0 },	// Surface Pro 2017 / 6
	{ "MSHW0103", 0 },	// unknown, but firmware exists
	{ "MSHW0137", 0 },	// Surface Book 2
	{ },
};
MODULE_DEVICE_TABLE(acpi, ipts_surface_acpi_match);

static struct platform_driver ipts_surface_driver = {
	.probe = ipts_surface_probe,
	.remove = ipts_surface_remove,
	.driver = {
		.name = "ipts_surface",
		.acpi_match_table = ACPI_PTR(ipts_surface_acpi_match),
	},
};
module_platform_driver(ipts_surface_driver);

MODULE_AUTHOR("Dorian Stoll <dorian.stoll@tmsp.io>");
MODULE_DESCRIPTION("IPTS companion driver for Microsoft Surface");
MODULE_LICENSE("GPL v2");

IPTS_SURFACE_FIRMWARE("MSHW0076");
IPTS_SURFACE_FIRMWARE("MSHW0078");
IPTS_SURFACE_FIRMWARE("MSHW0079");
IPTS_SURFACE_FIRMWARE("MSHW0101");
IPTS_SURFACE_FIRMWARE("MSHW0102");
IPTS_SURFACE_FIRMWARE("MSHW0103");
IPTS_SURFACE_FIRMWARE("MSHW0137");
