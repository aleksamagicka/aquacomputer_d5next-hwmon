// SPDX-License-Identifier: GPL-2.0+
/*
 * hwmon driver for Aquacomputer D5 Next watercooling pump
 *
 * The D5 Next sends HID reports (with ID 0x01) every second to report sensor values
 * (coolant temperature, pump and fan speed, voltage, current and power). This driver
 * also allows controlling the pump and fan speed via PWM.
 *
 * Copyright 2021 Aleksa Savic <savicaleksa83@gmail.com>,
 * Copyright 2022 Jack Doan <me@jackdoan.com>
 */

#include <asm/unaligned.h>
#include <linux/byteorder/generic.h>
#include <linux/crc16.h>
#include <linux/debugfs.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/stddef.h>
#include <linux/seq_file.h>

#define DRIVER_NAME "aquacomputer-d5next"

#define INTERRUPT_STATUS_REPORT_ID	0x01
#define CMD_WRITE_SETTINGS		0x03
#define CMD_SAVE_SETTINGS		0x02
#define STATUS_UPDATE_INTERVAL		(2 * HZ) /* In seconds */

#define SECONDARY_STATUS_REPORT_SIZE 0xB

/* Start index and length of the part of the report that gets checksummed */
#define STATUS_REPORT_CHECKSUM_START	0x01
#define STATUS_REPORT_CHECKSUM_LENGTH	0x326

enum fan_event_flags {
	FAN_EVENT_NO_SOURCE = 0x1,
	FAN_EVENT_RPM_WARN = 0x2,
	FAN_EVENT_RPM_ALARM = 0x4,
	FAN_EVENT_TEMPERATURE_WARN = 0x8,
	FAN_EVENT_TEMPERATURE_ALARM = 0x10,
	FAN_EVENT_OVERCURRENT = 0x20,
	FAN_EVENT_SHORT_CIRCUIT = 0x40,
	FAN_EVENT_BOOSTING = 0x80,
	FAN_EVENT_VCC_ERROR = 0x100,
	FAN_EVENT_UNDER_EXTERNAL_CONTROL = 0x200
};

struct fan_event_data {
	u16 pwm; /* per-thousand, setpoint? */
	u16 voltage;
	u16 current_ma;
	u16 watts;
	u16 speed; /* rpm, measurement */
	u16 torque; /* Ncm */
	u8  flags; //enum fan_event_flags
} __attribute__((packed));

struct pump_event_data {
	u16 pwm; /* per-thousand, setpoint? */
	u16 voltage;
	u16 current_ma;
	u16 watts;
	u16 speed; /* rpm, measurement */
	u8  flags; //enum fan_event_flags
} __attribute__((packed));

struct d5next_raw_event_data {
	u16 report_id;
	u8 pad1;
	u16 serial_number[2];
	/* starts at 7 */
	u8 pad2[6];
	u16 firmware_version; // 0x0d
	u8 pad3[11];
	u32 power_cycles; // 0x18
	u8 pad4[25];
	u16 vcc_12;
	u16 vbus; //0x39
	u16 vcc_aquabus;
	u8 pad5[26];
	s16 coolant_temp; //0x57
	s16 flow_data[3];
	struct fan_event_data fan;
	struct pump_event_data pump;
	struct {
		u16 fan;	//0x77
		u16 pump;	//0x79
	} setpoints;
	u8 pad6[14];
	u16 flow_cal_progress;
	u32 alarms[2];

} __attribute__((packed));

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

struct fan_properties {
	u8 flags;
	u16 min_pwm; /* r/w, expressed in big-endian as percent */
	u16 max_pwm; /* r/w, expressed in big-endian as percent */
	u16 fallback_pwm; /* expressed in big-endian as percent, purpose currently unclear */
	u16 max_speed; /* expressed in big-endian as RPM. R/W. Purpose unclear. */
} __attribute__((packed)); /* size = 9 */

enum fan_control_mode {
	FAN_CONTROL_MANUAL = 0x0u,
	FAN_CONTROL_PID = 0x1u,
	FAN_CONTROL_CURVE = 0x2u,
} __attribute__((__packed__));

struct fan_control_pid_data {
	u16 setpoint;
	u16 proportional;
	u16 integral;
	u16 derivative;
	u16 d_tn;
	u16 hysteresis;
	u16 invert_and_flags;
} __attribute__((__packed__)); /* size = 7 */

enum curve_control_point {
	CURVE_CONTROL_POINT_MIN = 0x00u,
	CURVE_CONTROL_POINT_MAX = 0x0fu,
	CURVE_CONTROL_POINT_AMOUNT = 0x10u,
	CURVE_CONTROL_STARTING_TEMPERATURE = 0x11u,
	CURVE_CONTROL_MIN_PWM = 0x12u,
	CURVE_CONTROL_MAX_PWM = 0x13u,
};

