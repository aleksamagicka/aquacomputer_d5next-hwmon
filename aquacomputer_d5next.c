// SPDX-License-Identifier: GPL-2.0+
/*
 * hwmon driver for Aquacomputer devices (D5 Next, Farbwerk, Farbwerk 360, Octo,
 * Quadro, High Flow Next, Aquaero, Leakshield, Aquastream XT)
 *
 * Aquacomputer devices send HID reports (with ID 0x01) every second to report
 * sensor values, with the exception of Aquastream XT.
 *
 * Copyright 2021 Aleksa Savic <savicaleksa83@gmail.com>
 * Copyright 2022 Jack Doan <me@jackdoan.com>
 */

#include <linux/crc16.h>
#include <linux/debugfs.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/usb.h>
#include <asm/unaligned.h>

#define USB_VENDOR_ID_AQUACOMPUTER	0x0c70
#define USB_PRODUCT_ID_AQUAERO		0xf001
#define USB_PRODUCT_ID_FARBWERK		0xf00a
#define USB_PRODUCT_ID_QUADRO		0xf00d
#define USB_PRODUCT_ID_D5NEXT		0xf00e
#define USB_PRODUCT_ID_FARBWERK360	0xf010
#define USB_PRODUCT_ID_OCTO		0xf011
#define USB_PRODUCT_ID_HIGHFLOWNEXT	0xf012
#define USB_PRODUCT_ID_LEAKSHIELD	0xf014
#define USB_PRODUCT_ID_AQUASTREAMXT	0xf0b6

enum kinds {
	aquaero, d5next, farbwerk, farbwerk360, octo,
	quadro, highflownext, leakshield, aquastreamxt
};

static const char *const aqc_device_names[] = {
	[aquaero] = "aquaero",
	[d5next] = "d5next",
	[farbwerk] = "farbwerk",
	[farbwerk360] = "farbwerk360",
	[octo] = "octo",
	[quadro] = "quadro",
	[highflownext] = "highflownext",
	[leakshield] = "leakshield",
	[aquastreamxt] = "aquastreamxt",
};

#define DRIVER_NAME			"aquacomputer_d5next"

#define STATUS_REPORT_ID		0x01
#define STATUS_UPDATE_INTERVAL		(2 * HZ)	/* In seconds */
#define SERIAL_PART_OFFSET		2

#define CTRL_REPORT_ID			0x03
#define AQUAERO_CTRL_REPORT_ID		0x0b

/*
 * The HID report that the official software always sends
 * after writing values, same for all devices, except Aquaero
 */
#define SECONDARY_CTRL_REPORT_ID	0x02
#define SECONDARY_CTRL_REPORT_SIZE	0x0B

static u8 secondary_ctrl_report[] = {
	0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x34, 0xC6
};

/* Secondary HID report values for Aquaero */
#define AQUAERO_SECONDARY_CTRL_REPORT_ID	0x06
#define AQUAERO_SECONDARY_CTRL_REPORT_SIZE	0x07

static u8 aquaero_secondary_ctrl_report[] = {
	0x06, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00
};

/* Secondary HID report values for Aquastream XT */
#define AQUASTREAMXT_SECONDARY_CTRL_REPORT_ID	0x02
#define AQUASTREAMXT_SECONDARY_CTRL_REPORT_SIZE	0x04

static u8 aquastreamxt_secondary_ctrl_report[] = {
	0x02, 0x05, 0x00, 0x00
};

/* Data types for reading and writing control reports */
#define AQC_8		0
#define AQC_BE16	1
#define AQC_LE16	2

/* Report IDs for legacy devices */
#define AQC_LEGACY_STATUS_REPORT_ID	0x04
#define AQC_LEGACY_CTRL_REPORT_ID	0x06

/* Info, sensor sizes and offsets for all Aquacomputer devices */
#define AQC_SERIAL_START		0x03
#define AQC_FIRMWARE_VERSION		0x0D
#define AQC_POWER_CYCLES		0x18

#define AQC_SENSOR_SIZE			0x02
#define AQC_SENSOR_NA			0x7FFF
#define AQC_FAN_PERCENT_OFFSET		0x00
#define AQC_FAN_VOLTAGE_OFFSET		0x02
#define AQC_FAN_CURRENT_OFFSET		0x04
#define AQC_FAN_POWER_OFFSET		0x06
#define AQC_FAN_SPEED_OFFSET		0x08

/* Report offsets for fan control */
#define AQC_FAN_CTRL_PWM_OFFSET		0x01
#define AQC_FAN_CTRL_TEMP_SELECT_OFFSET	0x03

/* Specs of the Aquaero fan controllers */
#define AQUAERO_SERIAL_START			0x07
#define AQUAERO_FIRMWARE_VERSION		0x0B
#define AQUAERO_NUM_FANS			4
#define AQUAERO_NUM_SENSORS			8
#define AQUAERO_NUM_VIRTUAL_SENSORS		8
#define AQUAERO_NUM_CALC_VIRTUAL_SENSORS	4
#define AQUAERO_NUM_FLOW_SENSORS		2
#define AQUAERO_CTRL_REPORT_SIZE		0xa93
#define AQUAERO_CTRL_PRESET_ID			0x5c
#define AQUAERO_CTRL_PRESET_SIZE		0x02
#define AQUAERO_CTRL_PRESET_START		0x55c

/* Sensor report offsets for Aquaero fan controllers */
#define AQUAERO_SENSOR_START			0x65
#define AQUAERO_VIRTUAL_SENSOR_START		0x85
#define AQUAERO_CALC_VIRTUAL_SENSOR_START	0x95
#define AQUAERO_FLOW_SENSORS_START		0xF9
#define AQUAERO_FAN_VOLTAGE_OFFSET		0x04
#define AQUAERO_FAN_CURRENT_OFFSET		0x06
#define AQUAERO_FAN_POWER_OFFSET		0x08
#define AQUAERO_FAN_SPEED_OFFSET		0x00
static u16 aquaero_sensor_fan_offsets[] = { 0x167, 0x173, 0x17f, 0x18B };

/* Control report offsets for the Aquaero fan controllers */
#define AQUAERO_TEMP_CTRL_OFFSET	0xdb
#define AQUAERO_FAN_CTRL_MIN_RPM_OFFSET	0x00
#define AQUAERO_FAN_CTRL_MAX_RPM_OFFSET	0x02
#define AQUAERO_FAN_CTRL_MIN_PWR_OFFSET	0x04
#define AQUAERO_FAN_CTRL_MODE_OFFSET	0x0f
#define AQUAERO_FAN_CTRL_SRC_OFFSET	0x10
static u16 aquaero_ctrl_fan_offsets[] = { 0x20c, 0x220, 0x234, 0x248 };

/* Specs of the D5 Next pump */
#define D5NEXT_NUM_FANS			2
#define D5NEXT_NUM_SENSORS		1
#define D5NEXT_NUM_VIRTUAL_SENSORS	8
#define D5NEXT_CTRL_REPORT_SIZE		0x329

/* Sensor report offsets for the D5 Next pump */
#define D5NEXT_COOLANT_TEMP		0x57
#define D5NEXT_PUMP_OFFSET		0x6c
#define D5NEXT_FAN_OFFSET		0x5f
#define D5NEXT_5V_VOLTAGE		0x39
#define D5NEXT_12V_VOLTAGE		0x37
#define D5NEXT_VIRTUAL_SENSORS_START	0x3f
static u16 d5next_sensor_fan_offsets[] = { D5NEXT_PUMP_OFFSET, D5NEXT_FAN_OFFSET };

/* Control report offsets for the D5 Next pump */
#define D5NEXT_TEMP_CTRL_OFFSET		0x2D	/* Temperature sensor offsets location */
static u16 d5next_ctrl_fan_offsets[] = { 0x96, 0x41 };	/* Pump and fan speed (from 0-100%) */

/* Spec and sensor report offset for the Farbwerk RGB controller */
#define FARBWERK_NUM_SENSORS		4
#define FARBWERK_SENSOR_START		0x2f

/* Specs of the Farbwerk 360 RGB controller */
#define FARBWERK360_NUM_SENSORS			4
#define FARBWERK360_NUM_VIRTUAL_SENSORS		16
#define FARBWERK360_CTRL_REPORT_SIZE		0x682

/* Sensor report offsets for the Farbwerk 360 */
#define FARBWERK360_SENSOR_START		0x32
#define FARBWERK360_VIRTUAL_SENSORS_START	0x3a

/* Control report offsets for the Farbwerk 360 */
#define FARBWERK360_TEMP_CTRL_OFFSET		0x8

/* Specs of the Octo fan controller */
#define OCTO_NUM_FANS			8
#define OCTO_NUM_SENSORS		4
#define OCTO_NUM_VIRTUAL_SENSORS	16
#define OCTO_CTRL_REPORT_SIZE		0x65F

/* Sensor report offsets for the Octo */
#define OCTO_SENSOR_START		0x3D
#define OCTO_VIRTUAL_SENSORS_START	0x45
static u16 octo_sensor_fan_offsets[] = { 0x7D, 0x8A, 0x97, 0xA4, 0xB1, 0xBE, 0xCB, 0xD8 };

