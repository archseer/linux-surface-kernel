/*
 * Surface ACPI Notify (SAN) and ACPI integration driver for SAM.
 * Translates communication from ACPI to SSH and back.
 */

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>

#include "surface_sam_ssh.h"


#define SAN_RQST_RETRY				5

#define SAN_DSM_REVISION			0
#define SAN_DSM_FN_NOTIFY_SENSOR_TRIP_POINT	0x09

static const guid_t SAN_DSM_UUID =
	GUID_INIT(0x93b666c5, 0x70c6, 0x469f, 0xa2, 0x15, 0x3d,
	          0x48, 0x7c, 0x91, 0xab, 0x3c);

#define SAM_EVENT_DELAY_PWR_STATE	msecs_to_jiffies(5000)

#define SAM_EVENT_PWR_TC		0x02
#define SAM_EVENT_PWR_RQID		0x0002
#define SAM_EVENT_PWR_CID_HWCHANGE	0x15
#define SAM_EVENT_PWR_CID_CHARGING	0x16
#define SAM_EVENT_PWR_CID_ADAPTER	0x17
#define SAM_EVENT_PWR_CID_STATE		0x4f

#define SAM_EVENT_TEMP_TC		0x03
#define SAM_EVENT_TEMP_RQID		0x0003
#define SAM_EVENT_TEMP_CID_NOTIFY_SENSOR_TRIP_POINT	0x0b

#define SAN_RQST_TAG			"surface_sam_san_rqst: "

#define SAN_QUIRK_BASE_STATE_DELAY	1000


struct san_acpi_consumer {
	char *path;
	bool  required;
	u32   flags;
};

struct san_opreg_context {
	struct acpi_connection_info connection;
	struct device *dev;
};

struct san_consumer_link {
	const struct san_acpi_consumer *properties;
	struct device_link                         *link;
};

struct san_consumers {
	u32                                   num;
	struct san_consumer_link *links;
};

struct san_drvdata {
	struct san_opreg_context opreg_ctx;
	struct san_consumers     consumers;
};

struct gsb_data_in {
	u8 cv;
} __packed;

struct gsb_data_rqsx {
	u8 cv;				// command value (should be 0x01 or 0x03)
	u8 tc;				// target controller
	u8 tid;				// expected to be 0x01, could be revision
	u8 iid;				// target sub-controller (e.g. primary vs. secondary battery)
	u8 snc;				// expect-response-flag
	u8 cid;				// command ID
	u8 cdl;				// payload length
	u8 _pad;			// padding
	u8 pld[0];			// payload
} __packed;

struct gsb_data_etwl {
	u8 cv;				// command value (should be 0x02)
	u8 etw3;			// ?
	u8 etw4;			// ?
	u8 msg[0];			// error message (ASCIIZ)
} __packed;

struct gsb_data_out {
	u8 status;			// _SSH communication status
	u8 len;				// _SSH payload length
	u8 pld[0];			// _SSH payload
} __packed;

union gsb_buffer_data {
	struct gsb_data_in   in;	// common input
	struct gsb_data_rqsx rqsx;	// RQSX input
	struct gsb_data_etwl etwl;	// ETWL input
	struct gsb_data_out  out;	// output
};

struct gsb_buffer {
	u8 status;			// GSB AttribRawProcess status
	u8 len;				// GSB AttribRawProcess length
	union gsb_buffer_data data;
} __packed;


enum san_pwr_event {
	SAN_PWR_EVENT_BAT1_STAT	= 0x03,
	SAN_PWR_EVENT_BAT1_INFO	= 0x04,
	SAN_PWR_EVENT_ADP1_STAT	= 0x05,
	SAN_PWR_EVENT_ADP1_INFO	= 0x06,
	SAN_PWR_EVENT_BAT2_STAT	= 0x07,
	SAN_PWR_EVENT_BAT2_INFO	= 0x08,
};


static int san_acpi_notify_power_event(struct device *dev, enum san_pwr_event event)
{
	acpi_handle san = ACPI_HANDLE(dev);
	union acpi_object *obj;

	obj = acpi_evaluate_dsm_typed(san, &SAN_DSM_UUID, SAN_DSM_REVISION,
	                              (u8) event, NULL, ACPI_TYPE_BUFFER);

	if (IS_ERR_OR_NULL(obj)) {
		return obj ? PTR_ERR(obj) : -ENXIO;
	}

	if (obj->buffer.length != 1 || obj->buffer.pointer[0] != 0) {
		dev_err(dev, "got unexpected result from _DSM\n");
		return -EFAULT;
	}

	ACPI_FREE(obj);
	return 0;
}