struct fan_control_curve_data {
	u16 start_temp;
	u16 temps[CURVE_CONTROL_POINT_AMOUNT];
	u16 powers[CURVE_CONTROL_POINT_AMOUNT];
} __attribute__((__packed__)); /* size = 33 */

struct fan_ctrl {
	enum fan_control_mode mode;
	u16 manual_setpoint; /* expressed in big-endian as percent */
	/* Refers to which sensor to apply a curve from.
	 * 0 is the internal water temp sensor.
	 * Other values currently unknown */
	u16 source;
	struct fan_control_pid_data pid;
	struct fan_control_curve_data curve;
} __attribute__((packed)); /* size = 45 */

enum d5next_ctrl_channel {
	FAN_CONTROL_CHANNEL_FAN = 0x00,
	FAN_CONTROL_CHANNEL_PUMP = 0x01,
	FAN_CONTROL_NUM_CHANNELS = 0x02
};

enum d5next_ctrl_channel_userspace {
	USERSPACE_CHANNEL_PUMP = 0x00,
	USERSPACE_CHANNEL_FAN = 0x01,
	USERSPACE_NUM_CHANNELS = 0x02
};

static enum d5next_ctrl_channel d5next_userspace_to_internal_channel(enum d5next_ctrl_channel_userspace x)
{
	switch (x) {
		case USERSPACE_CHANNEL_PUMP:
			return FAN_CONTROL_CHANNEL_PUMP;
		case USERSPACE_CHANNEL_FAN:
			return FAN_CONTROL_CHANNEL_FAN;
		default:
			return -EINVAL;
	}
}

struct d5next_control_data {
	u8 version;
	u8 padding[44];
	s16 temp_sensor_offset;
	struct fan_properties fan_properties[FAN_CONTROL_NUM_CHANNELS];
	struct fan_ctrl fan_ctrl[FAN_CONTROL_NUM_CHANNELS];
	u8 padding2[565];
	u16 alarm_flags;
	u16 flow_limit;
	s16 water_temp_alarm_limit;
	u8 profile_id;
	u16 crc;
} __attribute__((packed));

#define STATUS_REPORT_SIZE	sizeof(struct d5next_control_data)  /* 809 */

struct d5next_alarms {
	bool vcc12; //(alarms & 1L)
	bool vcc5; //(alarms & 2L)
	bool fan_short_circuit; //(alarms & 4L)
	bool fan_over_current; //(alarms & 8L)
	bool pump_speed; //(alarms & 0x10L)
	bool water_temperature; //(alarms & 0x200L)
	bool fan_speed; //(alarms & 0x400L)
};

static void update_alarms(struct d5next_alarms* alarms, u32 data) {
	alarms->vcc12 = (data & BIT(0)) != 0;
	alarms->vcc5 = (data & BIT(1)) != 0;
	alarms->fan_short_circuit = (data & BIT(2)) != 0;
	alarms->fan_over_current = (data & BIT(3)) != 0;
	alarms->pump_speed = (data & BIT(4)) != 0;
	alarms->water_temperature = (data & BIT(9)) != 0;
	alarms->fan_speed = (data & BIT(10)) != 0;
}

struct d5next_data {
	struct hid_device *hdev;
	struct device *hwmon_dev;
	struct dentry *debugfs;
	struct mutex mutex;
	struct d5next_control_data *buffer;
	const struct attribute_group *groups[1];
	s32 temp_input;
	u16 speed_input[USERSPACE_NUM_CHANNELS];
	u16 speed_setpoint[USERSPACE_NUM_CHANNELS];
	u32 power_input[2];
	u16 voltage_input[3];
	u16 current_input[2];
	u32 serial_number[2];
	u16 firmware_version;
	u32 power_cycles; /* How many times the device was powered on */
	struct d5next_alarms alarms;
	unsigned long updated;
};


static bool fan_channel_is_valid(enum d5next_ctrl_channel channel)
{
	return (channel >= 0 && channel < FAN_CONTROL_NUM_CHANNELS);
}

static bool curve_point_is_valid(u8 idx)
{
	if (idx >= CURVE_CONTROL_POINT_MIN && idx <= CURVE_CONTROL_POINT_MAX) {
		return true;
	}

	switch (idx) {
		case CURVE_CONTROL_STARTING_TEMPERATURE:
		case CURVE_CONTROL_MIN_PWM:
		case CURVE_CONTROL_MAX_PWM:
			return true;
		default:
			return false;
	}
}