/* Control report offsets for the Octo */
#define OCTO_TEMP_CTRL_OFFSET		0xA
/* Fan speed offsets (0-100%) */
static u16 octo_ctrl_fan_offsets[] = { 0x5A, 0xAF, 0x104, 0x159, 0x1AE, 0x203, 0x258, 0x2AD };

/* Specs of Quadro fan controller */
#define QUADRO_NUM_FANS			4
#define QUADRO_NUM_SENSORS		4
#define QUADRO_NUM_VIRTUAL_SENSORS	16
#define QUADRO_NUM_FLOW_SENSORS		1
#define QUADRO_CTRL_REPORT_SIZE		0x3c1

/* Sensor report offsets for the Quadro */
#define QUADRO_SENSOR_START		0x34
#define QUADRO_VIRTUAL_SENSORS_START	0x3c
#define QUADRO_FLOW_SENSOR_OFFSET	0x6e
static u16 quadro_sensor_fan_offsets[] = { 0x70, 0x7D, 0x8A, 0x97 };

/* Control report offsets for the Quadro */
#define QUADRO_TEMP_CTRL_OFFSET		0xA
#define QUADRO_FLOW_PULSES_CTRL_OFFSET	0x6
/* Fan speed offsets (0-100%) */
static u16 quadro_ctrl_fan_offsets[] = { 0x36, 0x8b, 0xe0, 0x135 };

/* Specs of High Flow Next flow sensor */
#define HIGHFLOWNEXT_NUM_SENSORS	2

/* Sensor report offsets for the High Flow Next */
#define HIGHFLOWNEXT_SENSOR_START	85
#define HIGHFLOWNEXT_FLOW		81
#define HIGHFLOWNEXT_WATER_QUALITY	89
#define HIGHFLOWNEXT_POWER		91
#define HIGHFLOWNEXT_CONDUCTIVITY	95
#define HIGHFLOWNEXT_5V_VOLTAGE		97
#define HIGHFLOWNEXT_5V_VOLTAGE_USB	99

/* Specs of the Leakshield */
#define LEAKSHIELD_NUM_SENSORS				2
#define LEAKSHIELD_USB_REPORT_LENGTH			49
#define LEAKSHIELD_USB_REPORT_ENDPOINT			2

/* Sensor report offsets for Leakshield */
#define LEAKSHIELD_PRESSURE_ADJUSTED	285
#define LEAKSHIELD_TEMPERATURE_1	265
#define LEAKSHIELD_TEMPERATURE_2	287
#define LEAKSHIELD_PRESSURE_MIN		291
#define LEAKSHIELD_PRESSURE_TARGET	293
#define LEAKSHIELD_PRESSURE_MAX		295
#define LEAKSHIELD_PUMP_RPM_IN		101
#define LEAKSHIELD_FLOW_IN		111
#define LEAKSHIELD_RESERVOIR_VOLUME	313
#define LEAKSHIELD_RESERVOIR_FILLED	311

/* USB control report offsets and info for Leakshield */
#define LEAKSHIELD_USB_REPORT_PUMP_RPM_OFFSET		1
#define LEAKSHIELD_USB_REPORT_FLOW_RPM_UNIT_OFFSET	33
#define LEAKSHIELD_USB_REPORT_FLOW_OFFSET		3
#define LEAKSHIELD_USB_REPORT_FLOW_UNIT_OFFSET		34
#define LEAKSHIELD_USB_REPORT_UNIT_RPM			0x03
#define LEAKSHIELD_USB_REPORT_UNIT_DL_PER_H		0x0C

/* USB bulk message to report pump RPM and flow rate for pressure calculations */
static u8 leakshield_usb_report_template[] = {
	0x4, 0x7f, 0xff, 0x7f, 0xff, 0x7f, 0xff, 0x7f, 0xff, 0x7f, 0xff, 0x7f, 0xff, 0x7f, 0xff,
	0x7f, 0xff, 0x7f, 0xff, 0x7f, 0xff, 0x7f, 0xff, 0x7f, 0xff, 0x7f, 0xff, 0x7f, 0xff,
	0x7f, 0xff, 0x7f, 0xff, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0
};

/* Specs of the Aquastream XT pump */
#define AQUASTREAMXT_SERIAL_START		0x3a
#define AQUASTREAMXT_FIRMWARE_VERSION		0x32
#define AQUASTREAMXT_NUM_FANS			2
#define AQUASTREAMXT_NUM_SENSORS		3
#define AQUASTREAMXT_FAN_STOPPED		0x4
#define AQUASTREAMXT_PUMP_CONVERSION_CONST	45000000
#define AQUASTREAMXT_FAN_CONVERSION_CONST	5646000
#define AQUASTREAMXT_PUMP_MIN_RPM		3000
#define AQUASTREAMXT_PUMP_MAX_RPM		6000
#define AQUASTREAMXT_SENSOR_REPORT_SIZE		0x42
#define AQUASTREAMXT_CTRL_REPORT_SIZE		0x34

/* Sensor report offsets and info for Aquastream XT */
#define AQUASTREAMXT_SENSOR_START		0xd
#define AQUASTREAMXT_FAN_VOLTAGE_OFFSET		0x7
#define AQUASTREAMXT_FAN_STATUS_OFFSET		0x1d
#define AQUASTREAMXT_PUMP_VOLTAGE_OFFSET	0x9
#define AQUASTREAMXT_PUMP_CURR_OFFSET		0xb
static u16 aquastreamxt_sensor_fan_offsets[] = { 0x13, 0x1b };

/* Control report offsets for Aquastream XT */
#define AQUASTREAMXT_PUMP_MODE_CTRL_OFFSET	0x3
#define AQUASTREAMXT_PUMP_MODE_CTRL_MANUAL	0x14
#define AQUASTREAMXT_FAN_MODE_CTRL_OFFSET	0x1a
#define AQUASTREAMXT_FAN_MODE_CTRL_MANUAL	0x1
static u16 aquastreamxt_ctrl_fan_offsets[] = { 0x8, 0x1b };

/* Labels for D5 Next */
static const char *const label_d5next_temp[] = {
	"Coolant temp"
};

static const char *const label_d5next_speeds[] = {
	"Pump speed",
	"Fan speed"
};

static const char *const label_d5next_power[] = {
	"Pump power",
	"Fan power"
};

static const char *const label_d5next_voltages[] = {
	"Pump voltage",
	"Fan voltage",
	"+5V voltage",
	"+12V voltage"
};

static const char *const label_d5next_current[] = {
	"Pump current",
	"Fan current"
};

/* Labels for Aquaero, Farbwerk, Farbwerk 360, Octo and Quadro temperature sensors */
static const char *const label_temp_sensors[] = {
	"Sensor 1",
	"Sensor 2",
	"Sensor 3",
	"Sensor 4",
	"Sensor 5",
	"Sensor 6",
	"Sensor 7",
	"Sensor 8"
};

static const char *const label_virtual_temp_sensors[] = {
	"Virtual sensor 1",
	"Virtual sensor 2",
	"Virtual sensor 3",
	"Virtual sensor 4",
	"Virtual sensor 5",
	"Virtual sensor 6",
	"Virtual sensor 7",
	"Virtual sensor 8",
	"Virtual sensor 9",
	"Virtual sensor 10",
	"Virtual sensor 11",
	"Virtual sensor 12",
	"Virtual sensor 13",
	"Virtual sensor 14",
	"Virtual sensor 15",
	"Virtual sensor 16",
};

static const char *const label_aquaero_calc_temp_sensors[] = {
	"Calc. virtual sensor 1",
	"Calc. virtual sensor 2",
	"Calc. virtual sensor 3",
	"Calc. virtual sensor 4"
};

/* Labels for Octo and Quadro (except speed) */
static const char *const label_fan_speed[] = {
	"Fan 1 speed",
	"Fan 2 speed",
	"Fan 3 speed",
	"Fan 4 speed",
	"Fan 5 speed",
	"Fan 6 speed",
	"Fan 7 speed",
	"Fan 8 speed"
};

static const char *const label_fan_power[] = {
	"Fan 1 power",
	"Fan 2 power",
	"Fan 3 power",
	"Fan 4 power",
	"Fan 5 power",
	"Fan 6 power",
	"Fan 7 power",
	"Fan 8 power"
};

static const char *const label_fan_voltage[] = {
	"Fan 1 voltage",
	"Fan 2 voltage",
	"Fan 3 voltage",
	"Fan 4 voltage",
	"Fan 5 voltage",
	"Fan 6 voltage",
	"Fan 7 voltage",
	"Fan 8 voltage"
};

static const char *const label_fan_current[] = {
	"Fan 1 current",
	"Fan 2 current",
	"Fan 3 current",
	"Fan 4 current",
	"Fan 5 current",
	"Fan 6 current",
	"Fan 7 current",
	"Fan 8 current"
};

/* Labels for Quadro fan speeds */
static const char *const label_quadro_speeds[] = {
	"Fan 1 speed",
	"Fan 2 speed",
	"Fan 3 speed",
	"Fan 4 speed",
	"Flow speed [dL/h]"
};