static int san_acpi_notify_sensor_trip_point(struct device *dev, u8 iid)
{
	acpi_handle san = ACPI_HANDLE(dev);
	union acpi_object *obj;
	union acpi_object param;

	param.type = ACPI_TYPE_INTEGER;
	param.integer.value = iid;

	obj = acpi_evaluate_dsm_typed(san, &SAN_DSM_UUID, SAN_DSM_REVISION,
	                              SAN_DSM_FN_NOTIFY_SENSOR_TRIP_POINT,
				      &param, ACPI_TYPE_BUFFER);

	if (IS_ERR_OR_NULL(obj)) {
		return obj ? PTR_ERR(obj) : -ENXIO;
	}

	if (obj->buffer.length != 1 || obj->buffer.pointer[0] != 0) {
		dev_err(dev, "got unexpected result from _DSM\n");
		return -EFAULT;
	}

	ACPI_FREE(obj);
	return 0;
}


inline static int san_evt_power_adapter(struct device *dev, struct surface_sam_ssh_event *event)
{
	int status;

	status = san_acpi_notify_power_event(dev, SAN_PWR_EVENT_ADP1_STAT);
	if (status) {
		dev_err(dev, "error handling power event (cid = %x)\n", event->cid);
		return status;
	}

	return 0;
}

inline static int san_evt_power_hwchange(struct device *dev, struct surface_sam_ssh_event *event)
{
	enum san_pwr_event evcode;
	int status;

	if (event->iid == 0x02) {
		evcode = SAN_PWR_EVENT_BAT2_INFO;
	} else {
		evcode = SAN_PWR_EVENT_BAT1_INFO;
	}

	status = san_acpi_notify_power_event(dev, evcode);
	if (status) {
		dev_err(dev, "error handling power event (cid = %x)\n", event->cid);
		return status;
	}

	return 0;
}

inline static int san_evt_power_state(struct device *dev, struct surface_sam_ssh_event *event)
{
	int status;

	status = san_acpi_notify_power_event(dev, SAN_PWR_EVENT_BAT1_STAT);
	if (status) {
		dev_err(dev, "error handling power event (cid = %x)\n", event->cid);
		return status;
	}

	status = san_acpi_notify_power_event(dev, SAN_PWR_EVENT_BAT2_STAT);
	if (status) {
		dev_err(dev, "error handling power event (cid = %x)\n", event->cid);
		return status;
	}

	return 0;
}

static unsigned long san_evt_power_delay(struct surface_sam_ssh_event *event, void *data)
{
	switch (event->cid) {
	case SAM_EVENT_PWR_CID_CHARGING:
	case SAM_EVENT_PWR_CID_STATE:
		return SAM_EVENT_DELAY_PWR_STATE;

	case SAM_EVENT_PWR_CID_ADAPTER:
	case SAM_EVENT_PWR_CID_HWCHANGE:
	default:
		return 0;
	}
}

static int san_evt_power(struct surface_sam_ssh_event *event, void *data)
{
	struct device *dev = (struct device *)data;

	switch (event->cid) {
	case SAM_EVENT_PWR_CID_HWCHANGE:
		return san_evt_power_hwchange(dev, event);

	case SAM_EVENT_PWR_CID_ADAPTER:
		return san_evt_power_adapter(dev, event);

	case SAM_EVENT_PWR_CID_CHARGING:
	case SAM_EVENT_PWR_CID_STATE:
		return san_evt_power_state(dev, event);

	default:
		dev_warn(dev, "unhandled power event (cid = %x)\n", event->cid);
	}

	return 0;
}


inline static int san_evt_thermal_notify(struct device *dev, struct surface_sam_ssh_event *event)
{
	int status;

	status = san_acpi_notify_sensor_trip_point(dev, event->iid);
	if (status) {
		dev_err(dev, "error handling thermal event (cid = %x)\n", event->cid);
		return status;
	}

	return 0;
}

static int san_evt_thermal(struct surface_sam_ssh_event *event, void *data)
{
	struct device *dev = (struct device *)data;

	switch (event->cid) {
	case SAM_EVENT_TEMP_CID_NOTIFY_SENSOR_TRIP_POINT:
		return san_evt_thermal_notify(dev, event);

	default:
		dev_warn(dev, "unhandled thermal event (cid = %x)\n", event->cid);
	}

	return 0;
}


