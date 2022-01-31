// SPDX-License-Identifier: GPL-2.0+
/*
 * hwmon driver for Aquacomputer D5 Next watercooling pump
 *
 * The D5 Next sends HID reports (with ID 0x01) every second to report sensor values
 * (coolant temperature, pump and fan speed, voltage, current and power). This driver
 * also allows controlling the pump and fan speed via PWM.
 *
 * Copyright 2021 Aleksa Savic <savicaleksa83@gmail.com>
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

#define DRIVER_NAME			"aquacomputer_d5next"

#define SENSOR_REPORT_ID		0x01
#define STATUS_REPORT_ID		0x03
#define SECONDARY_STATUS_REPORT_ID	0x02
#define STATUS_UPDATE_INTERVAL		(2 * HZ) /* In seconds */

#define STATUS_REPORT_SIZE		0x329
#define SECONDARY_STATUS_REPORT_SIZE	0xB

/* Start index and length of the part of the report that gets checksummed */
#define STATUS_REPORT_CHECKSUM_START	0x01
#define STATUS_REPORT_CHECKSUM_LENGTH	0x326

/* Offset of the checksum in the status report */
#define STATUS_REPORT_CHECKSUM		0x327

/* Register offsets for the D5 Next pump sensor report */
#define SERIAL_FIRST_PART	0x3
#define SERIAL_SECOND_PART	0x5
#define FIRMWARE_VERSION	0xD
#define POWER_CYCLES		0x18

#define COOLANT_TEMP		0x57

#define PUMP_SPEED		0x74
#define FAN_SPEED		0x67

#define PUMP_POWER		0x72
#define FAN_POWER		0x65

#define PUMP_VOLTAGE		0x6E
#define FAN_VOLTAGE		0x61
#define PLUS_5V_VOLTAGE		0x39

#define PUMP_CURRENT		0x70
#define FAN_CURRENT		0x63

#define OUTPUT_PUMP_SPEED	0x97
#define OUTPUT_FAN_SPEED	0x42

/* Labels for provided values */
#define L_COOLANT_TEMP		"Coolant temp"

#define L_PUMP_SPEED		"Pump speed"
#define L_FAN_SPEED		"Fan speed"

#define L_PUMP_POWER		"Pump power"
#define L_FAN_POWER		"Fan power"

#define L_PUMP_VOLTAGE		"Pump voltage"
#define L_FAN_VOLTAGE		"Fan voltage"
#define L_5V_VOLTAGE		"+5V voltage"

#define L_PUMP_CURRENT		"Pump current"
#define L_FAN_CURRENT		"Fan current"

static const char *const label_speeds[] = {
	L_PUMP_SPEED,
	L_FAN_SPEED,
};

static const char *const label_power[] = {
	L_PUMP_POWER,
	L_FAN_POWER,
};

static const char *const label_voltages[] = {
	L_PUMP_VOLTAGE,
	L_FAN_VOLTAGE,
	L_5V_VOLTAGE,
};

static const char *const label_current[] = {
	L_PUMP_CURRENT,
	L_FAN_CURRENT,
};

/* Contents of the HID report that the official software always sends after writing values */
static u8 secondary_status_report[] = {
	0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x34, 0xC6
};

struct d5next_data {
	struct hid_device *hdev;
	struct device *hwmon_dev;
	struct dentry *debugfs;
	struct mutex mutex;
	u8 *buffer;
	s32 temp_input;
	u16 speed_input[2];
	u32 power_input[2];
	u16 voltage_input[3];
	u16 current_input[2];
	u32 serial_number[2];
	u16 firmware_version;
	u32 power_cycles; /* How many times the device was powered on */
	unsigned long updated;
};

static umode_t d5next_is_visible(const void *data, enum hwmon_sensor_types type, u32 attr,
				 int channel)
{
	return 0777; // TODO
}