/* Labels for Aquaero fan speeds */
static const char *const label_aquaero_speeds[] = {
	"Fan 1 speed",
	"Fan 2 speed",
	"Fan 3 speed",
	"Fan 4 speed",
	"Flow sensor 1 [dL/h]",
	"Flow sensor 2 [dL/h]"
};

/* Labels for High Flow Next */
static const char *const label_highflownext_temp_sensors[] = {
	"Coolant temp",
	"External sensor"
};

static const char *const label_highflownext_fan_speed[] = {
	"Flow [dL/h]",
	"Water quality [%]",
	"Conductivity [nS/cm]",
};

static const char *const label_highflownext_power[] = {
	"Dissipated power",
};

static const char *const label_highflownext_voltage[] = {
	"+5V voltage",
	"+5V USB voltage"
};

/* Labels for Leakshield */
static const char *const label_leakshield_temp_sensors[] = {
	"Temperature 1",
	"Temperature 2"
};

static const char *const label_leakshield_fan_speed[] = {
	"Pressure [µbar]",
	"User-Provided Pump Speed",
	"User-Provided Flow [dL/h]",
	"Reservoir Volume [ml]",
	"Reservoir Filled [ml]",
};

/* Labels for AquastreamXT */
static const char *const label_temp_sensors_aquastreamxt[] = {
	"Fan IC temp",
	"External sensor",
	"Coolant temp"
};

struct aqc_fan_structure_offsets {
	u8 voltage;
	u8 curr;
	u8 power;
	u8 speed;
};

/* Fan structure offsets for Aquaero */
static struct aqc_fan_structure_offsets aqc_aquaero_fan_structure = {
	.voltage = AQUAERO_FAN_VOLTAGE_OFFSET,
	.curr = AQUAERO_FAN_CURRENT_OFFSET,
	.power = AQUAERO_FAN_POWER_OFFSET,
	.speed = AQUAERO_FAN_SPEED_OFFSET
};

/* Fan structure offsets for all devices except Aquaero */
static struct aqc_fan_structure_offsets aqc_general_fan_structure = {
	.voltage = AQC_FAN_VOLTAGE_OFFSET,
	.curr = AQC_FAN_CURRENT_OFFSET,
	.power = AQC_FAN_POWER_OFFSET,
	.speed = AQC_FAN_SPEED_OFFSET
};

struct aqc_data {
	struct hid_device *hdev;
	struct device *hwmon_dev;
	struct dentry *debugfs;
	struct mutex mutex;	/* Used for locking access when reading and writing PWM values */
	enum kinds kind;
	const char *name;

	int ctrl_report_id;
	int secondary_ctrl_report_id;
	int secondary_ctrl_report_size;
	u8 *secondary_ctrl_report;

	int buffer_size;
	u8 *buffer;
	int checksum_start;
	int checksum_length;
	int checksum_offset;

	int num_fans;
	u16 *fan_sensor_offsets;
	u16 *fan_ctrl_offsets;
	int num_temp_sensors;
	int temp_sensor_start_offset;
	int num_virtual_temp_sensors;
	int virtual_temp_sensor_start_offset;
	int num_calc_virtual_temp_sensors;
	int calc_virtual_temp_sensor_start_offset;
	u16 temp_ctrl_offset;
	u16 power_cycle_count_offset;
	int num_flow_sensors;
	u8 flow_sensors_start_offset;
	u8 flow_pulses_ctrl_offset;
	struct aqc_fan_structure_offsets *fan_structure;

	u8 serial_number_start_offset;
	u8 firmware_version_offset;

	/* General info, available across all devices */
	u32 serial_number[2];
	u16 firmware_version;

	/* How many times the device was powered on */
	u32 power_cycles;

	/* Sensor values */
	s32 temp_input[20];	/* Max 4 physical and 16 virtual or 8 physical and 12 virtual */
	s32 speed_input[8];
	u32 speed_input_min[8];
	u32 speed_input_target[8];
	u32 speed_input_max[8];
	u32 power_input[8];
	u16 voltage_input[8];
	u16 current_input[8];

	/* Label values */
	const char *const *temp_label;
	const char *const *virtual_temp_label;
	const char *const *calc_virtual_temp_label;	/* For Aquaero */
	const char *const *speed_label;
	const char *const *power_label;
	const char *const *voltage_label;
	const char *const *current_label;

	unsigned long updated;
};

/* Converts from centi-percent */
static int aqc_percent_to_pwm(u16 val)
{
	return DIV_ROUND_CLOSEST(val * 255, 100 * 100);
}

/* Converts to centi-percent */
static int aqc_pwm_to_percent(long val)
{
	return DIV_ROUND_CLOSEST(val * 100 * 100, 255);
}

/* Converts rpm to pwm */
static int aquastream_rpm_to_pwm(long val)
{
	return DIV_ROUND_CLOSEST((val - AQUASTREAMXT_PUMP_MIN_RPM) * 255,
				 AQUASTREAMXT_PUMP_MAX_RPM - AQUASTREAMXT_PUMP_MIN_RPM);
}

/* Converts to rpm between 3000 and 6000, where the output is a multiple of 60 */
static int aquastream_pwm_to_rpm(long val)
{
	return DIV_ROUND_CLOSEST(val * 50, 255) * 60 + AQUASTREAMXT_PUMP_MIN_RPM;
}

/* Converts raw value for Aquastream XT pump speed to rpm */
static int aquastream_convert_pump_rpm(u16 val)
{
	if (val > 0)
		return DIV_ROUND_CLOSEST(AQUASTREAMXT_PUMP_CONVERSION_CONST, val);
	return 0;
}

/* Converts raw value for Aquastream XT fan speed to rpm */
static int aquastream_convert_fan_rpm(u16 val)
{
	if (val > 0)
		return DIV_ROUND_CLOSEST(AQUASTREAMXT_FAN_CONVERSION_CONST, val);
	return 0;
}

/* Expects the mutex to be locked */
static int aqc_get_ctrl_data(struct aqc_data *priv)
{
	int ret;

	memset(priv->buffer, 0x00, priv->buffer_size);
	ret = hid_hw_raw_request(priv->hdev, priv->ctrl_report_id, priv->buffer, priv->buffer_size,
				 HID_FEATURE_REPORT, HID_REQ_GET_REPORT);
	if (ret < 0)
		ret = -ENODATA;

	return ret;
}

/* Expects the mutex to be locked */
static int aqc_send_ctrl_data(struct aqc_data *priv)
{
	int ret;
	u16 checksum;

	/* Checksum is not needed for Aquaero and Aquastream XT */
	if (priv->kind != aquaero && priv->kind != aquastreamxt) {
		/* Init and xorout value for CRC-16/USB is 0xffff */
		checksum =
		    crc16(0xffff, priv->buffer + priv->checksum_start, priv->checksum_length);
		checksum ^= 0xffff;

		/* Place the new checksum at the end of the report */
		put_unaligned_be16(checksum, priv->buffer + priv->checksum_offset);
	}

	/* Send the patched up report back to the device */
	ret = hid_hw_raw_request(priv->hdev, priv->ctrl_report_id, priv->buffer, priv->buffer_size,
				 HID_FEATURE_REPORT, HID_REQ_SET_REPORT);
	if (ret < 0)
		return ret;

	/* The official software sends this report after every change, so do it here as well */
	ret =
	    hid_hw_raw_request(priv->hdev, priv->secondary_ctrl_report_id,
			       priv->secondary_ctrl_report, priv->secondary_ctrl_report_size,
			       HID_FEATURE_REPORT, HID_REQ_SET_REPORT);
	return ret;
}

/* Refreshes the control buffer and stores value at offset in val */
static int aqc_get_ctrl_val(struct aqc_data *priv, int offset, long *val, int type)
{
	int ret;

	mutex_lock(&priv->mutex);

	ret = aqc_get_ctrl_data(priv);
	if (ret < 0)
		goto unlock_and_return;

	switch (type) {
	case AQC_LE16:
		*val = (s16)get_unaligned_le16(priv->buffer + offset);
		break;
	case AQC_BE16:
		*val = (s16)get_unaligned_be16(priv->buffer + offset);
		break;
	case AQC_8:
		*val = priv->buffer[offset];
		break;
	default:
		ret = -EINVAL;
		break;
	}

unlock_and_return:
	mutex_unlock(&priv->mutex);
	return ret;
}

static int aqc_set_buffer_val(u8 *buffer, int offset, long val, int type)
{
	switch (type) {
	case AQC_LE16:
		put_unaligned_le16((u16)val, buffer + offset);
		return 0;
	case AQC_BE16:
		put_unaligned_be16((u16)val, buffer + offset);
		return 0;
	case AQC_8:
		buffer[offset] = (u8)val;
		return 0;
	default:
		return -EINVAL;
	}
}

/* Refreshes the control buffer, updates values at offsets and writes buffer to device */
static int aqc_set_ctrl_vals(struct aqc_data *priv, int *offsets, long *values, int *types, int len)
{
	int ret, i;

	mutex_lock(&priv->mutex);

	ret = aqc_get_ctrl_data(priv);
	if (ret < 0)
		goto unlock_and_return;

	for (i = 0; i < len; i++) {
		ret = aqc_set_buffer_val(priv->buffer, offsets[i], values[i], types[i]);
		if (ret < 0)
			goto unlock_and_return;
	}

	ret = aqc_send_ctrl_data(priv);

unlock_and_return:
	mutex_unlock(&priv->mutex);
	return ret;
}