static struct gsb_data_rqsx
*san_validate_rqsx(struct device *dev, const char *type, struct gsb_buffer *buffer)
{
	struct gsb_data_rqsx *rqsx = &buffer->data.rqsx;

	if (buffer->len < 8) {
		dev_err(dev, "invalid %s package (len = %d)\n",
			type, buffer->len);
		return NULL;
	}

	if (rqsx->cdl != buffer->len - 8) {
		dev_err(dev, "bogus %s package (len = %d, cdl = %d)\n",
			type, buffer->len, rqsx->cdl);
		return NULL;
	}

	if (rqsx->tid != 0x01) {
		dev_warn(dev, "unsupported %s package (tid = 0x%02x)\n",
			 type, rqsx->tid);
		return NULL;
	}

	return rqsx;
}

static acpi_status
san_etwl(struct san_opreg_context *ctx, struct gsb_buffer *buffer)
{
	struct gsb_data_etwl *etwl = &buffer->data.etwl;

	if (buffer->len < 3) {
		dev_err(ctx->dev, "invalid ETWL package (len = %d)\n", buffer->len);
		return AE_OK;
	}

	dev_err(ctx->dev, "ETWL(0x%02x, 0x%02x): %.*s\n",
		etwl->etw3, etwl->etw4,
		buffer->len - 3, (char *)etwl->msg);

	// indicate success
	buffer->status = 0x00;
	buffer->len = 0x00;

	return AE_OK;
}

static acpi_status
san_rqst(struct san_opreg_context *ctx, struct gsb_buffer *buffer)
{
	struct gsb_data_rqsx *gsb_rqst = san_validate_rqsx(ctx->dev, "RQST", buffer);
	struct surface_sam_ssh_rqst rqst = {};
	struct surface_sam_ssh_buf result = {};
	int status = 0;
	int try;

	if (!gsb_rqst) {
		return AE_OK;
	}

	rqst.tc  = gsb_rqst->tc;
	rqst.iid = gsb_rqst->iid;
	rqst.cid = gsb_rqst->cid;
	rqst.snc = gsb_rqst->snc;
	rqst.cdl = gsb_rqst->cdl;
	rqst.pld = &gsb_rqst->pld[0];

	result.cap  = SURFACE_SAM_SSH_MAX_RQST_RESPONSE;
	result.len  = 0;
	result.data = kzalloc(result.cap, GFP_KERNEL);

	if (!result.data) {
		return AE_NO_MEMORY;
	}

	for (try = 0; try < SAN_RQST_RETRY; try++) {
		if (try) {
			dev_warn(ctx->dev, SAN_RQST_TAG "IO error occured, trying again\n");
		}

		status = surface_sam_ssh_rqst(&rqst, &result);
		if (status != -EIO) break;
	}

	if (rqst.tc == 0x11 && rqst.cid == 0x0D && status == -EPERM) {
		/* Base state quirk:
		 * The base state may be queried from ACPI when the EC is still
		 * suspended. In this case it will return '-EPERM'. This query
		 * will only be triggered from the ACPI lid GPE interrupt, thus
		 * we are either in laptop or studio mode (base status 0x01 or
		 * 0x02). Furthermore, we will only get here if the device (and
		 * EC) have been suspended.
		 *
		 * We now assume that the device is in laptop mode (0x01). This
		 * has the drawback that it will wake the device when unfolding
		 * it in studio mode, but it also allows us to avoid actively
		 * waiting for the EC to wake up, which may incur a notable
		 * delay.
		 */

		buffer->status          = 0x00;
		buffer->len             = 0x03;
		buffer->data.out.status = 0x00;
		buffer->data.out.len    = 0x01;
		buffer->data.out.pld[0] = 0x01;

	} else if (!status) {		// success
		buffer->status          = 0x00;
		buffer->len             = result.len + 2;
		buffer->data.out.status = 0x00;
		buffer->data.out.len    = result.len;
		memcpy(&buffer->data.out.pld[0], result.data, result.len);

	} else {			// failure
		dev_err(ctx->dev, SAN_RQST_TAG "failed with error %d\n", status);
		buffer->status          = 0x00;
		buffer->len             = 0x02;
		buffer->data.out.status = 0x01;		// indicate _SSH error
		buffer->data.out.len    = 0x00;
	}

	kfree(result.data);

	return AE_OK;
}

static acpi_status
san_rqsg(struct san_opreg_context *ctx, struct gsb_buffer *buffer)
{
	struct gsb_data_rqsx *rqsg = san_validate_rqsx(ctx->dev, "RQSG", buffer);

	if (!rqsg) {
		return AE_OK;
	}

	// TODO: RQSG handler

	dev_warn(ctx->dev, "unsupported request: RQSG(0x%02x, 0x%02x, 0x%02x)\n",
		 rqsg->tc, rqsg->cid, rqsg->iid);

	return AE_OK;
}