static int d5next_percent_to_pwm(u16 x)
{
	/*
	 * hwmon expresses PWM settings as a u8.
	 * The D5 Next expresses them as fixed-point big-endian u16 values, like this:
	 * 0x0000 = 0.00% = 0
	 * 0x0a1a = 25.86% = 66
	 * 0x2710 = 100.00% = 255
	 *
	 * So, to go from the pump's representation to hwmon's, we multiply by 255, then divide by (100*100)
	 * We could call it per-ten-thousand, but that's sort of unwieldy. Let's just think of it as fixed-point percent.
	 */
	return DIV_ROUND_CLOSEST(ntohs(x) * 255, 100 * 100);
}

static u16 d5next_pwm_to_percent(u8 x)
{
	/*
	 * Convert to 0-100 range, then multiply by 100, since that's what the pump expects
	 * Also see the explanation in d5next_percent_to_pwm
	 */
	return DIV_ROUND_CLOSEST(x * 100 * 100, 255);
}

static long d5next_temp_to_hwmon_temp(s32 x) {
	return ntohs(x) * 10;
}

static s16 d5next_temp_from_hwmon_temp(long x) {
	/* no ntohs() because d5next_set_val does that for us */
	return DIV_ROUND_CLOSEST(x, 10);
}

/* Note: Expects the mutex to be locked! */
static int d5next_get_ctrl_data(struct device *dev)
{
	int ret;
	struct d5next_data *priv = dev_get_drvdata(dev);
	/* Request the status report */
	memset(priv->buffer, 0x00, STATUS_REPORT_SIZE);
	ret = hid_hw_raw_request(priv->hdev, CMD_WRITE_SETTINGS, (u8 *) (priv->buffer), STATUS_REPORT_SIZE, HID_FEATURE_REPORT, HID_REQ_GET_REPORT);
	if (ret < 0)
		ret = -ENODATA;
	return ret;
}

/*
 * Contents of the HID report that the official software always sends after
 * writing values. Appears to be used to make setting changes persist after a
 * power-cycle
 */
static u8 cmd_save_settings_to_flash[] = {
	0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x34, 0xC6
};

/* Note: Expects the mutex to be locked! */
static int d5next_send_ctrl_data(struct d5next_data *priv)
{
	int ret;
	u16 checksum = 0xffff; /* Init value for CRC-16/USB */
	checksum = crc16(checksum, (u8 *) (priv->buffer) + STATUS_REPORT_CHECKSUM_START, STATUS_REPORT_CHECKSUM_LENGTH);
	checksum ^= 0xffff; /* Xorout value for CRC-16/USB */

	/* Place the new checksum at the end of the report */
	put_unaligned_be16(checksum, &(priv->buffer->crc));

	/* Send the patched up report back to the pump */
	ret = hid_hw_raw_request(priv->hdev, CMD_WRITE_SETTINGS, (u8 *) (priv->buffer), STATUS_REPORT_SIZE, HID_FEATURE_REPORT, HID_REQ_SET_REPORT);
	if (ret < 0)
		goto exit;

	/* The official software sends this report after every change, so do it here as well */
	ret = hid_hw_raw_request(priv->hdev, CMD_SAVE_SETTINGS, cmd_save_settings_to_flash, SECONDARY_STATUS_REPORT_SIZE, HID_FEATURE_REPORT, HID_REQ_SET_REPORT);

exit:
	return ret;
}

/*
 * Goes and grabs a value out of a fresh GET_FEATURE_REPORT for you.
 * If you call this function, your function doesn't need to touch the mutex.
 */
static int d5next_get_val(struct device *dev, void *to_get, void *val, size_t size)
{
	int ret;
	struct d5next_data *priv = dev_get_drvdata(dev);

	mutex_lock(&priv->mutex);

	/*
	 * Request a complete config report from the pump by sending a GET_FEATURE_REPORT and
	 * store it in dev->buffer
	*/
	if (size < 1 || size > 4) {
		ret = -EINVAL;
		goto unlock_and_return;
	}
	ret = d5next_get_ctrl_data(dev);
	if (ret < 0)
		goto unlock_and_return;

	memcpy(val, to_get, size);

unlock_and_return:
	mutex_unlock(&priv->mutex);
	return ret;
}

/*
 * Performs a read-modify-write of the pump control data for you of the specified size in bits.
 * Currently only supports 8 and 16, since that's all this driver has needed so far. Other sizes will return EINVAL
 */