/* Refreshes the control buffer, updates value at offset and writes buffer to device */
static int aqc_set_ctrl_val(struct aqc_data *priv, int offset, long val, int type)
{
	return aqc_set_ctrl_vals(priv, &offset, &val, &type, 1);
}

static umode_t aqc_is_visible(const void *data, enum hwmon_sensor_types type, u32 attr, int channel)
{
	const struct aqc_data *priv = data;

	switch (type) {
	case hwmon_temp:
		if (channel < priv->num_temp_sensors) {
			switch (attr) {
			case hwmon_temp_label:
			case hwmon_temp_input:
				return 0444;
			case hwmon_temp_offset:
				if (priv->temp_ctrl_offset != 0)
					return 0644;
			default:
				break;
			}
		}

		if (channel <
		    priv->num_temp_sensors + priv->num_virtual_temp_sensors +
		    priv->num_calc_virtual_temp_sensors)
			switch (attr) {
			case hwmon_temp_label:
			case hwmon_temp_input:
				return 0444;
			default:
				break;
			}
		break;
	case hwmon_pwm:
		if (priv->fan_ctrl_offsets && channel < priv->num_fans) {
			switch (priv->kind) {
			case aquaero:
				switch (attr) {
				case hwmon_pwm_input:
				case hwmon_pwm_mode:
					return 0644;
				default:
					break;
				}
				break;
			case aquastreamxt:
				switch (attr) {
				case hwmon_pwm_input:
					return 0644;
				default:
					break;
				}
				break;
			default:
				switch (attr) {
				case hwmon_pwm_enable:
				case hwmon_pwm_input:
				case hwmon_pwm_auto_channels_temp:
					return 0644;
				default:
					break;
				}
				break;
			}
		}
		break;
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_input:
		case hwmon_fan_label:
			switch (priv->kind) {
			case highflownext:
				/*
				 * Special case to support flow sensor, water quality
				 * and conductivity
				 */
				if (channel < 3)
					return 0444;
				break;
			case leakshield:
				/* Special case for user-provided Leakshield sensors */
				if (channel == 1 || channel == 2)
					return 0644;
				if (channel < 5)
					return 0444;
				break;
			case aquaero:
			case quadro:
				/* Special case to support flow sensors */
				if (channel < priv->num_fans + priv->num_flow_sensors)
					return 0444;
				break;
			default:
				if (channel < priv->num_fans)
					return 0444;
				break;
			}
			break;
		case hwmon_fan_min:
		case hwmon_fan_max:
			if (priv->kind == aquaero && channel < priv->num_fans)
				return 0644;
			fallthrough;
		case hwmon_fan_target:
			/* Special case for Leakshield pressure sensor */
			if (priv->kind == leakshield && channel == 0)
				return 0444;
			break;
		case hwmon_fan_pulses:
			/* Special case for flow sensor */
			if (priv->kind == quadro && channel == priv->num_fans)
				return 0644;
		default:
			break;
		}
		break;
	case hwmon_power:
		switch (priv->kind) {
		case highflownext:
			/* Special case to support one power sensor */
			if (channel == 0)
				return 0444;
			break;
		case aquastreamxt:
			break;
		default:
			if (channel < priv->num_fans)
				return 0444;
			break;
		}
		break;
	case hwmon_curr:
		switch (priv->kind) {
		case aquastreamxt:
			/* Current only reported for pump */
			if (channel == 0)
				return 0444;
			break;
		default:
			if (channel < priv->num_fans)
				return 0444;
			break;
		}
		break;
	case hwmon_in:
		switch (priv->kind) {
		case d5next:
			/* Special case to support +5V and +12V voltage sensors */
			if (channel < priv->num_fans + 2)
				return 0444;
			break;
		case highflownext:
			/* Special case to support two voltage sensors */
			if (channel < 2)
				return 0444;
			break;
		default:
			if (channel < priv->num_fans)
				return 0444;
			break;
		}
		break;
	default:
		break;
	}

	return 0;
}

static int aqc_legacy_read(struct aqc_data *priv)
{
	int ret, i, sensor_value;

	mutex_lock(&priv->mutex);

	memset(priv->buffer, 0x00, priv->buffer_size);
	ret = hid_hw_raw_request(priv->hdev, AQC_LEGACY_STATUS_REPORT_ID, priv->buffer,
				 priv->buffer_size, HID_FEATURE_REPORT, HID_REQ_GET_REPORT);
	if (ret < 0)
		goto unlock_and_return;

	/* Info provided with every report */
	priv->serial_number[0] = get_unaligned_le16(priv->buffer +
						    priv->serial_number_start_offset);
	priv->firmware_version = get_unaligned_le16(priv->buffer + priv->firmware_version_offset);

	/* Temperature sensor readings */
	for (i = 0; i < priv->num_temp_sensors; i++) {
		sensor_value = get_unaligned_le16(priv->buffer + priv->temp_sensor_start_offset +
						  i * AQC_SENSOR_SIZE);
		priv->temp_input[i] = sensor_value * 10;
	}

	sensor_value = get_unaligned_le16(priv->buffer + priv->fan_sensor_offsets[0]);
	priv->speed_input[0] = aquastream_convert_pump_rpm(sensor_value);

	sensor_value = get_unaligned_le16(priv->buffer + AQUASTREAMXT_FAN_STATUS_OFFSET);
	if (sensor_value == AQUASTREAMXT_FAN_STOPPED) {
		priv->speed_input[1] = 0;
	} else {
		sensor_value = get_unaligned_le16(priv->buffer + priv->fan_sensor_offsets[1]);
		priv->speed_input[1] = aquastream_convert_fan_rpm(sensor_value);
	}

	/* Calculation derived from linear regression */
	sensor_value = get_unaligned_le16(priv->buffer + AQUASTREAMXT_PUMP_CURR_OFFSET);
	priv->current_input[0] = DIV_ROUND_CLOSEST(sensor_value * 176, 100) - 52;

	sensor_value = get_unaligned_le16(priv->buffer + AQUASTREAMXT_PUMP_VOLTAGE_OFFSET);
	priv->voltage_input[0] = DIV_ROUND_CLOSEST(sensor_value * 1000, 61);

	sensor_value = get_unaligned_le16(priv->buffer + AQUASTREAMXT_FAN_VOLTAGE_OFFSET);
	priv->voltage_input[1] = DIV_ROUND_CLOSEST(sensor_value * 1000, 63);

unlock_and_return:
	mutex_unlock(&priv->mutex);
	return ret;
}

static int aqc_read(struct device *dev, enum hwmon_sensor_types type, u32 attr,
		    int channel, long *val)
{
	int ret;
	struct aqc_data *priv = dev_get_drvdata(dev);

	if (time_after(jiffies, priv->updated + STATUS_UPDATE_INTERVAL)) {
		if (priv->kind == aquastreamxt) {
			aqc_legacy_read(priv);
			priv->updated = jiffies;
		} else {
			return -ENODATA;
		}
	}

	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
			if (priv->temp_input[channel] == -ENODATA)
				return -ENODATA;

			*val = priv->temp_input[channel];
			break;
		case hwmon_temp_offset:
			ret =
			    aqc_get_ctrl_val(priv,
					     priv->temp_ctrl_offset +
					     channel * AQC_SENSOR_SIZE, val, AQC_BE16);
			if (ret < 0)
				return ret;
			*val *= 10;
		default:
			break;
		}
		break;
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_input:
			if (priv->speed_input[channel] == -ENODATA)
				return -ENODATA;

			*val = priv->speed_input[channel];
			break;
		case hwmon_fan_min:
			if (priv->kind == aquaero) {
				ret =
				    aqc_get_ctrl_val(priv,
						     priv->fan_ctrl_offsets[channel] +
						     AQUAERO_FAN_CTRL_MIN_RPM_OFFSET,
						     val, AQC_BE16);
				if (ret < 0)
					return ret;
				break;
			}

			*val = priv->speed_input_min[channel];
			break;
		case hwmon_fan_max:
			if (priv->kind == aquaero) {
				ret =
				    aqc_get_ctrl_val(priv,
						     priv->fan_ctrl_offsets[channel] +
						     AQUAERO_FAN_CTRL_MAX_RPM_OFFSET,
						     val, AQC_BE16);
				if (ret < 0)
					return ret;
				break;
			}

			*val = priv->speed_input_max[channel];
			break;
		case hwmon_fan_target:
			*val = priv->speed_input_target[channel];
			break;
		case hwmon_fan_pulses:
			ret = aqc_get_ctrl_val(priv, priv->flow_pulses_ctrl_offset, val, AQC_BE16);
			if (ret < 0)
				return ret;
			break;
		default:
			return -EOPNOTSUPP;
		}
		break;
	case hwmon_power:
		*val = priv->power_input[channel];
		break;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_enable:
			ret = aqc_get_ctrl_val(priv, priv->fan_ctrl_offsets[channel], val, AQC_8);
			if (ret < 0)
				return ret;

			/* Incrementing to satisfy hwmon rules */
			*val = *val + 1;
			break;
		case hwmon_pwm_input:
			switch (priv->kind) {
			case aquaero:
				ret =
				    aqc_get_ctrl_val(priv,
						     AQUAERO_CTRL_PRESET_START +
						     channel * AQUAERO_CTRL_PRESET_SIZE,
						     val, AQC_BE16);
				if (ret < 0)
					return ret;
				*val = aqc_percent_to_pwm(*val);
				break;
			case aquastreamxt:
				if (channel == 0) {
					ret =
					    aqc_get_ctrl_val(priv, priv->fan_ctrl_offsets[channel],
							     val, AQC_LE16);
					if (ret < 0)
						return ret;
					*val = aquastream_convert_pump_rpm(*val);
					*val = aquastream_rpm_to_pwm(*val);
				} else {
					ret =
					    aqc_get_ctrl_val(priv, priv->fan_ctrl_offsets[channel],
							     val, AQC_8);
					if (ret < 0)
						return ret;
				}
				break;
			default:
				ret =
				    aqc_get_ctrl_val(priv,
						     AQUAERO_CTRL_PRESET_START +
						     channel * AQUAERO_CTRL_PRESET_SIZE,
						     val, AQC_BE16);
				if (ret < 0)
					return ret;
				*val = aqc_percent_to_pwm(*val);
				break;
			}
			break;
		case hwmon_pwm_auto_channels_temp:
			ret =
			    aqc_get_ctrl_val(priv,
					     priv->fan_ctrl_offsets[channel] +
					     AQC_FAN_CTRL_TEMP_SELECT_OFFSET, val, AQC_BE16);
			if (ret < 0)
				return ret;

			*val = 1 << *val;
			break;
		case hwmon_pwm_mode:
			ret = aqc_get_ctrl_val(priv,
					       priv->fan_ctrl_offsets[channel] +
					       AQUAERO_FAN_CTRL_MODE_OFFSET, val, AQC_8);
			if (ret < 0)
				return ret;

			switch (*val) {
			case 0:	/* DC mode */
				break;
			case 2:	/* PWM mode */
				*val = 1;
				break;
			default:
				break;
			}
			break;
		default:
			return -EOPNOTSUPP;
		}
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