static acpi_status
san_opreg_handler(u32 function, acpi_physical_address command,
		  u32 bits, u64 *value64,
		  void *opreg_context, void *region_context)
{
	struct san_opreg_context *context = opreg_context;
	struct gsb_buffer *buffer = (struct gsb_buffer *)value64;
	int accessor_type = (0xFFFF0000 & function) >> 16;

	if (command != 0) {
		dev_warn(context->dev, "unsupported command: 0x%02llx\n", command);
		return AE_OK;
	}

	if (accessor_type != ACPI_GSB_ACCESS_ATTRIB_RAW_PROCESS) {
		dev_err(context->dev, "invalid access type: 0x%02x\n", accessor_type);
		return AE_OK;
	}

	// buffer must have at least contain the command-value
	if (buffer->len == 0) {
		dev_err(context->dev, "request-package too small\n");
		return AE_OK;
	}

	switch (buffer->data.in.cv) {
	case 0x01:  return san_rqst(context, buffer);
	case 0x02:  return san_etwl(context, buffer);
	case 0x03:  return san_rqsg(context, buffer);
	}

	dev_warn(context->dev, "unsupported SAN0 request (cv: 0x%02x)\n", buffer->data.in.cv);
	return AE_OK;
}

static int san_enable_events(struct device *dev)
{
	int status;

	status = surface_sam_ssh_set_delayed_event_handler(
			SAM_EVENT_PWR_RQID, san_evt_power,
			san_evt_power_delay, dev);
	if (status) {
		goto err_handler_power;
	}

	status = surface_sam_ssh_set_event_handler(
			SAM_EVENT_TEMP_RQID, san_evt_thermal,
			dev);
	if (status) {
		goto err_handler_thermal;
	}

	status = surface_sam_ssh_enable_event_source(SAM_EVENT_PWR_TC, 0x01, SAM_EVENT_PWR_RQID);
	if (status) {
		goto err_source_power;
	}

	status = surface_sam_ssh_enable_event_source(SAM_EVENT_TEMP_TC, 0x01, SAM_EVENT_TEMP_RQID);
	if (status) {
		goto err_source_thermal;
	}

	return 0;

err_source_thermal:
	surface_sam_ssh_disable_event_source(SAM_EVENT_PWR_TC, 0x01, SAM_EVENT_PWR_RQID);
err_source_power:
	surface_sam_ssh_remove_event_handler(SAM_EVENT_TEMP_RQID);
err_handler_thermal:
	surface_sam_ssh_remove_event_handler(SAM_EVENT_PWR_RQID);
err_handler_power:
	return status;
}

static void san_disable_events(void)
{
	surface_sam_ssh_disable_event_source(SAM_EVENT_TEMP_TC, 0x01, SAM_EVENT_TEMP_RQID);
	surface_sam_ssh_disable_event_source(SAM_EVENT_PWR_TC, 0x01, SAM_EVENT_PWR_RQID);
	surface_sam_ssh_remove_event_handler(SAM_EVENT_TEMP_RQID);
	surface_sam_ssh_remove_event_handler(SAM_EVENT_PWR_RQID);
}


static int san_consumers_link(struct platform_device *pdev,
			      const struct san_acpi_consumer *cons,
			      struct san_consumers *out)
{
	const struct san_acpi_consumer *con;
	struct san_consumer_link *links, *link;
	struct acpi_device *adev;
	acpi_handle handle;
	u32 max_links = 0;
	int status;

	if (!cons) {
		return 0;
	}

	// count links
	for (con = cons; con->path; ++con) {
		max_links += 1;
	}

	// allocate
	links = kzalloc(max_links * sizeof(struct san_consumer_link), GFP_KERNEL);
	link = &links[0];

	if (!links) {
		return -ENOMEM;
	}

	// create links
	for (con = cons; con->path; ++con) {
		status = acpi_get_handle(NULL, con->path, &handle);
		if (status) {
			if (con->required || status != AE_NOT_FOUND) {
				status = -ENXIO;
				goto cleanup;
			} else {
				continue;
			}
		}

		status = acpi_bus_get_device(handle, &adev);
		if (status) {
			goto cleanup;
		}

		link->link = device_link_add(&adev->dev, &pdev->dev, con->flags);
		if (!(link->link)) {
			status = -EFAULT;
			goto cleanup;
		}
		link->properties = con;

		link += 1;
	}