static int d5next_set_val(struct device *dev, void *to_set, long val, size_t size)
{
	int ret;
	struct d5next_data *priv = dev_get_drvdata(dev);

	mutex_lock(&priv->mutex);

	/*
	 * Request a complete config report from the pump by sending a GET_FEATURE_REPORT and
	 * store it in dev->buffer
	*/
	ret = d5next_get_ctrl_data(dev);
	if (ret < 0)
		goto unlock_and_return;

	/*
	 * Set values accordingly. We only modify those values so that any other
	 * settings, such as RGB lights, stay untouched
	 */
	switch (size) {
		case 16:
			put_unaligned_be16(val, to_set);
			break;
		case 8:
			*(u8 *) to_set = val;
			break;
		default:
			ret = -EINVAL;
			goto unlock_and_return;
	}

	ret = d5next_send_ctrl_data(priv);

unlock_and_return:
	mutex_unlock(&priv->mutex);
	return ret;
}

static int d5next_set_u16_val(struct device *dev, u16 *to_set, long val)
{
	return d5next_set_val(dev, to_set, val, 16);
}

static int d5next_set_u8_val(struct device *dev, u8 *to_set, long val)
{
	return d5next_set_val(dev, to_set, val, 8);
}

static ssize_t d5next_get_pwm_setting(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct sensor_device_attribute_2 *sensor_attr = to_sensor_dev_attr_2(attr);
	int channel = d5next_userspace_to_internal_channel(sensor_attr->nr);
	int idx = sensor_attr->index;
	struct d5next_data *priv = dev_get_drvdata(dev);
	int ret;
	void *to_get;
	u16 val = 0;

	if (!fan_channel_is_valid(channel))
		return -ENOENT;
	if (!curve_point_is_valid(idx))
		return -ENOENT;

	if (idx == CURVE_CONTROL_MIN_PWM)
		to_get = &priv->buffer->fan_properties[channel].min_pwm;
	else if (idx == CURVE_CONTROL_MAX_PWM)
		to_get = &priv->buffer->fan_properties[channel].max_pwm;
	else
		to_get = &priv->buffer->fan_ctrl[channel].curve.powers[idx];

	ret = d5next_get_val(dev, to_get, &val, 2);
	if (ret < 0)
		return ret;

	ret = sprintf(buf, "%d\n", d5next_percent_to_pwm(val));
	return ret;
}

static ssize_t d5next_get_auto_temp(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct sensor_device_attribute_2 *sensor_attr = to_sensor_dev_attr_2(attr);
	int channel = d5next_userspace_to_internal_channel(sensor_attr->nr);
	int idx = sensor_attr->index;
	struct d5next_data *priv = dev_get_drvdata(dev);
	int ret;
	const char fmt[] = "%d0\n"; /* slap a 0 on the end of the reading to go from centi-degrees to milli-degrees */
	void *to_get;
	u16 val = 0;

	if (!fan_channel_is_valid(channel))
		return -ENOENT;
	if (!curve_point_is_valid(idx))
		return -ENOENT;
	if (idx == CURVE_CONTROL_STARTING_TEMPERATURE)
		to_get = &priv->buffer->fan_ctrl[channel].curve.start_temp;
	else
		to_get = &priv->buffer->fan_ctrl[channel].curve.temps[idx];

	ret = d5next_get_val(dev, to_get, &val, 2);
	if (ret < 0)
		return ret;

	ret = sprintf(buf, fmt, ntohs(val));
	return ret;
}

static ssize_t d5next_set_auto_temp(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct sensor_device_attribute_2 *sensor_attr = to_sensor_dev_attr_2(attr);
	int channel = d5next_userspace_to_internal_channel(sensor_attr->nr);
	int idx = sensor_attr->index;
	struct d5next_data *priv = dev_get_drvdata(dev);
	u16 * to_set;
	int ret;
	long val;

	if (!fan_channel_is_valid(channel))
		return -ENOENT;
	if (!curve_point_is_valid(idx))
		return -ENOENT;
	if (kstrtol(buf, 10, &val) < 0)
		return -EINVAL;

	/* pump wants values in centi-degrees Celsius, hwmon uses mdegC */
	val = d5next_temp_from_hwmon_temp(val);
	if (idx == CURVE_CONTROL_STARTING_TEMPERATURE)
		to_set = &(priv->buffer->fan_ctrl[channel].curve.start_temp);
	else
		to_set = &(priv->buffer->fan_ctrl[channel].curve.temps[idx]);

	ret = d5next_set_u16_val(dev, to_set, val);
	return ret;
}