static int aqc_read_string(struct device *dev, enum hwmon_sensor_types type, u32 attr,
			   int channel, const char **str)
{
	struct aqc_data *priv = dev_get_drvdata(dev);

	/* Number of sensors that are not calculated */
	int num_non_calc_sensors = priv->num_temp_sensors + priv->num_virtual_temp_sensors;

	switch (type) {
	case hwmon_temp:
		if (channel < priv->num_temp_sensors) {
			*str = priv->temp_label[channel];
		} else {
			if (priv->kind == aquaero && channel >= num_non_calc_sensors)
				*str =
				    priv->calc_virtual_temp_label[channel - num_non_calc_sensors];
			else
				*str = priv->virtual_temp_label[channel - priv->num_temp_sensors];
		}

		break;
	case hwmon_fan:
		*str = priv->speed_label[channel];
		break;
	case hwmon_power:
		*str = priv->power_label[channel];
		break;
	case hwmon_in:
		*str = priv->voltage_label[channel];
		break;
	case hwmon_curr:
		*str = priv->current_label[channel];
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int aqc_leakshield_send_report(struct aqc_data *priv, int channel, long val)
{
	struct usb_interface *intf;
	struct usb_device *usb_dev;
	unsigned int pipe;
	int actual_length;
	int ret;
	u16 checksum;
	u16 val16;

	if (priv->kind != leakshield)
		return -EOPNOTSUPP;

	/* Forbid out-of-bounds values */
	if (val < -1 || val >= AQC_SENSOR_NA)
		return -EINVAL;

	if (val == -1)
		/*
		 * Map -1 to N/A value. The device will still remember the
		 * old value for 5 minutes
		 */
		val16 = AQC_SENSOR_NA;
	else
		val16 = (u16)val;

	/*
	 * leakshield_usb_report_template is loaded into priv->buffer during initialization.
	 * Modify only the requested value (pump RPM or flow) without resetting the other one
	 */
	switch (channel) {
	case 1:
		priv->buffer[LEAKSHIELD_USB_REPORT_FLOW_RPM_UNIT_OFFSET] =
		    (val16 == AQC_SENSOR_NA) ? 0 : LEAKSHIELD_USB_REPORT_UNIT_RPM;
		put_unaligned_be16(val16, priv->buffer + LEAKSHIELD_USB_REPORT_PUMP_RPM_OFFSET);
		break;
	case 2:
		priv->buffer[LEAKSHIELD_USB_REPORT_FLOW_UNIT_OFFSET] =
		    (val16 == AQC_SENSOR_NA) ? 0 : LEAKSHIELD_USB_REPORT_UNIT_DL_PER_H;
		put_unaligned_be16(val16, priv->buffer + LEAKSHIELD_USB_REPORT_FLOW_OFFSET);
		break;
	default:
		return -EINVAL;
	}

	/* Init and xorout value for CRC-16/USB is 0xffff */
	checksum = crc16(0xffff, priv->buffer, LEAKSHIELD_USB_REPORT_LENGTH);
	checksum ^= 0xffff;

	/* Place the new checksum at the end of the report */
	put_unaligned_be16(checksum, priv->buffer + LEAKSHIELD_USB_REPORT_LENGTH);

	intf = to_usb_interface(priv->hdev->dev.parent);
	usb_dev = interface_to_usbdev(intf);
	pipe = usb_sndbulkpipe(usb_dev, LEAKSHIELD_USB_REPORT_ENDPOINT);
	ret = usb_bulk_msg(usb_dev, pipe, priv->buffer, priv->buffer_size, &actual_length, 1000);

	if (actual_length != priv->buffer_size)
		return -EIO;

	return ret;
}

static int aqc_write(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel,
		     long val)
{
	int ret, pwm_value, temp_sensor;
	long ctrl_mode;
	/* Arrays for setting multiple values at once in the control report */
	int ctrl_values_offsets[3];
	long ctrl_values[3];
	int ctrl_values_types[3];
	struct aqc_data *priv = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_offset:
			/* Limit temp offset to +/- 15K as in the official software */
			val = clamp_val(val, -15000, 15000) / 10;
			ret =
			    aqc_set_ctrl_val(priv,
					     priv->temp_ctrl_offset +
					     channel * AQC_SENSOR_SIZE, val, AQC_BE16);
			if (ret < 0)
				return ret;
			break;
		default:
			return -EOPNOTSUPP;
		}
		break;
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_min:
			val = clamp_val(val, 0, 15000);
			ret = aqc_set_ctrl_val(priv,
					       priv->fan_ctrl_offsets[channel] +
					       AQUAERO_FAN_CTRL_MIN_RPM_OFFSET, val, AQC_BE16);
			if (ret < 0)
				return ret;
			break;
		case hwmon_fan_max:
			val = clamp_val(val, 0, 15000);
			ret = aqc_set_ctrl_val(priv,
					       priv->fan_ctrl_offsets[channel] +
					       AQUAERO_FAN_CTRL_MAX_RPM_OFFSET, val, AQC_BE16);
			if (ret < 0)
				return ret;
			break;
		case hwmon_fan_input:
			return aqc_leakshield_send_report(priv, channel, val);
		case hwmon_fan_pulses:
			val = clamp_val(val, 10, 1000);
			ret = aqc_set_ctrl_val(priv, priv->flow_pulses_ctrl_offset, val, AQC_BE16);
			if (ret < 0)
				return ret;
			break;
		default:
			return -EOPNOTSUPP;
		}
		break;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_enable:
			switch (priv->kind) {
			case d5next:
				if (val < 0 || val > 3)
					return -EINVAL;
				break;
			case octo:
			case quadro:
				if (val < 0 || val > priv->num_fans + 3)
					return -EINVAL;

				/* Fan can't follow itself */
				if (val == channel + 4)
					return -EINVAL;

				/*
				 * Check if fan we want to follow is following another one
				 * currently. This is disallowed in the official software
				 */
				if (val > 3) {
					ret =
					    aqc_get_ctrl_val(priv, priv->fan_ctrl_offsets[val - 4],
							     &ctrl_mode, AQC_8);
					if (ret < 0)
						return ret;

					/* The fan is indeed following another one */
					if (ctrl_mode > 2)
						return -EINVAL;
				}
				break;
			default:
				return -EOPNOTSUPP;
			}

			/* Decrement to convert from hwmon to aqc */
			ret = aqc_set_ctrl_val(priv,
					       priv->fan_ctrl_offsets[channel], val - 1, AQC_8);
			if (ret < 0)
				return ret;
			break;
		case hwmon_pwm_input:
			if (val < 0 || val > 255)
				return -EINVAL;

			switch (priv->kind) {
			case aquaero:
				pwm_value = aqc_pwm_to_percent(val);
				/* Write pwm value to preset corresponding to the channel */
				ctrl_values_offsets[0] = AQUAERO_CTRL_PRESET_START +
				    channel * AQUAERO_CTRL_PRESET_SIZE;
				ctrl_values[0] = pwm_value;
				ctrl_values_types[0] = AQC_BE16;

				/* Write preset number in fan control source */
				ctrl_values_offsets[1] = priv->fan_ctrl_offsets[channel] +
				    AQUAERO_FAN_CTRL_SRC_OFFSET;
				ctrl_values[1] = AQUAERO_CTRL_PRESET_ID + channel;
				ctrl_values_types[1] = AQC_BE16;

				/* Set minimum power to 0 to allow the fan to turn off */
				ctrl_values_offsets[2] = priv->fan_ctrl_offsets[channel] +
				    AQUAERO_FAN_CTRL_MIN_PWR_OFFSET;
				ctrl_values[2] = 0;
				ctrl_values_types[2] = AQC_BE16;

				ret = aqc_set_ctrl_vals(priv, ctrl_values_offsets, ctrl_values,
							ctrl_values_types, 3);
				if (ret < 0)
					return ret;
				break;
			case aquastreamxt:
				if (channel == 0) {
					pwm_value = aquastream_pwm_to_rpm(val);
					pwm_value = aquastream_convert_pump_rpm(pwm_value);
					ctrl_values_offsets[0] = priv->fan_ctrl_offsets[channel];
					ctrl_values[0] = pwm_value;
					ctrl_values_types[0] = AQC_LE16;

					/* Enable manual speed control */
					ctrl_values_offsets[1] = AQUASTREAMXT_PUMP_MODE_CTRL_OFFSET;
					ctrl_values[1] = AQUASTREAMXT_PUMP_MODE_CTRL_MANUAL;
					ctrl_values_types[1] = AQC_8;
				} else {
					ctrl_values_offsets[0] = priv->fan_ctrl_offsets[channel];
					ctrl_values[0] = val;
					ctrl_values_types[0] = AQC_8;

					/* Enable manual speed control */
					ctrl_values_offsets[1] = AQUASTREAMXT_FAN_MODE_CTRL_OFFSET;
					ctrl_values[1] = AQUASTREAMXT_FAN_MODE_CTRL_MANUAL;
					ctrl_values_types[1] = AQC_8;
				}
				ret = aqc_set_ctrl_vals(priv, ctrl_values_offsets, ctrl_values,
							ctrl_values_types, 2);
				if (ret < 0)
					return ret;
				break;
			default:
				pwm_value = aqc_pwm_to_percent(val);
				ret =
				    aqc_set_ctrl_val(priv,
						     priv->fan_ctrl_offsets[channel] +
						     AQC_FAN_CTRL_PWM_OFFSET, pwm_value, AQC_BE16);
				if (ret < 0)
					return ret;
				break;
			}
			break;
		case hwmon_pwm_auto_channels_temp:
			switch (val) {
			case 1:
				temp_sensor = 0;
				break;
			case 2:
				temp_sensor = 1;
				break;
			case 4:
				temp_sensor = 2;
				break;
			case 8:
				temp_sensor = 3;
				break;
			default:
				return -EINVAL;
			}

			if (temp_sensor >= priv->num_temp_sensors)
				return -EINVAL;

			ret =
			    aqc_set_ctrl_val(priv,
					     priv->fan_ctrl_offsets[channel] +
					     AQC_FAN_CTRL_TEMP_SELECT_OFFSET, temp_sensor,
					     AQC_BE16);
			if (ret < 0)
				return ret;
			break;
		case hwmon_pwm_mode:
			switch (val) {
			case 0:	/* DC mode */
				ctrl_mode = 0;
				break;
			case 1:	/* PWM mode */
				ctrl_mode = 2;
				break;
			default:
				return -EINVAL;
			}
			ret = aqc_set_ctrl_val(priv,
					       priv->fan_ctrl_offsets[channel] +
					       AQUAERO_FAN_CTRL_MODE_OFFSET, ctrl_mode, AQC_8);
			if (ret < 0)
				return ret;
		default:
			break;
		}
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static const struct hwmon_ops aqc_hwmon_ops = {
	.is_visible = aqc_is_visible,
	.read = aqc_read,
	.read_string = aqc_read_string,
	.write = aqc_write
};

