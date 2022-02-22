// SPDX-License-Identifier: GPL-2.0+
/*
 * hwmon driver for Aquacomputer Octo fan controller
 *
 *
 * Copyright 2022 Aleksa Savic <savicaleksa83@gmail.com>
 */

#include <asm/unaligned.h>
#include <linux/crc16.h>
#include <linux/debugfs.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>

#define DRIVER_NAME		"aquacomputer_octo"

#define SENSOR_REPORT_ID	0x01
#define SENSOR_UPDATE_INTERVAL	(2 * HZ) /* In seconds */

/* Register offsets for debug info */
#define SERIAL_FIRST_PART	0x03
#define SERIAL_SECOND_PART	0x05
#define FIRMWARE_VERSION	0x0D

/* Register offsets for fans */
#define NUM_FANS		8
#define FAN_PERCENT_OFFSET	0x00
#define FAN_VOLTAGE_OFFSET	0x02
#define FAN_CURRENT_OFFSET	0x04
#define FAN_POWER_OFFSET	0x06
#define FAN_SPEED_OFFSET	0x08

/* Registers for reading fan-related info */
static u8 sensor_fan_offsets[] = {
	0x7D,
	0x8A,
	0x97,
	0xA4,
	0xB1,
	0xBE,
	0xCB,
	0xD8
};

/* Registers for writing fan speeds (0-100%)*/
/* static u16 input_fan_offsets[] = {
	0x5B,
	0xB0,
	0x105,
	0x15A,
	0x1AF,
	0x204,
	0x259,
	0x2AE
};*/

/* Register offsets for temperature sensors */
#define NUM_SENSORS		4
#define SENSORS_START		0x3D
#define SENSOR_SIZE		0x02
#define SENSOR_DISCONNECTED	0x7FFF

#define STATUS_REPORT_ID		0x03
#define STATUS_REPORT_SIZE		0x65F /* TODO */
#define STATUS_REPORT_CHECKSUM_OFFSET	0x65D /* gde je checksum */
#define STATUS_REPORT_CHECKSUM_START	0x01
#define STATUS_REPORT_CHECKSUM_LENGTH	0x65C /* pred pocetak checksum */

/* The HID report that the official software always sends after writing values */
#define SECONDARY_STATUS_REPORT_ID	0x02
#define SECONDARY_STATUS_REPORT_SIZE	0x0B

static u8 secondary_status_report[] = {
	0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x34, 0xC6
};

static const char *const label_temps[] = {
	"Sensor 1",
	"Sensor 2",
	"Sensor 3",
	"Sensor 4"
};

static const char *const label_speeds[] = {
	"Fan 1 speed",
	"Fan 2 speed",
	"Fan 3 speed",
	"Fan 4 speed",
	"Fan 5 speed",
	"Fan 6 speed",
	"Fan 7 speed",
	"Fan 8 speed"
};

static const char *const label_power[] = {
	"Fan 1 power",
	"Fan 2 power",
	"Fan 3 power",
	"Fan 4 power",
	"Fan 5 power",
	"Fan 6 power",
	"Fan 7 power",
	"Fan 8 power"
};

static const char *const label_voltages[] = {
	"Fan 1 voltage",
	"Fan 2 voltage",
	"Fan 3 voltage",
	"Fan 4 voltage",
	"Fan 5 voltage",
	"Fan 6 voltage",
	"Fan 7 voltage",
	"Fan 8 voltage"
};

static const char *const label_current[] = {
	"Fan 1 current",
	"Fan 2 current",
	"Fan 3 current",
	"Fan 4 current",
	"Fan 5 current",
	"Fan 6 current",
	"Fan 7 current",
	"Fan 8 current"
};

struct octo_data {
	struct hid_device *hdev;
	struct device *hwmon_dev;
	struct dentry *debugfs;
	s32 temp_input[4];
	u16 speed_input[8];
	u32 power_input[8];
	u16 voltage_input[8];
	u16 current_input[8];
	u32 serial_number[2];
	u16 firmware_version;
	unsigned long updated;
};

static umode_t octo_is_visible(const void *data, enum hwmon_sensor_types type, u32 attr,
				 int channel)
{
	return 0444;
}

static int octo_read(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel,
		       long *val)
{
	int ret;
	struct octo_data *priv = dev_get_drvdata(dev);

	if (time_after(jiffies, priv->updated + SENSOR_UPDATE_INTERVAL))
		return -ENODATA;

	switch (type) {
	case hwmon_temp:
		*val = priv->temp_input[channel];
		break;
	case hwmon_fan:
		*val = priv->speed_input[channel];
		break;
	case hwmon_power:
		*val = priv->power_input[channel];
		break;
	case hwmon_in:
		*val = priv->voltage_input[channel];
		break;
	case hwmon_curr:
		*val = priv->current_input[channel];
		break;
	default:
		return -EOPNOTSUPP;
	}

	return ret;
}