static ssize_t d5next_set_auto_pwm(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct sensor_device_attribute_2 *sensor_attr = to_sensor_dev_attr_2(attr);
	int channel = d5next_userspace_to_internal_channel(sensor_attr->nr);
	int idx = sensor_attr->index;
	struct d5next_data *priv = dev_get_drvdata(dev);
	int ret;
	long val;

	if (!fan_channel_is_valid(channel))
		return -ENOENT;
	if (!curve_point_is_valid(idx))
		return -ENOENT;
	if (kstrtol(buf, 10, &val) < 0)
		return -EINVAL;
	if (val < 0 || val > 255)
		return -EINVAL;

	val = d5next_pwm_to_percent(val);

	ret = d5next_set_u16_val(dev, &(priv->buffer->fan_ctrl[channel].curve.powers[idx]), val);
	return ret;
}

static umode_t d5next_auto_pwm_is_visible(struct kobject *kobj, struct attribute *attr, int index)
{
	/* these settings are always visible, they exist on every d5next */
	return attr->mode;
}

static umode_t d5next_is_visible(const void *data, enum hwmon_sensor_types type, u32 attr, int channel)
{
	switch (type) {
		case hwmon_fan:
			switch (attr) {
				case hwmon_fan_max:
					return 0644;
				default:
					break;
			}
			break;
		case hwmon_pwm:
			switch (attr) {
				case hwmon_pwm_enable:
				case hwmon_pwm_input:
					return 0644;
				default:
					break;
			}
			break;
		case hwmon_temp:
			switch (attr) {
				case hwmon_temp_max:
				case hwmon_temp_offset:
					return 0644;
				default:
					break;
			}
			break;
		default:
			break;
	}

	return 0444;
}

static int d5next_read_pwm(struct device *dev, u32 attr, int channel, long *val)
{
	int ret;
	struct fan_ctrl *fan_control_data;
	/* Request the status report and extract current PWM values */
	struct d5next_data *priv = dev_get_drvdata(dev);
	mutex_lock(&priv->mutex);
	ret = d5next_get_ctrl_data(dev);
	channel = d5next_userspace_to_internal_channel(channel);

	if (ret < 0 || !fan_channel_is_valid(channel)) {
		ret = -ENODATA;
		goto unlock_and_return;
	}

	fan_control_data = &(priv->buffer->fan_ctrl[channel]);
	switch (attr) {
		case hwmon_pwm_input:
			switch (fan_control_data->mode) {
				case FAN_CONTROL_MANUAL:
					*val = fan_control_data->manual_setpoint;
					break;
				default:
					*val = priv->speed_setpoint[channel];
					break;
			}
			break;
		case hwmon_pwm_enable:
			*val = fan_control_data->mode;
			break;
		default:
			ret = -ENODATA;
			goto unlock_and_return;
	}

unlock_and_return:
	mutex_unlock(&priv->mutex);
	return ret;
}

static int d5next_read_fan(struct device *dev, u32 attr, int channel, long *val)
{
	int ret;
	struct fan_properties *fan_props;
	/* Request the status report and extract current PWM values */
	struct d5next_data *priv = dev_get_drvdata(dev);
	mutex_lock(&priv->mutex);
	ret = d5next_get_ctrl_data(dev);

	channel = d5next_userspace_to_internal_channel(channel);
	if (ret < 0 || !fan_channel_is_valid(channel)) {
		ret = -ENODATA;
		goto unlock_and_return;
	}

	fan_props = &(priv->buffer->fan_properties[channel]);
	switch (attr) {
		case hwmon_fan_input:
			*val = priv->speed_input[channel];
			break;
		case hwmon_fan_max:
			*val = ntohs(fan_props->max_speed);
			break;
		default:
			ret = -ENODATA;
			goto unlock_and_return;
	}

unlock_and_return:
	mutex_unlock(&priv->mutex);
	return ret;
}

static int d5next_read_temp(struct device *dev, u32 attr, long *val)
{
	/* Request the status report and extract current temp sensor values */
	int ret;
	struct d5next_data *priv = dev_get_drvdata(dev);
	mutex_lock(&priv->mutex);
	ret = d5next_get_ctrl_data(dev);

	switch (attr) {
		case hwmon_temp_input:
			*val = priv->temp_input;
			break;
		case hwmon_temp_offset:
			*val = d5next_temp_to_hwmon_temp(priv->buffer->temp_sensor_offset);
			break;
		case hwmon_temp_max_alarm:
			*val = priv->alarms.water_temperature;
			break;
		case hwmon_temp_max:
			*val = d5next_temp_to_hwmon_temp(priv->buffer->water_temp_alarm_limit);
			break;
		default:
			ret = -ENODATA;
			goto unlock_and_return;
	}

unlock_and_return:
	mutex_unlock(&priv->mutex);
	return ret;
}

