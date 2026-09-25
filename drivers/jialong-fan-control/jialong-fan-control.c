// SPDX-License-Identifier: GPL-2.0
/*
 * Gated JIAOLONG fan control module based on the QC71 WMI transaction.
 *
 * jialong-ec-monitor remains the independent read-only telemetry module.
 * This module is opt-in: with control_enable unset it creates no write
 * interface and performs no EC write.  The write interface is a root-only,
 * write-only, lease-based test/control surface; it restores the saved PWM
 * state after a timeout or unload.
 */
#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#define QC71_WMI_GUID	"ABBC0F6F-8EA1-11D1-00A0-C90629100000"
#define QC71_WMI_METHOD	4
#define QC71_OUTPUT_EXTRA 40

#define JL_PROJECT_ID		0x0740
#define JL_FAN_CTRL		0x078e
#define JL_MODE			0x0751
#define JL_PWM_CPU		0x1804
#define JL_PWM_GPU		0x1809
#define JL_FAN_CTRL_UNIVERSAL	BIT(6)
#define JL_PWM_MIN		50
#define JL_PWM_MAX		200
#define JL_CONTROL_LEASE_MS	30000

struct jialong_fan_control {
	struct mutex lock;
	struct delayed_work restore_work;
	struct platform_device *device;
	bool controlled;
	u8 changed_mask;
	u8 saved_pwm[2];
	u8 saved_mode;
};

static struct jialong_fan_control control;
static bool control_enable;
module_param(control_enable, bool, 0444);
MODULE_PARM_DESC(control_enable,
	"enable the root-only leased fan write interface (default: disabled)");

static bool jialong_dmi_matches(void)
{
	return dmi_match(DMI_SYS_VENDOR, "MECHREVO") &&
	       dmi_match(DMI_BOARD_VENDOR, "MECHREVO") &&
	       dmi_match(DMI_PRODUCT_NAME, "JIAOLONG Series") &&
	       dmi_match(DMI_BOARD_NAME, "JIAOLONG Series-X6xR55xK-B2") &&
	       dmi_match(DMI_BIOS_VERSION, "N.1.16MRO14") &&
	       dmi_match(DMI_EC_FIRMWARE_RELEASE, "1.20");
}

static int qc71_transaction(u16 address, u16 data, bool read, u8 *value)
{
	u8 input_buffer[8] = {
		(u8)(address & 0xff),
		(u8)(address >> 8),
		(u8)(data & 0xff),
		(u8)(data >> 8),
		0,
		read ? 1 : 0,
		0,
		0,
	};
	u8 output_buffer[sizeof(union acpi_object) + QC71_OUTPUT_EXTRA] = {};
	struct acpi_buffer input = {
		.length = sizeof(input_buffer),
		.pointer = input_buffer,
	};
	struct acpi_buffer output = {
		.length = sizeof(output_buffer),
		.pointer = output_buffer,
	};
	union acpi_object *object;
	acpi_status status;

	status = wmi_evaluate_method(QC71_WMI_GUID, 0, QC71_WMI_METHOD,
				     &input, &output);
	if (ACPI_FAILURE(status))
		return -EIO;
	if (read) {
		object = output.pointer;
		if (!object || object->type != ACPI_TYPE_BUFFER ||
		    object->buffer.length < sizeof(u8))
			return -ENODATA;
		if (value)
			*value = ((u8 *)object->buffer.pointer)[0];
	}
	return 0;
}

static int read_byte(u16 address, u8 *value)
{
	return qc71_transaction(address, 0, true, value);
}

static int write_byte(u16 address, u8 value)
{
	return qc71_transaction(address, value, false, NULL);
}

static u16 fan_address(u8 fan)
{
	return fan ? JL_PWM_GPU : JL_PWM_CPU;
}

static int restore_locked(void)
{
	int ret = 0;
	u8 value;

	if (!control.controlled)
		return 0;

	if ((control.changed_mask & BIT(0)) &&
	    write_byte(fan_address(0), control.saved_pwm[0]))
		ret = -EIO;
	if ((control.changed_mask & BIT(1)) &&
	    write_byte(fan_address(1), control.saved_pwm[1]))
		ret = -EIO;

	if (!read_byte(JL_MODE, &value)) {
		if (value != control.saved_mode) {
			if (write_byte(JL_MODE, control.saved_mode))
				ret = -EIO;
		}
	} else {
		ret = -EIO;
	}

	pr_info("jialong_fan_control: restored changed_mask=0x%02x "
		"CPU PWM=0x%02x GPU PWM=0x%02x mode=0x%02x\n",
		control.changed_mask, control.saved_pwm[0], control.saved_pwm[1],
		control.saved_mode);
	control.controlled = false;
	control.changed_mask = 0;
	return ret;
}