static int octo_read_string(struct device *dev, enum hwmon_sensor_types type, u32 attr,
			      int channel, const char **str)
{
	switch (type) {
	case hwmon_temp:
		*str = label_temps[channel];
		break;
	case hwmon_fan:
		*str = label_speeds[channel];
		break;
	case hwmon_power:
		*str = label_power[channel];
		break;
	case hwmon_in:
		*str = label_voltages[channel];
		break;
	case hwmon_curr:
		*str = label_current[channel];
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static const struct hwmon_ops octo_hwmon_ops = {
	.is_visible = octo_is_visible,
	.read = octo_read,
	.read_string = octo_read_string
};

static const struct hwmon_channel_info *octo_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL
			   ),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL
			   ),
	HWMON_CHANNEL_INFO(power,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL
			   ),
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL
			   ),
	HWMON_CHANNEL_INFO(curr,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL
			   ),
	NULL
};

static const struct hwmon_chip_info octo_chip_info = {
	.ops = &octo_hwmon_ops,
	.info = octo_info,
};

static int octo_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data, int size)
{
	int i, sensor_value;
	struct octo_data *priv;

	if (report->id != SENSOR_REPORT_ID)
		return 0;

	priv = hid_get_drvdata(hdev);

	/* Info provided with every report */
	priv->serial_number[0] = get_unaligned_be16(data + SERIAL_FIRST_PART);
	priv->serial_number[1] = get_unaligned_be16(data + SERIAL_SECOND_PART);

	priv->firmware_version = get_unaligned_be16(data + FIRMWARE_VERSION);

	/* Temperature sensor readings */
	for (i = 0; i < NUM_SENSORS; i++) {
		 sensor_value = get_unaligned_be16(data + SENSORS_START + i * SENSOR_SIZE);
		 if (sensor_value == SENSOR_DISCONNECTED)
			 sensor_value = 0;

		 priv->temp_input[i] = sensor_value * 10;
	}

	/* Fan speed and PWM readings */
	for (i = 0; i < NUM_FANS; i++) {
		priv->speed_input[i] = get_unaligned_be16(data + sensor_fan_offsets[i] + FAN_SPEED_OFFSET);
		priv->power_input[i] = get_unaligned_be16(data + sensor_fan_offsets[i] + FAN_POWER_OFFSET) * 10000;
		priv->voltage_input[i] = get_unaligned_be16(data + sensor_fan_offsets[i] + FAN_VOLTAGE_OFFSET) * 10;
		priv->current_input[i] = get_unaligned_be16(data + sensor_fan_offsets[i] + FAN_CURRENT_OFFSET);
	}

	priv->updated = jiffies;

	return 0;
}

#ifdef CONFIG_DEBUG_FS

static int serial_number_show(struct seq_file *seqf, void *unused)
{
	struct octo_data *priv = seqf->private;

	seq_printf(seqf, "%05u-%05u\n", priv->serial_number[0], priv->serial_number[1]);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(serial_number);

static int firmware_version_show(struct seq_file *seqf, void *unused)
{
	struct octo_data *priv = seqf->private;

	seq_printf(seqf, "%u\n", priv->firmware_version);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(firmware_version);

static void debugfs_init(struct octo_data *priv)
{
	char name[32];

	scnprintf(name, sizeof(name), "%s-%s", DRIVER_NAME, dev_name(&priv->hdev->dev));

	priv->debugfs = debugfs_create_dir(name, NULL);
	debugfs_create_file("serial_number", 0444, priv->debugfs, priv, &serial_number_fops);
	debugfs_create_file("firmware_version", 0444, priv->debugfs, priv, &firmware_version_fops);
}

#else

static void debugfs_init(struct octo_data *priv)
{
}

#endif

static int octo_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int ret;
	struct octo_data *priv;

	priv = devm_kzalloc(&hdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->hdev = hdev;
	hid_set_drvdata(hdev, priv);

	priv->updated = jiffies - SENSOR_UPDATE_INTERVAL;

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret)
		return ret;

	ret = hid_hw_open(hdev);
	if (ret)
		goto fail_and_stop;

	/*hid_device_io_start(hdev);*/

	priv->hwmon_dev = hwmon_device_register_with_info(&hdev->dev, "octo", priv,
							  &octo_chip_info, NULL);

	if (IS_ERR(priv->hwmon_dev)) {
		ret = PTR_ERR(priv->hwmon_dev);
		goto fail_and_close;
	}

	debugfs_init(priv);

	return 0;

fail_and_close:
	hid_hw_close(hdev);
fail_and_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void octo_remove(struct hid_device *hdev)
{
	struct octo_data *priv = hid_get_drvdata(hdev);

	debugfs_remove_recursive(priv->debugfs);
	hwmon_device_unregister(priv->hwmon_dev);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id octo_table[] = {
	{ HID_USB_DEVICE(0x0c70, 0xf011) }, /* Aquacomputer Octo */
	{},
};

MODULE_DEVICE_TABLE(hid, octo_table);

static struct hid_driver octo_driver = {
	.name = DRIVER_NAME,
	.id_table = octo_table,
	.probe = octo_probe,
	.remove = octo_remove,
	.raw_event = octo_raw_event,
};

static int __init octo_init(void)
{
	return hid_register_driver(&octo_driver);
}

static void __exit octo_exit(void)
{
	hid_unregister_driver(&octo_driver);
}

/* Request to initialize after the HID bus to ensure it's not being loaded before */
late_initcall(octo_init);
module_exit(octo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Aleksa Savic <savicaleksa83@gmail.com>");
MODULE_DESCRIPTION("Hwmon driver for Aquacomputer Octo fan controller");