static int d5next_read(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel,
		       long *val)
{
	int ret;
	struct d5next_data *priv = dev_get_drvdata(dev);

	if (time_after(jiffies, priv->updated + STATUS_UPDATE_INTERVAL))
		return -ENODATA;

	switch (type) {
	case hwmon_temp:
		*val = priv->temp_input;
		break;
	case hwmon_fan:
		*val = priv->speed_input[channel];
		break;
	case hwmon_power:
		*val = priv->power_input[channel];
		break;
	case hwmon_pwm:
		/* Request the status report and extract current PWM values */
		mutex_lock(&priv->mutex);

		memset(priv->buffer, 0x00, STATUS_REPORT_SIZE);
		ret = hid_hw_raw_request(priv->hdev, STATUS_REPORT_ID, priv->buffer, STATUS_REPORT_SIZE, HID_FEATURE_REPORT, HID_REQ_GET_REPORT);
		if (ret < 0)
			goto unlock;

		switch (channel)
		{
		case 0:
			*val = get_unaligned_be16(priv->buffer + OUTPUT_PUMP_SPEED);
			break;
		case 1:
			*val = get_unaligned_be16(priv->buffer + OUTPUT_FAN_SPEED);
			break;
		default:
			break;
		}

		*val = DIV_ROUND_CLOSEST(*val*255, 100 * 100);

unlock:
		mutex_unlock(&priv->mutex);

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

	return 0;
}

static int d5next_read_string(struct device *dev, enum hwmon_sensor_types type, u32 attr,
			      int channel, const char **str)
{
	switch (type) {
	case hwmon_temp:
		*str = L_COOLANT_TEMP;
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

static int d5next_set_pwm(struct d5next_data *dev, int channel, long val)
{
	int ret;
	u16 checksum = 0xffff; /* Init value for CRC-16/USB */

	if (val < 0 || val > 255)
		return -EINVAL;

	mutex_lock(&dev->mutex);

	/* Convert to 0-100 range, then multiply by 100, since that's what the pump expects */

	val = DIV_ROUND_CLOSEST(val * 100 * 100, 255);

	/*
	 * Request a complete config report from the pump by sending a GET_FEATURE_REPORT and
	 * store it in dev->buffer
	*/
	memset(dev->buffer, 0x00, STATUS_REPORT_SIZE);
	ret = hid_hw_raw_request(dev->hdev, STATUS_REPORT_ID, dev->buffer, STATUS_REPORT_SIZE, HID_FEATURE_REPORT, HID_REQ_GET_REPORT);

	if (ret < 0)
		goto unlock_and_return;

	/*
	 * Set pump or fan PWM values, accordingly. We only modify those values so that any other
	 * settings, such as RGB lights, stay untouched
	 */

	// TODO: Ensure that the pump is configured to follow a single value, which is set below, so reset it from any other 'curve' modes!

	switch (channel) {
	case 0:
		put_unaligned_be16(val, dev->buffer + OUTPUT_PUMP_SPEED);
		break;
	case 1:
		put_unaligned_be16(val, dev->buffer + OUTPUT_FAN_SPEED);
		break;
	default:
		break;
	}

	checksum = crc16(checksum, dev->buffer + STATUS_REPORT_CHECKSUM_START, STATUS_REPORT_CHECKSUM_LENGTH);
 	checksum ^= 0xffff; /* Xorout value for CRC-16/USB */

	/* Place the new checksum at the end of the report */
	put_unaligned_be16(checksum, dev->buffer + STATUS_REPORT_CHECKSUM);

	/* Send the patched up report back to the pump */
	ret = hid_hw_raw_request(dev->hdev, STATUS_REPORT_ID, dev->buffer, STATUS_REPORT_SIZE, HID_FEATURE_REPORT, HID_REQ_SET_REPORT);
	if (ret < 0)
		goto unlock_and_return;

	/* The official software sends this report after every change, so do it here as well */
	ret = hid_hw_raw_request(dev->hdev, SECONDARY_STATUS_REPORT_ID, secondary_status_report, SECONDARY_STATUS_REPORT_SIZE, HID_FEATURE_REPORT, HID_REQ_SET_REPORT);
	if (ret < 0)
		goto unlock_and_return;

unlock_and_return:
	mutex_unlock(&dev->mutex);
	return ret;
}

static int d5next_write(struct device *dev, enum hwmon_sensor_types type, u32 attr,
			      int channel, long val)
{
	struct d5next_data *priv = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			return d5next_set_pwm(priv, channel, val);
		default:
			break;
		}
		break;
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static const struct hwmon_ops d5next_hwmon_ops = {
	.is_visible = d5next_is_visible,
	.read = d5next_read,
	.read_string = d5next_read_string,
	.write = d5next_write
};

static const struct hwmon_channel_info *d5next_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT | HWMON_F_LABEL, HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT, HWMON_PWM_INPUT),
	HWMON_CHANNEL_INFO(power, HWMON_P_INPUT | HWMON_P_LABEL, HWMON_P_INPUT | HWMON_P_LABEL),
	HWMON_CHANNEL_INFO(in, HWMON_I_INPUT | HWMON_I_LABEL, HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL),
	HWMON_CHANNEL_INFO(curr, HWMON_C_INPUT | HWMON_C_LABEL, HWMON_C_INPUT | HWMON_C_LABEL),
	NULL
};

static const struct hwmon_chip_info d5next_chip_info = {
	.ops = &d5next_hwmon_ops,
	.info = d5next_info,
};

/* Parses sensor reports which the pump automatically sends every second */
static int d5next_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data, int size)
{
	struct d5next_data *priv;

	if (report->id != SENSOR_REPORT_ID)
		return 0;

	priv = hid_get_drvdata(hdev);

	/* Info provided with every sensor report */

	priv->serial_number[0] = get_unaligned_be16(data + SERIAL_FIRST_PART);
	priv->serial_number[1] = get_unaligned_be16(data + SERIAL_SECOND_PART);

	priv->firmware_version = get_unaligned_be16(data + FIRMWARE_VERSION);
	priv->power_cycles = get_unaligned_be32(data + POWER_CYCLES);

	/* Sensor readings */

	priv->temp_input = get_unaligned_be16(data + COOLANT_TEMP) * 10;

	priv->speed_input[0] = get_unaligned_be16(data + PUMP_SPEED);
	priv->speed_input[1] = get_unaligned_be16(data + FAN_SPEED);

	priv->power_input[0] = get_unaligned_be16(data + PUMP_POWER) * 10000;
	priv->power_input[1] = get_unaligned_be16(data + FAN_POWER) * 10000;

	priv->voltage_input[0] = get_unaligned_be16(data + PUMP_VOLTAGE) * 10;
	priv->voltage_input[1] = get_unaligned_be16(data + FAN_VOLTAGE) * 10;
	priv->voltage_input[2] = get_unaligned_be16(data + PLUS_5V_VOLTAGE) * 10;

	priv->current_input[0] = get_unaligned_be16(data + PUMP_CURRENT);
	priv->current_input[1] = get_unaligned_be16(data + FAN_CURRENT);

	priv->updated = jiffies;

	return 0;
}

