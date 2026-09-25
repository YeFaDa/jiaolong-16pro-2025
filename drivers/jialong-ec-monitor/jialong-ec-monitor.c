// SPDX-License-Identifier: GPL-2.0
/*
 * Read-only hwmon bridge for the JIAOLONG Series ACPI EC window.
 *
 * The JIAOLONG firmware exposes its extended EC window through the
 * INOU0000/ECRR method.  The generic ec_sys debugfs interface only exposes
 * the first 256 bytes and cannot be used for these addresses.
 *
 * This driver intentionally contains no EC write operation.  It evaluates
 * ECRR only for a fixed, model-specific allowlist and exposes the results as
 * read-only hwmon sensors.
 */
#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <linux/units.h>

#define JL_HID			"INOU0000"
#define JL_PROJECT_ID		0x0740
#define JL_PROJECT_ID_EXPECTED	0x1a
#define JL_CPU_TEMP		0x043e
#define JL_CPU_TEMP_ALT		0x044c
#define JL_GPU_TEMP		0x044f
#define JL_FAN_CPU_HI		0x0464
#define JL_FAN_CPU_LO		0x0465
#define JL_FAN_GPU_HI		0x046c
#define JL_FAN_GPU_LO		0x046d
#define JL_MODE			0x0751
#define JL_DUTY_CPU		0x075b
#define JL_DUTY_GPU		0x075c
#define JL_PWM_MAX		200
#define JL_PWM_SCALE		U8_MAX
#define JL_EC_READ_DELAY_US	6000

struct jialong_ec_monitor {
	acpi_handle handle;
	struct mutex lock;
};

struct jialong_snapshot {
	u8 project_id;
	u8 cpu_temp;
	u8 cpu_temp_alt;
	u8 gpu_temp;
	u8 fan_cpu_hi;
	u8 fan_cpu_lo;
	u8 fan_gpu_hi;
	u8 fan_gpu_lo;
	u8 mode;
	u8 duty_cpu;
	u8 duty_gpu;
};

static bool jialong_dmi_matches(void)
{
	return dmi_match(DMI_SYS_VENDOR, "MECHREVO") &&
	       dmi_match(DMI_BOARD_VENDOR, "MECHREVO") &&
	       dmi_match(DMI_PRODUCT_NAME, "JIAOLONG Series") &&
	       dmi_match(DMI_BOARD_NAME, "JIAOLONG Series-X6xR55xK-B2") &&
	       dmi_match(DMI_BIOS_VERSION, "N.1.16MRO14") &&
	       dmi_match(DMI_EC_FIRMWARE_RELEASE, "1.20");
}

static int jialong_read_byte_unlocked(struct jialong_ec_monitor *monitor,
				      unsigned int address, u8 *value)
{
	union acpi_object argument = {};
	struct acpi_object_list arguments;
	unsigned long long result;
	acpi_status status;

	/* Keep the driver from becoming an arbitrary EC reader. */
	if (address > 0x0fff)
		return -EINVAL;

	argument.integer.type = ACPI_TYPE_INTEGER;
	argument.integer.value = address;
	arguments.count = 1;
	arguments.pointer = &argument;

	status = acpi_evaluate_integer(monitor->handle, "ECRR", &arguments, &result);
	if (ACPI_FAILURE(status))
		return -EIO;
	if (result > U8_MAX)
		return -EIO;

	*value = (u8)result;
	usleep_range(JL_EC_READ_DELAY_US, JL_EC_READ_DELAY_US * 2);
	return 0;
}

static int jialong_read_byte(struct jialong_ec_monitor *monitor,
				     unsigned int address, u8 *value)
{
	int ret;

	mutex_lock(&monitor->lock);
	ret = jialong_read_byte_unlocked(monitor, address, value);
	mutex_unlock(&monitor->lock);
	return ret;
}