/* todo pull the curve data functions into this somehow */
static int d5next_read(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel, long *val)
{
	/* deferring translation of channel value to called functions inside the switch */
	int ret = 0;
	struct d5next_data *priv = dev_get_drvdata(dev);

	if (time_after(jiffies, priv->updated + STATUS_UPDATE_INTERVAL))
		return -ENODATA;

	switch (type) {
		case hwmon_temp:
			ret = d5next_read_temp(dev, attr, val);
			break;
		case hwmon_fan:
			ret = d5next_read_fan(dev, attr, channel, val);
			break;
		case hwmon_power:
			*val = priv->power_input[channel];
			break;
		case hwmon_pwm:
			ret = d5next_read_pwm(dev, attr, channel, val);
			break;
		case hwmon_in:
			*val = priv->voltage_input[channel];
			break;
		case hwmon_curr:
			*val = priv->current_input[channel];
			break;
		default:
			ret = -EOPNOTSUPP;
	}

	return ret;
}

static int d5next_read_string(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel, const char **str)
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

static int d5next_set_pwm(struct device *dev, enum d5next_ctrl_channel channel, u32 attr, long val)
{
	struct d5next_data *priv = dev_get_drvdata(dev);
	if (!fan_channel_is_valid(channel))
		return -ENOENT;

	switch (attr) {
		case hwmon_pwm_input:
			if (val < 0 || val > 255)
				return -EINVAL;
			val = d5next_pwm_to_percent(val);
			/* TODO: Ensure that the pump is configured to use fan ctrl out, and not flow sensor in! */
			return d5next_set_u16_val(dev, &(priv->buffer->fan_ctrl[channel].manual_setpoint), val);
		case hwmon_pwm_enable:
			if (val < FAN_CONTROL_MANUAL || val > FAN_CONTROL_CURVE)
				return -EINVAL;
			return d5next_set_u8_val(dev, &(priv->buffer->fan_ctrl[channel].mode), val);
		default:
			return -EOPNOTSUPP;
	}
}

static int d5next_set_fan(struct device *dev, enum d5next_ctrl_channel channel, u32 attr, long val)
{
	struct d5next_data *priv = dev_get_drvdata(dev);
	if (!fan_channel_is_valid(channel))
		return -ENOENT;
	switch (attr) {
		case hwmon_fan_max:
			return d5next_set_u16_val(dev, &(priv->buffer->fan_properties[channel].max_speed), val);
		default:
			return -EOPNOTSUPP;
	}
}

static int d5next_set_temp(struct device *dev, u32 attr, long val)
{
	struct d5next_data *priv = dev_get_drvdata(dev);
	switch (attr) {
		case hwmon_temp_max:
			return d5next_set_u16_val(dev, &(priv->buffer->water_temp_alarm_limit), val / 10);
		case hwmon_temp_offset:
			return d5next_set_u16_val(dev, &(priv->buffer->temp_sensor_offset), val / 10);
		default:
			return -EOPNOTSUPP;
	}
}

static int d5next_write(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel, long val)
{
	switch (type) {
		case hwmon_pwm:
			channel = d5next_userspace_to_internal_channel(channel);
			return d5next_set_pwm(dev, channel, attr, val);
		case hwmon_fan:
			channel = d5next_userspace_to_internal_channel(channel);
			return d5next_set_fan(dev, channel, attr, val);
		case hwmon_temp:
			if (channel != 0)
				return -EOPNOTSUPP;
			return d5next_set_temp(dev, attr, val);
		default:
			return -EOPNOTSUPP;
	}
}

static const struct hwmon_ops d5next_hwmon_ops = {
	.is_visible = d5next_is_visible,
	.read = d5next_read,
	.read_string = d5next_read_string,
	.write = d5next_write
};

