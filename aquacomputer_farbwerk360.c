// SPDX-License-Identifier: GPL-2.0+
/*
 * hwmon driver for Aquacomputer Farbwerk 360 (RGB controller)
 *
 * The Farbwerk 360 sends HID reports (with ID 0x01) every second to report sensor values
 * of up to four connected temperature sensors.
 *
 * Copyright 2022 Aleksa Savic <savicaleksa83@gmail.com>
 */

#include <asm/unaligned.h>
#include <linux/debugfs.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/seq_file.h>

#define DRIVER_NAME		"aquacomputer_farbwerk360"

#define STATUS_REPORT_ID	0x01
#define STATUS_UPDATE_INTERVAL	(2 * HZ) /* In seconds */

/* Register offsets */
#define SERIAL_FIRST_PART	0x03
#define SERIAL_SECOND_PART	0x05
#define FIRMWARE_VERSION	0x0D

#define NUM_SENSORS		4
#define SENSOR_START		0x32
#define SENSOR_SIZE		0x02
#define SENSOR_DISCONNECTED	0x7FFF

static const char *const label_temps[] = {
	"Sensor 1",
	"Sensor 2",
	"Sensor 3",
	"Sensor 4"
};

struct farbwerk360_data {
	struct hid_device *hdev;
	struct device *hwmon_dev;
	struct dentry *debugfs;
	s32 temp_input[4];
	u32 serial_number[2];
	u16 firmware_version;
	unsigned long updated;
};

static umode_t farbwerk360_is_visible(const void *data, enum hwmon_sensor_types type, u32 attr,
				 int channel)
{
	return 0444;
}

static int farbwerk360_read(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel,
		       long *val)
{
	struct farbwerk360_data *priv = dev_get_drvdata(dev);

	if (time_after(jiffies, priv->updated + STATUS_UPDATE_INTERVAL))
		return -ENODATA;

	switch (type) {
	case hwmon_temp:
		*val = priv->temp_input[channel];
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int farbwerk360_read_string(struct device *dev, enum hwmon_sensor_types type, u32 attr,
			      int channel, const char **str)
{
	switch (type) {
	case hwmon_temp:
		*str = label_temps[channel];
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static const struct hwmon_ops farbwerk360_hwmon_ops = {
	.is_visible = farbwerk360_is_visible,
	.read = farbwerk360_read,
	.read_string = farbwerk360_read_string,
};

static const struct hwmon_channel_info *farbwerk360_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL
			   ),
	NULL
};

static const struct hwmon_chip_info farbwerk360_chip_info = {
	.ops = &farbwerk360_hwmon_ops,
	.info = farbwerk360_info,
};

static int farbwerk360_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data, int size)
{
	int i, sensor_value;
	struct farbwerk360_data *priv;

	if (report->id != STATUS_REPORT_ID)
		return 0;

	priv = hid_get_drvdata(hdev);

	/* Info provided with every report */
	priv->serial_number[0] = get_unaligned_be16(data + SERIAL_FIRST_PART);
	priv->serial_number[1] = get_unaligned_be16(data + SERIAL_SECOND_PART);

	priv->firmware_version = get_unaligned_be16(data + FIRMWARE_VERSION);

	/* Temperature sensor readings */
	for (i = 0; i < NUM_SENSORS; i++)
	{
		sensor_value = get_unaligned_be16(data + SENSOR_START + i * SENSOR_SIZE);
		if (sensor_value == SENSOR_DISCONNECTED)
			sensor_value = 0;

		priv->temp_input[i] = sensor_value * 10;
	}

	priv->updated = jiffies;

	return 0;
}

#ifdef CONFIG_DEBUG_FS

static int serial_number_show(struct seq_file *seqf, void *unused)
{
	struct farbwerk360_data *priv = seqf->private;

	seq_printf(seqf, "%05u-%05u\n", priv->serial_number[0], priv->serial_number[1]);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(serial_number);

static int firmware_version_show(struct seq_file *seqf, void *unused)
{
	struct farbwerk360_data *priv = seqf->private;

	seq_printf(seqf, "%u\n", priv->firmware_version);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(firmware_version);

static void farbwerk360_debugfs_init(struct farbwerk360_data *priv)
{
	char name[32];

	scnprintf(name, sizeof(name), "%s-%s", DRIVER_NAME, dev_name(&priv->hdev->dev));

	priv->debugfs = debugfs_create_dir(name, NULL);
	debugfs_create_file("serial_number", 0444, priv->debugfs, priv, &serial_number_fops);
	debugfs_create_file("firmware_version", 0444, priv->debugfs, priv, &firmware_version_fops);
}

#else

static void farbwerk360_debugfs_init(struct farbwerk360_data *priv)
{
}

#endif

static int farbwerk360_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct farbwerk360_data *priv;
	int ret;

	priv = devm_kzalloc(&hdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->hdev = hdev;
	hid_set_drvdata(hdev, priv);

	priv->updated = jiffies - STATUS_UPDATE_INTERVAL;

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret)
		return ret;

	ret = hid_hw_open(hdev);
	if (ret)
		goto fail_and_stop;

	priv->hwmon_dev = hwmon_device_register_with_info(&hdev->dev, "farbwerk360", priv,
							  &farbwerk360_chip_info, NULL);

	if (IS_ERR(priv->hwmon_dev)) {
		ret = PTR_ERR(priv->hwmon_dev);
		goto fail_and_close;
	}

	farbwerk360_debugfs_init(priv);

	return 0;

fail_and_close:
	hid_hw_close(hdev);
fail_and_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void farbwerk360_remove(struct hid_device *hdev)
{
	struct farbwerk360_data *priv = hid_get_drvdata(hdev);

	debugfs_remove_recursive(priv->debugfs);
	hwmon_device_unregister(priv->hwmon_dev);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id farbwerk360_table[] = {
	{ HID_USB_DEVICE(0x0c70, 0xf010) }, /* Aquacomputer Farbwerk 360 */
	{},
};

MODULE_DEVICE_TABLE(hid, farbwerk360_table);

static struct hid_driver farbwerk360_driver = {
	.name = DRIVER_NAME,
	.id_table = farbwerk360_table,
	.probe = farbwerk360_probe,
	.remove = farbwerk360_remove,
	.raw_event = farbwerk360_raw_event,
};

static int __init farbwerk360_init(void)
{
	return hid_register_driver(&farbwerk360_driver);
}

static void __exit farbwerk360_exit(void)
{
	hid_unregister_driver(&farbwerk360_driver);
}

/* Request to initialize after the HID bus to ensure it's not being loaded before */
late_initcall(farbwerk360_init);
module_exit(farbwerk360_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Aleksa Savic <savicaleksa83@gmail.com>");
MODULE_DESCRIPTION("Hwmon driver for Aquacomputer Farbwerk 360");