static int jialong_read_u16_be(struct jialong_ec_monitor *monitor,
				       unsigned int address, u16 *value)
{
	u8 high, low;
	int ret;

	mutex_lock(&monitor->lock);
	ret = jialong_read_byte_unlocked(monitor, address, &high);
	if (!ret)
		ret = jialong_read_byte_unlocked(monitor, address + 1, &low);
	mutex_unlock(&monitor->lock);
	if (ret)
		return ret;

	*value = ((u16)high << 8) | low;
	return 0;
}

static int jialong_read_snapshot(struct jialong_ec_monitor *monitor,
				 struct jialong_snapshot *snapshot)
{
	int ret;

	mutex_lock(&monitor->lock);
#define JL_SNAPSHOT_READ(address, member) \
	do { \
		ret = jialong_read_byte_unlocked(monitor, (address), &(snapshot->member)); \
		if (ret) \
			goto out; \
	} while (0)

	JL_SNAPSHOT_READ(JL_PROJECT_ID, project_id);
	JL_SNAPSHOT_READ(JL_CPU_TEMP, cpu_temp);
	JL_SNAPSHOT_READ(JL_CPU_TEMP_ALT, cpu_temp_alt);
	JL_SNAPSHOT_READ(JL_GPU_TEMP, gpu_temp);
	JL_SNAPSHOT_READ(JL_FAN_CPU_HI, fan_cpu_hi);
	JL_SNAPSHOT_READ(JL_FAN_CPU_LO, fan_cpu_lo);
	JL_SNAPSHOT_READ(JL_FAN_GPU_HI, fan_gpu_hi);
	JL_SNAPSHOT_READ(JL_FAN_GPU_LO, fan_gpu_lo);
	JL_SNAPSHOT_READ(JL_MODE, mode);
	JL_SNAPSHOT_READ(JL_DUTY_CPU, duty_cpu);
	JL_SNAPSHOT_READ(JL_DUTY_GPU, duty_gpu);
#undef JL_SNAPSHOT_READ
out:
	mutex_unlock(&monitor->lock);
	return ret;
}