static const struct hwmon_channel_info *d5next_info[] = {
	HWMON_CHANNEL_INFO(temp,
			HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_MAX_ALARM | HWMON_T_MAX | HWMON_T_OFFSET
			),
	HWMON_CHANNEL_INFO(fan,
			HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_MAX,
			HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_MAX
			),
	HWMON_CHANNEL_INFO(pwm,
			HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
			HWMON_PWM_INPUT | HWMON_PWM_ENABLE
			),
	HWMON_CHANNEL_INFO(power,
			HWMON_P_INPUT | HWMON_P_LABEL,
			HWMON_P_INPUT | HWMON_P_LABEL
			),
	HWMON_CHANNEL_INFO(in,
			HWMON_I_INPUT | HWMON_I_LABEL,
			HWMON_I_INPUT | HWMON_I_LABEL,
			HWMON_I_INPUT | HWMON_I_LABEL
			),
	HWMON_CHANNEL_INFO(curr,
			HWMON_C_INPUT | HWMON_C_LABEL,
			HWMON_C_INPUT | HWMON_C_LABEL
			),
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
	/* Info provided with every sensor report */
	struct d5next_raw_event_data *event_data = (struct d5next_raw_event_data *)data;

	if(report->id != INTERRUPT_STATUS_REPORT_ID)
		return 0;
	if(size < sizeof(struct d5next_raw_event_data))
		return 0;

	priv = hid_get_drvdata(hdev);

	priv->serial_number[0] = ntohs(event_data->serial_number[0]);
	priv->serial_number[1] = ntohs(event_data->serial_number[1]);

	priv->firmware_version = ntohs(event_data->firmware_version);
	priv->power_cycles = ntohs(event_data->power_cycles);

	/* Sensor readings */
	priv->temp_input = d5next_temp_to_hwmon_temp(event_data->coolant_temp);
	/*
	 * NOTE! The driver uses fan = index 0 / pump = index 1 internally.
	 * However, we report the pump as [pwm|fan]_1 and the fan as [pwm|fan]_2 to userspace.
	 * Only use `enum d5next_ctrl_channel` to set these values,
	 * and make sure you translate any `channel` value from hwmon first!
	 *
	 * Functions that take an already converted channel as input shall use an
	 * `enum d5next_ctrl_channel` as their channel argument.
	 */
	priv->speed_input[FAN_CONTROL_CHANNEL_PUMP] = ntohs(event_data->pump.speed);
	priv->speed_input[FAN_CONTROL_CHANNEL_FAN] = ntohs(event_data->fan.speed);

	priv->speed_setpoint[FAN_CONTROL_CHANNEL_PUMP] = d5next_percent_to_pwm(event_data->setpoints.pump);
	priv->speed_setpoint[FAN_CONTROL_CHANNEL_FAN] = d5next_percent_to_pwm(event_data->setpoints.fan);

	priv->power_input[0] = ntohs(event_data->pump.watts) * 10000;
	priv->power_input[1] = ntohs(event_data->fan.watts) * 10000;

	priv->voltage_input[0] = ntohs(event_data->pump.voltage) * 10;
	priv->voltage_input[1] = ntohs(event_data->fan.voltage) * 10;
	priv->voltage_input[2] = ntohs(event_data->vbus) * 10;
	/* todo we can technically also report event_data->vcc_12 and event_data->vcc_aquabus, but does anyone care? */

	priv->current_input[0] = ntohs(event_data->pump.current_ma);
	priv->current_input[1] = ntohs(event_data->fan.current_ma);

	update_alarms(&(priv->alarms), ntohs(event_data->alarms[0]));

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
	int temp;
	int power;

	for (i = 0; i < sizeof(struct d5next_control_data); i++) {
		seq_printf(seqf, "%02x ", ((u8 *) (priv->buffer))[i]);
		if ((i + 1) % 16 == 0)
			seq_printf(seqf, "\n");
	}

	seq_printf(seqf, "\n");
	seq_printf(seqf, "fan1 setpt: %d\n", ntohs(((struct d5next_control_data *) priv->buffer)->fan_ctrl[0].manual_setpoint));
	seq_printf(seqf, "pump setpt: %d\n", ntohs(((struct d5next_control_data *) priv->buffer)->fan_ctrl[1].manual_setpoint));
	seq_printf(seqf, "crc: %x\n", ntohs(((struct d5next_control_data *) priv->buffer)->crc));
	for (i = 0; i < CURVE_CONTROL_POINT_AMOUNT; i++) {
		temp = ntohs(((struct d5next_control_data *) priv->buffer)->fan_ctrl[0].curve.temps[i]);
		power = ntohs(((struct d5next_control_data *) priv->buffer)->fan_ctrl[0].curve.powers[i]);
		seq_printf(seqf, "fan1 curve: temp %x power %x\n", temp, power);
	}

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


static SENSOR_DEVICE_ATTR_2(pwm1_auto_point1_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point1_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point2_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN + 1);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point2_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN + 1);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point3_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN + 2);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point3_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN + 2);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point4_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN + 3);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point4_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN + 3);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point5_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN + 4);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point5_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN + 4);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point6_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN + 5);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point6_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN + 5);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point7_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN + 6);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point7_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN + 6);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point8_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN + 7);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point8_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN + 7);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point9_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN + 8);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point9_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN + 8);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point10_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN + 9);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point10_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN + 9);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point11_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN + 10);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point11_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN + 10);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point12_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN + 11);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point12_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN + 11);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point13_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN + 12);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point13_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN + 12);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point14_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN + 13);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point14_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN + 13);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point15_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN + 14);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point15_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MIN + 14);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point16_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm, USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MAX);
static SENSOR_DEVICE_ATTR_2(pwm1_auto_point16_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp, USERSPACE_CHANNEL_PUMP, CURVE_CONTROL_POINT_MAX);