static void restore_work_fn(struct work_struct *work)
{
	mutex_lock(&control.lock);
	restore_locked();
	mutex_unlock(&control.lock);
}

static ssize_t fan_control_store(struct device *dev, struct device_attribute *attr,
					const char *buf, size_t count)
{
	unsigned int fan, raw;
	u8 project, fan_ctrl, mode, readback = 0;
	int ret;

	(void)dev;
	(void)attr;
	if (!control_enable)
		return -EPERM;
	if (sscanf(buf, "%u %u", &fan, &raw) != 2 ||
	    fan < 1 || fan > 2 || raw < JL_PWM_MIN || raw > JL_PWM_MAX)
		return -EINVAL;

	mutex_lock(&control.lock);
	if (!control.controlled) {
		ret = read_byte(JL_PROJECT_ID, &project);
		if (!ret)
			ret = read_byte(JL_FAN_CTRL, &fan_ctrl);
		if (!ret)
			ret = read_byte(JL_MODE, &mode);
		if (!ret)
			ret = read_byte(fan_address(0), &control.saved_pwm[0]);
		if (!ret)
			ret = read_byte(fan_address(1), &control.saved_pwm[1]);
		if (ret)
			goto out;
		if (project != 0x1a || !(fan_ctrl & JL_FAN_CTRL_UNIVERSAL)) {
			pr_err("jialong_fan_control: refusing identity/control "
			       "project=0x%02x fan_ctrl=0x%02x\n", project, fan_ctrl);
			ret = -EPERM;
			goto out;
		}
		control.saved_mode = mode;
		control.controlled = true;
	}

	control.changed_mask |= BIT(fan - 1);
	ret = write_byte(fan_address(fan - 1), raw);
	if (!ret)
		ret = read_byte(fan_address(fan - 1), &readback);
	if (ret || readback != raw) {
		pr_err("jialong_fan_control: write verification failed ret=%d "
		       "value=0x%02x\n", ret, readback);
		restore_locked();
		ret = ret ? ret : -EIO;
		goto out;
	}

	pr_info("jialong_fan_control: fan%u PWM=0x%02x; lease %d ms\n",
		fan, readback, JL_CONTROL_LEASE_MS);
	schedule_delayed_work(&control.restore_work,
			      msecs_to_jiffies(JL_CONTROL_LEASE_MS));
	ret = 0;
out:
	mutex_unlock(&control.lock);
	return ret ? ret : count;
}

static DEVICE_ATTR_WO(fan_control);

static int __init jialong_fan_control_init(void)
{
	int ret;

	if (!jialong_dmi_matches()) {
		pr_warn("jialong_fan_control: unsupported DMI/firmware; refusing\n");
		return -ENODEV;
	}
	if (!wmi_has_guid(QC71_WMI_GUID)) {
		pr_warn("jialong_fan_control: QC71 WMI GUID unavailable\n");
		return -ENODEV;
	}

	mutex_init(&control.lock);
	INIT_DELAYED_WORK(&control.restore_work, restore_work_fn);
	control.device = platform_device_alloc("jialong_fan_control", -1);
	if (!control.device)
		return -ENOMEM;

	ret = platform_device_add(control.device);
	if (ret) {
		platform_device_put(control.device);
		control.device = NULL;
		return ret;
	}

	if (control_enable) {
		ret = device_create_file(&control.device->dev,
					 &dev_attr_fan_control);
		if (ret) {
			platform_device_unregister(control.device);
			control.device = NULL;
			return ret;
		}
		pr_info("jialong_fan_control: leased write interface enabled\n");
	} else {
		pr_info("jialong_fan_control: read-only safety mode\n");
	}
	return 0;
}

static void __exit jialong_fan_control_exit(void)
{
	flush_delayed_work(&control.restore_work);
	mutex_lock(&control.lock);
	restore_locked();
	mutex_unlock(&control.lock);

	if (control.device) {
		if (control_enable)
			device_remove_file(&control.device->dev,
					  &dev_attr_fan_control);
		platform_device_unregister(control.device);
		control.device = NULL;
	}
}

module_init(jialong_fan_control_init);
module_exit(jialong_fan_control_exit);

MODULE_AUTHOR("OpenCode");
MODULE_DESCRIPTION("Gated JIAOLONG QC71-based fan control");
MODULE_LICENSE("GPL");