static int jialong_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			      u32 attr, int channel, long *value)
{
	struct jialong_ec_monitor *monitor = dev_get_drvdata(dev);
	u8 byte;
	u16 rpm;
	int ret;

	if (!monitor)
		return -ENODEV;

	if (attr != hwmon_temp_input && attr != hwmon_fan_input &&
	    attr != hwmon_pwm_input)
		return -EOPNOTSUPP;

	switch (type) {
	case hwmon_temp:
		if (channel < 0 || channel > 1)
			return -EOPNOTSUPP;
		ret = jialong_read_byte(monitor, channel ? JL_GPU_TEMP : JL_CPU_TEMP,
					 &byte);
		if (ret)
			return ret;
		*value = byte * MILLIDEGREE_PER_DEGREE;
		return 0;
	case hwmon_fan:
		if (channel < 0 || channel > 1)
			return -EOPNOTSUPP;
		ret = jialong_read_u16_be(monitor,
					 channel ? JL_FAN_GPU_HI : JL_FAN_CPU_HI,
					 &rpm);
		if (ret)
			return ret;
		*value = rpm;
		return 0;
	case hwmon_pwm:
		if (channel < 0 || channel > 1)
			return -EOPNOTSUPP;
		ret = jialong_read_byte(monitor, channel ? JL_DUTY_GPU : JL_DUTY_CPU,
					 &byte);
		if (ret)
			return ret;
		if (byte > JL_PWM_MAX)
			return -EIO;
		*value = DIV_ROUND_CLOSEST((u32)byte * JL_PWM_SCALE, JL_PWM_MAX);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int jialong_hwmon_read_string(struct device *dev,
				     enum hwmon_sensor_types type, u32 attr,
				     int channel, const char **string)
{
	if (attr != hwmon_temp_label && attr != hwmon_fan_label)
		return -EOPNOTSUPP;

	switch (type) {
	case hwmon_temp:
	case hwmon_fan:
		if (channel < 0 || channel > 1)
			return -EOPNOTSUPP;
		*string = channel ? "GPU" : "CPU";
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t jialong_hwmon_is_visible(const void *drvdata,
					enum hwmon_sensor_types type,
					u32 attr, int channel)
{
	(void)drvdata;

	if (channel < 0 || channel > 1)
		return 0;

	switch (type) {
	case hwmon_temp:
		if (attr == hwmon_temp_input || attr == hwmon_temp_label)
			return 0444;
		break;
	case hwmon_fan:
		if (attr == hwmon_fan_input || attr == hwmon_fan_label)
			return 0444;
		break;
	case hwmon_pwm:
		if (attr == hwmon_pwm_input)
			return 0444;
		break;
	default:
		break;
	}

	return 0;
}

static const struct hwmon_ops jialong_hwmon_ops = {
	.is_visible = jialong_hwmon_is_visible,
	.read = jialong_hwmon_read,
	.read_string = jialong_hwmon_read_string,
};

static const struct hwmon_channel_info *const jialong_hwmon_info[] = {
	HWMON_CHANNEL_INFO(chip, HWMON_C_REGISTER_TZ),
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT),
	NULL
};

static const struct hwmon_chip_info jialong_hwmon_chip_info = {
	.ops = &jialong_hwmon_ops,
	.info = jialong_hwmon_info,
};

static int jialong_ec_probe(struct platform_device *pdev)
{
	struct jialong_ec_monitor *monitor;
	struct jialong_snapshot snapshot;
	struct device *hwmon_dev;
	int ret;

	if (!jialong_dmi_matches()) {
		pr_warn("jialong_ec_monitor: unsupported DMI/firmware; refusing to probe\n");
		return -ENODEV;
	}

	monitor = devm_kzalloc(&pdev->dev, sizeof(*monitor), GFP_KERNEL);
	if (!monitor)
		return -ENOMEM;

	monitor->handle = ACPI_HANDLE(&pdev->dev);
	if (!monitor->handle)
		return -ENODEV;

	platform_set_drvdata(pdev, monitor);
	mutex_init(&monitor->lock);

	ret = jialong_read_snapshot(monitor, &snapshot);
	if (ret) {
		pr_err("jialong_ec_monitor: initial ECRR read failed: %d\n", ret);
		return ret;
	}
	if (snapshot.project_id != JL_PROJECT_ID_EXPECTED) {
		pr_warn("jialong_ec_monitor: unexpected project 0x%02x; refusing to probe\n",
			snapshot.project_id);
		return -ENODEV;
	}

	hwmon_dev = devm_hwmon_device_register_with_info(&pdev->dev, "jialong_ec",
							 monitor,
							 &jialong_hwmon_chip_info,
							 NULL);
	if (IS_ERR(hwmon_dev)) {
		ret = PTR_ERR(hwmon_dev);
		pr_err("jialong_ec_monitor: hwmon registration failed: %d\n", ret);
		return ret;
	}

	pr_info("jialong_ec_monitor: read-only monitor active (project=0x%02x, "
		"temp=%u/%u C, fan=%u/%u RPM, duty=0x%02x/0x%02x, mode=0x%02x)\n",
		snapshot.project_id, snapshot.cpu_temp, snapshot.gpu_temp,
		((unsigned int)snapshot.fan_cpu_hi << 8) | snapshot.fan_cpu_lo,
		((unsigned int)snapshot.fan_gpu_hi << 8) | snapshot.fan_gpu_lo,
		snapshot.duty_cpu, snapshot.duty_gpu, snapshot.mode);
	return 0;
}

static const struct acpi_device_id jialong_ec_acpi_match[] = {
	{ JL_HID, 0 },
	{ }
};

static struct platform_driver jialong_ec_driver = {
	.probe = jialong_ec_probe,
	.driver = {
		.name = "jialong_ec_monitor",
		.acpi_match_table = jialong_ec_acpi_match,
	},
};

module_platform_driver(jialong_ec_driver);

MODULE_AUTHOR("OpenCode");
MODULE_DESCRIPTION("Read-only JIAOLONG ACPI EC monitor");
MODULE_LICENSE("GPL");