static const struct hwmon_channel_info *aqc_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_OFFSET,
			   HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_OFFSET,
			   HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_OFFSET,
			   HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_OFFSET,
			   HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_OFFSET,
			   HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_OFFSET,
			   HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_OFFSET,
			   HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_OFFSET,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_MIN | HWMON_F_MAX |
			   HWMON_F_TARGET,
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_MIN | HWMON_F_MAX,
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_MIN | HWMON_F_MAX,
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_MIN | HWMON_F_MAX,
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_PULSES,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(power,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_AUTO_CHANNELS_TEMP |
			   HWMON_PWM_MODE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_AUTO_CHANNELS_TEMP |
			   HWMON_PWM_MODE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_AUTO_CHANNELS_TEMP |
			   HWMON_PWM_MODE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_AUTO_CHANNELS_TEMP |
			   HWMON_PWM_MODE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_AUTO_CHANNELS_TEMP,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_AUTO_CHANNELS_TEMP,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_AUTO_CHANNELS_TEMP,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_AUTO_CHANNELS_TEMP),
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL),
	HWMON_CHANNEL_INFO(curr,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL),
	NULL
};

static const struct hwmon_chip_info aqc_chip_info = {
	.ops = &aqc_hwmon_ops,
	.info = aqc_info,
};

static int aqc_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data, int size)
{
	int i, j;
	s16 sensor_value;
	struct aqc_data *priv;

	if (report->id != STATUS_REPORT_ID)
		return 0;

	priv = hid_get_drvdata(hdev);

	/* Info provided with every report */
	priv->serial_number[0] = get_unaligned_be16(data + priv->serial_number_start_offset);
	priv->serial_number[1] =
	    get_unaligned_be16(data + priv->serial_number_start_offset + SERIAL_PART_OFFSET);
	priv->firmware_version = get_unaligned_be16(data + priv->firmware_version_offset);

	/* Normal temperature sensor readings */
	for (i = 0; i < priv->num_temp_sensors; i++) {
		sensor_value = get_unaligned_be16(data +
						  priv->temp_sensor_start_offset +
						  i * AQC_SENSOR_SIZE);
		if (sensor_value == AQC_SENSOR_NA)
			priv->temp_input[i] = -ENODATA;
		else
			priv->temp_input[i] = sensor_value * 10;
	}

	/* Virtual temperature sensor readings */
	for (j = 0; j < priv->num_virtual_temp_sensors; j++) {
		sensor_value = get_unaligned_be16(data +
						  priv->virtual_temp_sensor_start_offset +
						  j * AQC_SENSOR_SIZE);
		if (sensor_value == AQC_SENSOR_NA)
			priv->temp_input[i] = -ENODATA;
		else
			priv->temp_input[i] = sensor_value * 10;
		i++;
	}

	/* Fan speed and related readings */
	for (i = 0; i < priv->num_fans; i++) {
		priv->speed_input[i] =
		    get_unaligned_be16(data + priv->fan_sensor_offsets[i] +
				       priv->fan_structure->speed);
		priv->power_input[i] =
		    get_unaligned_be16(data + priv->fan_sensor_offsets[i] +
				       priv->fan_structure->power) * 10000;
		priv->voltage_input[i] =
		    get_unaligned_be16(data + priv->fan_sensor_offsets[i] +
				       priv->fan_structure->voltage) * 10;
		priv->current_input[i] =
		    get_unaligned_be16(data + priv->fan_sensor_offsets[i] +
				       priv->fan_structure->curr);
	}

	/* Flow sensor readings */
	for (j = 0; j < priv->num_flow_sensors; j++) {
		priv->speed_input[i] = get_unaligned_be16(data + priv->flow_sensors_start_offset +
							  j * AQC_SENSOR_SIZE);
		i++;
	}

	if (priv->power_cycle_count_offset != 0)
		priv->power_cycles = get_unaligned_be32(data + priv->power_cycle_count_offset);

	/* Special-case sensor readings */
	switch (priv->kind) {
	case aquaero:
		/* Read calculated virtual temp sensors */
		i = priv->num_temp_sensors + priv->num_virtual_temp_sensors;
		for (j = 0; j < priv->num_calc_virtual_temp_sensors; j++) {
			sensor_value = get_unaligned_be16(data +
					priv->calc_virtual_temp_sensor_start_offset +
					j * AQC_SENSOR_SIZE);
			if (sensor_value == AQC_SENSOR_NA)
				priv->temp_input[i] = -ENODATA;
			else
				priv->temp_input[i] = sensor_value * 10;
			i++;
		}
		break;
	case d5next:
		priv->voltage_input[2] = get_unaligned_be16(data + D5NEXT_5V_VOLTAGE) * 10;
		priv->voltage_input[3] = get_unaligned_be16(data + D5NEXT_12V_VOLTAGE) * 10;
		break;
	case highflownext:
		/* If external temp sensor is not connected, its power reading is also N/A */
		if (priv->temp_input[1] == -ENODATA)
			priv->power_input[0] = -ENODATA;
		else
			priv->power_input[0] =
			    get_unaligned_be16(data + HIGHFLOWNEXT_POWER) * 1000000;

		priv->voltage_input[0] = get_unaligned_be16(data + HIGHFLOWNEXT_5V_VOLTAGE) * 10;
		priv->voltage_input[1] =
		    get_unaligned_be16(data + HIGHFLOWNEXT_5V_VOLTAGE_USB) * 10;

		priv->speed_input[0] = get_unaligned_be16(data + HIGHFLOWNEXT_FLOW);
		priv->speed_input[1] = get_unaligned_be16(data + HIGHFLOWNEXT_WATER_QUALITY);
		priv->speed_input[2] = get_unaligned_be16(data + HIGHFLOWNEXT_CONDUCTIVITY);
		break;
	case leakshield:
		priv->speed_input[0] =
		    ((s16)get_unaligned_be16(data + LEAKSHIELD_PRESSURE_ADJUSTED)) * 100;
		priv->speed_input_min[0] = get_unaligned_be16(data + LEAKSHIELD_PRESSURE_MIN) * 100;
		priv->speed_input_target[0] =
		    get_unaligned_be16(data + LEAKSHIELD_PRESSURE_TARGET) * 100;
		priv->speed_input_max[0] = get_unaligned_be16(data + LEAKSHIELD_PRESSURE_MAX) * 100;

		priv->speed_input[1] = get_unaligned_be16(data + LEAKSHIELD_PUMP_RPM_IN);
		if (priv->speed_input[1] == AQC_SENSOR_NA)
			priv->speed_input[1] = -ENODATA;

		priv->speed_input[2] = get_unaligned_be16(data + LEAKSHIELD_FLOW_IN);
		if (priv->speed_input[2] == AQC_SENSOR_NA)
			priv->speed_input[2] = -ENODATA;

		priv->speed_input[3] = get_unaligned_be16(data + LEAKSHIELD_RESERVOIR_VOLUME);
		priv->speed_input[4] = get_unaligned_be16(data + LEAKSHIELD_RESERVOIR_FILLED);

		/* Second temp sensor is not positioned after the first one, read it here */
		priv->temp_input[1] = get_unaligned_be16(data + LEAKSHIELD_TEMPERATURE_2) * 10;
		break;
	default:
		break;
	}

	priv->updated = jiffies;

	return 0;
}