static SENSOR_DEVICE_ATTR_2(pwm1_auto_start_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp, 0, CURVE_CONTROL_STARTING_TEMPERATURE);
static SENSOR_DEVICE_ATTR_2(pwm1_min, 0644, d5next_get_pwm_setting, d5next_set_auto_temp, 0, CURVE_CONTROL_MIN_PWM);
static SENSOR_DEVICE_ATTR_2(pwm1_max, 0644, d5next_get_pwm_setting, d5next_set_auto_temp, 0, CURVE_CONTROL_MAX_PWM);

static SENSOR_DEVICE_ATTR_2(pwm2_auto_point1_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point1_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point2_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN + 1);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point2_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN + 1);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point3_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN + 2);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point3_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN + 2);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point4_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN + 3);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point4_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN + 3);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point5_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN + 4);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point5_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN + 4);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point6_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN + 5);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point6_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN + 5);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point7_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN + 6);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point7_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN + 6);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point8_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN + 7);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point8_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN + 7);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point9_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN + 8);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point9_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN + 8);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point10_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN + 9);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point10_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN + 9);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point11_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN + 10);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point11_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN + 10);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point12_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN + 11);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point12_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN + 11);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point13_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN + 12);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point13_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN + 12);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point14_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN + 13);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point14_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN + 13);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point15_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN + 14);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point15_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MIN + 14);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point16_pwm, 0644, d5next_get_pwm_setting, d5next_set_auto_pwm,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MAX);
static SENSOR_DEVICE_ATTR_2(pwm2_auto_point16_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp,  USERSPACE_CHANNEL_FAN, CURVE_CONTROL_POINT_MAX);

static SENSOR_DEVICE_ATTR_2(pwm2_auto_start_temp, 0644, d5next_get_auto_temp, d5next_set_auto_temp, 1, CURVE_CONTROL_STARTING_TEMPERATURE);
static SENSOR_DEVICE_ATTR_2(pwm2_min, 0644, d5next_get_pwm_setting, d5next_set_auto_temp, 1, CURVE_CONTROL_MIN_PWM);
static SENSOR_DEVICE_ATTR_2(pwm2_max, 0644, d5next_get_pwm_setting, d5next_set_auto_temp, 1, CURVE_CONTROL_MAX_PWM);

static struct attribute *d5next_attributes_auto_pwm[] = {
	&sensor_dev_attr_pwm1_auto_point1_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point2_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point3_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point4_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point5_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point6_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point7_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point8_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point9_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point10_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point11_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point12_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point13_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point14_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point15_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point16_pwm.dev_attr.attr,

	&sensor_dev_attr_pwm1_auto_point1_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point2_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point3_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point4_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point5_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point6_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point7_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point8_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point9_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point10_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point11_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point12_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point13_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point14_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point15_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point16_temp.dev_attr.attr,

	&sensor_dev_attr_pwm1_auto_start_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_min.dev_attr.attr,
	&sensor_dev_attr_pwm1_max.dev_attr.attr,

	&sensor_dev_attr_pwm2_auto_point1_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point2_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point3_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point4_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point5_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point6_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point7_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point8_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point9_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point10_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point11_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point12_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point13_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point14_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point15_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point16_pwm.dev_attr.attr,

	&sensor_dev_attr_pwm2_auto_point1_temp.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point2_temp.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point3_temp.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point4_temp.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point5_temp.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point6_temp.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point7_temp.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point8_temp.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point9_temp.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point10_temp.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point11_temp.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point12_temp.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point13_temp.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point14_temp.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point15_temp.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point16_temp.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_start_temp.dev_attr.attr,
	&sensor_dev_attr_pwm2_min.dev_attr.attr,
	&sensor_dev_attr_pwm2_max.dev_attr.attr,
	NULL,
};

static const struct attribute_group d5next_group_auto_pwm = {
	.attrs = d5next_attributes_auto_pwm,
	.is_visible = d5next_auto_pwm_is_visible,
};

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

	priv->groups[0] = &d5next_group_auto_pwm;

	priv->hwmon_dev = hwmon_device_register_with_info(&hdev->dev, "d5next", priv,
							  &d5next_chip_info, priv->groups);

	if (IS_ERR(priv->hwmon_dev)) {
		ret = (int)PTR_ERR(priv->hwmon_dev);
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
MODULE_AUTHOR("Jack Doan <me@jackdoan.com>");
MODULE_DESCRIPTION("Hwmon driver for Aquacomputer D5 Next pump");