#ifdef CONFIG_DEBUG_FS

static int serial_number_show(struct seq_file *seqf, void *unused)
{
	struct d5next_data *priv = seqf->private;

	seq_printf(seqf, "%05u-%05u\n", priv->serial_number[0], priv->serial_number[1]);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(serial_number);

static int firmware_version_show(struct seq_file *seqf, void *unused)
{
	struct d5next_data *priv = seqf->private;

	seq_printf(seqf, "%u\n", priv->firmware_version);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(firmware_version);

static int power_cycles_show(struct seq_file *seqf, void *unused)
{
	struct d5next_data *priv = seqf->private;

	seq_printf(seqf, "%u\n", priv->power_cycles);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(power_cycles);

static int raw_buffer_show(struct seq_file *seqf, void *unused)
{
	struct d5next_data *priv = seqf->private;

	int i;

	for (i = 0; i < STATUS_REPORT_SIZE; i++)
	{
		seq_printf(seqf, "%02x ", priv->buffer[i]);
	}

	seq_printf(seqf, "\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(raw_buffer);

static void d5next_debugfs_init(struct d5next_data *priv)
{
	char name[32];

	scnprintf(name, sizeof(name), "%s-%s", DRIVER_NAME, dev_name(&priv->hdev->dev));

	priv->debugfs = debugfs_create_dir(name, NULL);
	debugfs_create_file("serial_number", 0444, priv->debugfs, priv, &serial_number_fops);
	debugfs_create_file("firmware_version", 0444, priv->debugfs, priv, &firmware_version_fops);
	debugfs_create_file("power_cycles", 0444, priv->debugfs, priv, &power_cycles_fops);
	debugfs_create_file("raw_buffer", 0444, priv->debugfs, priv, &raw_buffer_fops);
}

#else

static void d5next_debugfs_init(struct d5next_data *priv)
{
}

#endif

static int d5next_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct d5next_data *priv;
	int ret;

	priv = devm_kzalloc(&hdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->buffer = devm_kzalloc(&hdev->dev, STATUS_REPORT_SIZE, GFP_KERNEL);
	if (!priv->buffer)
		return -ENOMEM;

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

	priv->hdev = hdev;
	hid_set_drvdata(hdev, priv);
	mutex_init(&priv->mutex);

	hid_device_io_start(hdev);

	priv->hwmon_dev = hwmon_device_register_with_info(&hdev->dev, "d5next", priv,
							  &d5next_chip_info, NULL);

	if (IS_ERR(priv->hwmon_dev)) {
		ret = PTR_ERR(priv->hwmon_dev);
		goto fail_and_close;
	}

	d5next_debugfs_init(priv);

	return 0;

fail_and_close:
	hid_hw_close(hdev);
fail_and_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void d5next_remove(struct hid_device *hdev)
{
	struct d5next_data *priv = hid_get_drvdata(hdev);

	mutex_unlock(&priv->mutex);

	debugfs_remove_recursive(priv->debugfs);
	hwmon_device_unregister(priv->hwmon_dev);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id d5next_table[] = {
	{ HID_USB_DEVICE(0x0c70, 0xf00e) }, /* Aquacomputer D5 Next */
	{},
};

MODULE_DEVICE_TABLE(hid, d5next_table);

static struct hid_driver d5next_driver = {
	.name = DRIVER_NAME,
	.id_table = d5next_table,
	.probe = d5next_probe,
	.remove = d5next_remove,
	.raw_event = d5next_raw_event,
};

static int __init d5next_init(void)
{
	return hid_register_driver(&d5next_driver);
}

static void __exit d5next_exit(void)
{
	hid_unregister_driver(&d5next_driver);
}

/* Request to initialize after the HID bus to ensure it's not being loaded before */

late_initcall(d5next_init);
module_exit(d5next_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Aleksa Savic <savicaleksa83@gmail.com>");
MODULE_DESCRIPTION("Hwmon driver for Aquacomputer D5 Next pump");