#ifdef CONFIG_DEBUG_FS

static int serial_number_show(struct seq_file *seqf, void *unused)
{
	struct aqc_data *priv = seqf->private;

	seq_printf(seqf, "%05u-%05u\n", priv->serial_number[0], priv->serial_number[1]);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(serial_number);

static int firmware_version_show(struct seq_file *seqf, void *unused)
{
	struct aqc_data *priv = seqf->private;

	seq_printf(seqf, "%u\n", priv->firmware_version);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(firmware_version);

static int power_cycles_show(struct seq_file *seqf, void *unused)
{
	struct aqc_data *priv = seqf->private;

	seq_printf(seqf, "%u\n", priv->power_cycles);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(power_cycles);

static void aqc_debugfs_init(struct aqc_data *priv)
{
	char name[64];

	scnprintf(name, sizeof(name), "%s_%s-%s", "aquacomputer", priv->name,
		  dev_name(&priv->hdev->dev));

	priv->debugfs = debugfs_create_dir(name, NULL);
	debugfs_create_file("serial_number", 0444, priv->debugfs, priv, &serial_number_fops);
	debugfs_create_file("firmware_version", 0444, priv->debugfs, priv, &firmware_version_fops);

	if (priv->power_cycle_count_offset != 0)
		debugfs_create_file("power_cycles", 0444, priv->debugfs, priv, &power_cycles_fops);
}

#else

static void aqc_debugfs_init(struct aqc_data *priv)
{
}

#endif

static int aqc_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct aqc_data *priv;
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

	switch (hdev->product) {
	case USB_PRODUCT_ID_AQUAERO:
		/*
		 * Aquaero presents itself as three HID devices under the same product ID:
		 * "aquaero keyboard/mouse", "aquaero System Control" and "aquaero Device",
		 * which is the one we want to communicate with. Unlike most other Aquacomputer
		 * devices, Aquaero does not return meaningful data when explicitly requested
		 * using GET_FEATURE_REPORT.
		 *
		 * The difference between "aquaero Device" and the other two is in the collections
		 * they present. The two other devices have the type of the second element in
		 * their respective collections set to 1, while the real device has it set to 0.
		 */
		if (hdev->collection[1].type != 0) {
			ret = -ENODEV;
			goto fail_and_close;
		}

		priv->kind = aquaero;

		priv->num_fans = AQUAERO_NUM_FANS;
		priv->fan_sensor_offsets = aquaero_sensor_fan_offsets;
		priv->fan_ctrl_offsets = aquaero_ctrl_fan_offsets;

		priv->num_temp_sensors = AQUAERO_NUM_SENSORS;
		priv->temp_sensor_start_offset = AQUAERO_SENSOR_START;
		priv->num_virtual_temp_sensors = AQUAERO_NUM_VIRTUAL_SENSORS;
		priv->virtual_temp_sensor_start_offset = AQUAERO_VIRTUAL_SENSOR_START;
		priv->num_calc_virtual_temp_sensors = AQUAERO_NUM_CALC_VIRTUAL_SENSORS;
		priv->calc_virtual_temp_sensor_start_offset = AQUAERO_CALC_VIRTUAL_SENSOR_START;

		priv->buffer_size = AQUAERO_CTRL_REPORT_SIZE;
		priv->temp_ctrl_offset = AQUAERO_TEMP_CTRL_OFFSET;
		priv->num_flow_sensors = AQUAERO_NUM_FLOW_SENSORS;
		priv->flow_sensors_start_offset = AQUAERO_FLOW_SENSORS_START;

		priv->temp_label = label_temp_sensors;
		priv->virtual_temp_label = label_virtual_temp_sensors;
		priv->calc_virtual_temp_label = label_aquaero_calc_temp_sensors;
		priv->speed_label = label_aquaero_speeds;
		priv->power_label = label_fan_power;
		priv->voltage_label = label_fan_voltage;
		priv->current_label = label_fan_current;
		break;
	case USB_PRODUCT_ID_D5NEXT:
		priv->kind = d5next;

		priv->num_fans = D5NEXT_NUM_FANS;
		priv->fan_sensor_offsets = d5next_sensor_fan_offsets;
		priv->fan_ctrl_offsets = d5next_ctrl_fan_offsets;

		priv->num_temp_sensors = D5NEXT_NUM_SENSORS;
		priv->temp_sensor_start_offset = D5NEXT_COOLANT_TEMP;
		priv->num_virtual_temp_sensors = D5NEXT_NUM_VIRTUAL_SENSORS;
		priv->virtual_temp_sensor_start_offset = D5NEXT_VIRTUAL_SENSORS_START;

		priv->power_cycle_count_offset = AQC_POWER_CYCLES;
		priv->buffer_size = D5NEXT_CTRL_REPORT_SIZE;
		priv->temp_ctrl_offset = D5NEXT_TEMP_CTRL_OFFSET;

		priv->temp_label = label_d5next_temp;
		priv->virtual_temp_label = label_virtual_temp_sensors;
		priv->speed_label = label_d5next_speeds;
		priv->power_label = label_d5next_power;
		priv->voltage_label = label_d5next_voltages;
		priv->current_label = label_d5next_current;
		break;
	case USB_PRODUCT_ID_FARBWERK:
		priv->kind = farbwerk;

		priv->num_fans = 0;

		priv->num_temp_sensors = FARBWERK_NUM_SENSORS;
		priv->temp_sensor_start_offset = FARBWERK_SENSOR_START;

		priv->temp_ctrl_offset = 0;

		priv->temp_label = label_temp_sensors;
		break;
	case USB_PRODUCT_ID_FARBWERK360:
		priv->kind = farbwerk360;

		priv->num_fans = 0;

		priv->num_temp_sensors = FARBWERK360_NUM_SENSORS;
		priv->temp_sensor_start_offset = FARBWERK360_SENSOR_START;
		priv->num_virtual_temp_sensors = FARBWERK360_NUM_VIRTUAL_SENSORS;
		priv->virtual_temp_sensor_start_offset = FARBWERK360_VIRTUAL_SENSORS_START;

		priv->buffer_size = FARBWERK360_CTRL_REPORT_SIZE;
		priv->temp_ctrl_offset = FARBWERK360_TEMP_CTRL_OFFSET;

		priv->temp_label = label_temp_sensors;
		priv->virtual_temp_label = label_virtual_temp_sensors;
		break;
	case USB_PRODUCT_ID_OCTO:
		priv->kind = octo;

		priv->num_fans = OCTO_NUM_FANS;
		priv->fan_sensor_offsets = octo_sensor_fan_offsets;
		priv->fan_ctrl_offsets = octo_ctrl_fan_offsets;

		priv->num_temp_sensors = OCTO_NUM_SENSORS;
		priv->temp_sensor_start_offset = OCTO_SENSOR_START;
		priv->num_virtual_temp_sensors = OCTO_NUM_VIRTUAL_SENSORS;
		priv->virtual_temp_sensor_start_offset = OCTO_VIRTUAL_SENSORS_START;

		priv->power_cycle_count_offset = AQC_POWER_CYCLES;
		priv->buffer_size = OCTO_CTRL_REPORT_SIZE;
		priv->temp_ctrl_offset = OCTO_TEMP_CTRL_OFFSET;

		priv->temp_label = label_temp_sensors;
		priv->virtual_temp_label = label_virtual_temp_sensors;
		priv->speed_label = label_fan_speed;
		priv->power_label = label_fan_power;
		priv->voltage_label = label_fan_voltage;
		priv->current_label = label_fan_current;
		break;
	case USB_PRODUCT_ID_QUADRO:
		priv->kind = quadro;

		priv->num_fans = QUADRO_NUM_FANS;
		priv->fan_sensor_offsets = quadro_sensor_fan_offsets;
		priv->fan_ctrl_offsets = quadro_ctrl_fan_offsets;

		priv->num_temp_sensors = QUADRO_NUM_SENSORS;
		priv->temp_sensor_start_offset = QUADRO_SENSOR_START;
		priv->num_virtual_temp_sensors = QUADRO_NUM_VIRTUAL_SENSORS;
		priv->virtual_temp_sensor_start_offset = QUADRO_VIRTUAL_SENSORS_START;

		priv->power_cycle_count_offset = AQC_POWER_CYCLES;
		priv->buffer_size = QUADRO_CTRL_REPORT_SIZE;
		priv->num_flow_sensors = QUADRO_NUM_FLOW_SENSORS;
		priv->flow_sensors_start_offset = QUADRO_FLOW_SENSOR_OFFSET;
		priv->temp_ctrl_offset = QUADRO_TEMP_CTRL_OFFSET;
		priv->flow_pulses_ctrl_offset = QUADRO_FLOW_PULSES_CTRL_OFFSET;

		priv->temp_label = label_temp_sensors;
		priv->virtual_temp_label = label_virtual_temp_sensors;
		priv->speed_label = label_quadro_speeds;
		priv->power_label = label_fan_power;
		priv->voltage_label = label_fan_voltage;
		priv->current_label = label_fan_current;
		break;
	case USB_PRODUCT_ID_HIGHFLOWNEXT:
		priv->kind = highflownext;

		priv->num_fans = 0;
		priv->num_temp_sensors = HIGHFLOWNEXT_NUM_SENSORS;
		priv->temp_sensor_start_offset = HIGHFLOWNEXT_SENSOR_START;
		priv->power_cycle_count_offset = AQC_POWER_CYCLES;

		priv->temp_label = label_highflownext_temp_sensors;
		priv->speed_label = label_highflownext_fan_speed;
		priv->power_label = label_highflownext_power;
		priv->voltage_label = label_highflownext_voltage;
		break;
	case USB_PRODUCT_ID_LEAKSHIELD:
		/*
		 * Choose the right Leakshield device, because the other one acts
		 * as a keyboard
		 */
		if (hdev->type != 2) {
			ret = -ENODEV;
			goto fail_and_close;
		}

		priv->kind = leakshield;

		priv->num_fans = 0;
		priv->num_temp_sensors = LEAKSHIELD_NUM_SENSORS;
		priv->temp_sensor_start_offset = LEAKSHIELD_TEMPERATURE_1;

		/* Plus two bytes for checksum */
		priv->buffer_size = LEAKSHIELD_USB_REPORT_LENGTH + 2;

		priv->temp_label = label_leakshield_temp_sensors;
		priv->speed_label = label_leakshield_fan_speed;
		break;
	case USB_PRODUCT_ID_AQUASTREAMXT:
		priv->kind = aquastreamxt;

		priv->num_fans = AQUASTREAMXT_NUM_FANS;
		priv->fan_sensor_offsets = aquastreamxt_sensor_fan_offsets;
		priv->fan_ctrl_offsets = aquastreamxt_ctrl_fan_offsets;

		priv->num_temp_sensors = AQUASTREAMXT_NUM_SENSORS;
		priv->temp_sensor_start_offset = AQUASTREAMXT_SENSOR_START;
		priv->buffer_size = AQUASTREAMXT_SENSOR_REPORT_SIZE;

		priv->temp_label = label_temp_sensors_aquastreamxt;
		priv->speed_label = label_d5next_speeds;
		priv->power_label = label_d5next_power;
		priv->voltage_label = label_d5next_voltages;
		priv->current_label = label_d5next_current;
		break;
	default:
		break;
	}

	if (priv->kind == aquaero) {
		priv->serial_number_start_offset = AQUAERO_SERIAL_START;
		priv->firmware_version_offset = AQUAERO_FIRMWARE_VERSION;

		priv->fan_structure = &aqc_aquaero_fan_structure;

		priv->ctrl_report_id = AQUAERO_CTRL_REPORT_ID;
		priv->secondary_ctrl_report_id = AQUAERO_SECONDARY_CTRL_REPORT_ID;
		priv->secondary_ctrl_report_size = AQUAERO_SECONDARY_CTRL_REPORT_SIZE;
		priv->secondary_ctrl_report = aquaero_secondary_ctrl_report;
	} else if (priv->kind == aquastreamxt) {
		priv->serial_number_start_offset = AQUASTREAMXT_SERIAL_START;
		priv->firmware_version_offset = AQUASTREAMXT_FIRMWARE_VERSION;

		priv->ctrl_report_id = AQC_LEGACY_CTRL_REPORT_ID;
		priv->secondary_ctrl_report_id = AQUASTREAMXT_SECONDARY_CTRL_REPORT_ID;
		priv->secondary_ctrl_report_size = AQUASTREAMXT_SECONDARY_CTRL_REPORT_SIZE;
		priv->secondary_ctrl_report = aquastreamxt_secondary_ctrl_report;
	} else {
		priv->serial_number_start_offset = AQC_SERIAL_START;
		priv->firmware_version_offset = AQC_FIRMWARE_VERSION;

		priv->fan_structure = &aqc_general_fan_structure;

		priv->ctrl_report_id = CTRL_REPORT_ID;
		priv->secondary_ctrl_report_id = SECONDARY_CTRL_REPORT_ID;
		priv->secondary_ctrl_report_size = SECONDARY_CTRL_REPORT_SIZE;
		priv->secondary_ctrl_report = secondary_ctrl_report;
	}

	if (priv->buffer_size != 0) {
		priv->checksum_start = 0x01;
		priv->checksum_length = priv->buffer_size - 3;
		priv->checksum_offset = priv->buffer_size - 2;
	}

	priv->name = aqc_device_names[priv->kind];

	priv->buffer = devm_kzalloc(&hdev->dev, priv->buffer_size, GFP_KERNEL);
	if (!priv->buffer) {
		ret = -ENOMEM;
		goto fail_and_close;
	}

	if (priv->kind == leakshield)
		memcpy(priv->buffer, leakshield_usb_report_template, LEAKSHIELD_USB_REPORT_LENGTH);

	mutex_init(&priv->mutex);

	priv->hwmon_dev = hwmon_device_register_with_info(&hdev->dev, priv->name, priv,
							  &aqc_chip_info, NULL);

	if (IS_ERR(priv->hwmon_dev)) {
		ret = (int)PTR_ERR(priv->hwmon_dev);
		goto fail_and_close;
	}

	aqc_debugfs_init(priv);

	return 0;

fail_and_close:
	hid_hw_close(hdev);
fail_and_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void aqc_remove(struct hid_device *hdev)
{
	struct aqc_data *priv = hid_get_drvdata(hdev);

	debugfs_remove_recursive(priv->debugfs);
	hwmon_device_unregister(priv->hwmon_dev);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id aqc_table[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_AQUACOMPUTER, USB_PRODUCT_ID_AQUAERO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_AQUACOMPUTER, USB_PRODUCT_ID_D5NEXT) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_AQUACOMPUTER, USB_PRODUCT_ID_FARBWERK) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_AQUACOMPUTER, USB_PRODUCT_ID_FARBWERK360) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_AQUACOMPUTER, USB_PRODUCT_ID_OCTO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_AQUACOMPUTER, USB_PRODUCT_ID_QUADRO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_AQUACOMPUTER, USB_PRODUCT_ID_HIGHFLOWNEXT) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_AQUACOMPUTER, USB_PRODUCT_ID_LEAKSHIELD) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_AQUACOMPUTER, USB_PRODUCT_ID_AQUASTREAMXT) },
	{ }
};

MODULE_DEVICE_TABLE(hid, aqc_table);

static struct hid_driver aqc_driver = {
	.name = DRIVER_NAME,
	.id_table = aqc_table,
	.probe = aqc_probe,
	.remove = aqc_remove,
	.raw_event = aqc_raw_event,
};

static int __init aqc_init(void)
{
	return hid_register_driver(&aqc_driver);
}

static void __exit aqc_exit(void)
{
	hid_unregister_driver(&aqc_driver);
}

/* Request to initialize after the HID bus to ensure it's not being loaded before */
late_initcall(aqc_init);
module_exit(aqc_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Aleksa Savic <savicaleksa83@gmail.com>");
MODULE_AUTHOR("Jack Doan <me@jackdoan.com>");
MODULE_DESCRIPTION("Hwmon driver for Aquacomputer devices");
