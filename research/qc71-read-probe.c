// SPDX-License-Identifier: GPL-2.0
/*
 * Read-only QC71/WMI transport probe for JIAOLONG.
 *
 * This is deliberately not qc71_laptop and contains no EC write operation.
 * It uses the same WMI method ID and input layout as qc71_laptop, with the
 * read flag set, and prints a fixed set of addresses for comparison with the
 * ECRR-based jialong-ec-monitor module.
 */
#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>

#define QC71_WMI_GUID	"ABBC0F6F-8EA1-11D1-00A0-C90629100000"
#define QC71_WMI_METHOD	4
#define QC71_OUTPUT_EXTRA 40

struct jialong_register {
	u16 address;
	const char *name;
};

static const struct jialong_register jialong_registers[] = {
	{ 0x0740, "project" },
	{ 0x0751, "mode" },
	{ 0x043e, "cpu_temp" },
	{ 0x044f, "gpu_temp" },
	{ 0x0464, "fan_cpu_hi" },
	{ 0x0465, "fan_cpu_lo" },
	{ 0x046c, "fan_gpu_hi" },
	{ 0x046d, "fan_gpu_lo" },
	{ 0x075b, "duty_cpu" },
	{ 0x075c, "duty_gpu" },
	{ 0x078e, "fan_ctrl" },
	{ 0x1804, "qc71_pwm_cpu" },
	{ 0x1809, "qc71_pwm_gpu" },
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

/* This is the read-only qc71_ec_transaction() format from qc71_laptop. */
static int qc71_read_byte(u16 address, u8 *value)
{
	u8 input_buffer[8] = {
		(u8)(address & 0xff),
		(u8)(address >> 8),
		0, 0,
		0,
		1, /* read operation */
		0, 0,
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

	object = output.pointer;
	if (!object || object->type != ACPI_TYPE_BUFFER ||
	    object->buffer.length < sizeof(u8))
		return -ENODATA;

	*value = ((u8 *)object->buffer.pointer)[0];
	return 0;
}

static int __init jialong_qc71_read_init(void)
{
	unsigned int i;

	if (!jialong_dmi_matches()) {
		pr_warn("jialong_qc71_read: unsupported DMI/firmware; refusing to probe\n");
		return -ENODEV;
	}
	if (!wmi_has_guid(QC71_WMI_GUID)) {
		pr_warn("jialong_qc71_read: QC71 WMI GUID is unavailable\n");
		return -ENODEV;
	}

	pr_info("jialong_qc71_read: read-only WMI probe active\n");
	for (i = 0; i < ARRAY_SIZE(jialong_registers); i++) {
		const struct jialong_register *reg = &jialong_registers[i];
		u8 value;
		int ret;

		ret = qc71_read_byte(reg->address, &value);
		if (ret)
			pr_err("jialong_qc71_read: %s 0x%03x failed: %d\n",
			       reg->name, reg->address, ret);
		else
			pr_info("jialong_qc71_read: %s 0x%03x=0x%02x\n",
				reg->name, reg->address, value);
	}

	return 0;
}

static void __exit jialong_qc71_read_exit(void)
{
	pr_info("jialong_qc71_read: unloaded\n");
}

module_init(jialong_qc71_read_init);
module_exit(jialong_qc71_read_exit);

MODULE_AUTHOR("OpenCode");
MODULE_DESCRIPTION("Read-only JIAOLONG QC71 WMI transport probe");
MODULE_LICENSE("GPL");