	out->num = link - links;
	out->links = links;

	return 0;

cleanup:
	for (link = link - 1; link >= links; --link) {
		if (link->properties->flags & DL_FLAG_STATELESS) {
			device_link_del(link->link);
		}
	}

	return status;
}

static void san_consumers_unlink(struct san_consumers *consumers) {
	u32 i;

	if (!consumers) {
		return;
	}

	for (i = 0; i < consumers->num; ++i) {
		if (consumers->links[i].properties->flags & DL_FLAG_STATELESS) {
			device_link_del(consumers->links[i].link);
		}
	}

	kfree(consumers->links);

	consumers->num = 0;
	consumers->links = NULL;
}

static int surface_sam_san_probe(struct platform_device *pdev)
{
	const struct san_acpi_consumer *cons;
	struct san_drvdata *drvdata;
	acpi_handle san = ACPI_HANDLE(&pdev->dev);	// _SAN device node
	int status;

	/*
	 * Defer probe if the _SSH driver has not set up the controller yet. This
	 * makes sure we do not fail any initial requests (e.g. _STA request without
	 * which the battery does not get set up correctly). Otherwise register as
	 * consumer to set up a device_link.
	 */
	status = surface_sam_ssh_consumer_register(&pdev->dev);
	if (status) {
		return status == -ENXIO ? -EPROBE_DEFER : status;
	}

	drvdata = kzalloc(sizeof(struct san_drvdata), GFP_KERNEL);
	if (!drvdata) {
		return -ENOMEM;
	}

	drvdata->opreg_ctx.dev = &pdev->dev;

	cons = acpi_device_get_match_data(&pdev->dev);
	status = san_consumers_link(pdev, cons, &drvdata->consumers);
	if (status) {
		goto err_consumers;
	}

	platform_set_drvdata(pdev, drvdata);

	status = acpi_install_address_space_handler(san,
			ACPI_ADR_SPACE_GSBUS,
			&san_opreg_handler,
			NULL, &drvdata->opreg_ctx);

	if (ACPI_FAILURE(status)) {
		status = -ENODEV;
		goto err_install_handler;
	}

	status = san_enable_events(&pdev->dev);
	if (status) {
		goto err_enable_events;
	}

	acpi_walk_dep_device_list(san);
	return 0;

err_enable_events:
	acpi_remove_address_space_handler(san, ACPI_ADR_SPACE_GSBUS, &san_opreg_handler);
err_install_handler:
	platform_set_drvdata(san, NULL);
	san_consumers_unlink(&drvdata->consumers);
err_consumers:
	kfree(drvdata);
	return status;
}

static int surface_sam_san_remove(struct platform_device *pdev)
{
	struct san_drvdata *drvdata = platform_get_drvdata(pdev);
	acpi_handle san = ACPI_HANDLE(&pdev->dev);	// _SAN device node
	acpi_status status = AE_OK;

	acpi_remove_address_space_handler(san, ACPI_ADR_SPACE_GSBUS, &san_opreg_handler);
	san_disable_events();

	san_consumers_unlink(&drvdata->consumers);
	kfree(drvdata);

	platform_set_drvdata(pdev, NULL);
	return status;
}


static const struct san_acpi_consumer san_mshw0091_consumers[] = {
	{ "\\_SB.SRTC", true,  DL_FLAG_PM_RUNTIME | DL_FLAG_STATELESS },
	{ "\\ADP1",     true,  DL_FLAG_PM_RUNTIME | DL_FLAG_STATELESS },
	{ "\\_SB.BAT1", true,  DL_FLAG_PM_RUNTIME | DL_FLAG_STATELESS },
	{ "\\_SB.BAT2", false, DL_FLAG_PM_RUNTIME | DL_FLAG_STATELESS },
	{ },
};

static const struct acpi_device_id surface_sam_san_match[] = {
	{ "MSHW0091", (long unsigned int) san_mshw0091_consumers },
	{ },
};
MODULE_DEVICE_TABLE(acpi, surface_sam_san_match);

struct platform_driver surface_sam_san = {
	.probe = surface_sam_san_probe,
	.remove = surface_sam_san_remove,
	.driver = {
		.name = "surface_sam_san",
		.acpi_match_table = ACPI_PTR(surface_sam_san_match),
	},
};
module_platform_driver(surface_sam_san);

MODULE_AUTHOR("Maximilian Luz <luzmaximilian@gmail.com>");
MODULE_DESCRIPTION("Surface ACPI Notify Driver for 5th Generation Surface Devices");
MODULE_LICENSE("GPL v2");
