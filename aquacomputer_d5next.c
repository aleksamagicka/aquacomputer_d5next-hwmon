// SPDX-License-Identifier: GPL-2.0+
/*
 * hwmon driver for Aquacomputer devices (D5 Next, Farbwerk, Farbwerk 360, Octo,
 * Quadro, High Flow Next, Aquaero, Leakshield, Aquastream XT, Aquastream Ultimate,
 * Poweradjust 3, High Flow USB)
 *
 * Aquacomputer devices send HID reports (with ID 0x01) every second to report
 * sensor values, except for devices that communicate through the
 * legacy way (currently, Aquastream XT and Poweradjust 3).
 *
 * Copyright 2021 Aleksa Savic <savicaleksa83@gmail.com>
 * Copyright 2022 Jack Doan <me@jackdoan.com>
 */

#include <linux/crc16.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
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
#define USB_PRODUCT_ID_AQUASTREAMULT	0xf00b
#define USB_PRODUCT_ID_POWERADJUST3	0xf0bd
#define USB_PRODUCT_ID_HIGHFLOW		0xf003

enum kinds {
	aquaero, d5next, farbwerk, farbwerk360, octo,
	quadro, highflownext, leakshield, aquastreamxt,
	aquastreamult, poweradjust3, highflow
};

enum aquaero_hw_kinds { unknown, aquaero5, aquaero6 };

DECLARE_COMPLETION(aquaero_sensor_report_received);

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
	[aquastreamult] = "aquastreamultimate",
	[poweradjust3] = "poweradjust3",
	[highflow] = "highflow"
};

#define DRIVER_NAME			"aquacomputer_d5next"

#define STATUS_REPORT_ID		0x01
#define STATUS_UPDATE_INTERVAL		(2 * HZ)	/* In seconds */
#define SERIAL_PART_OFFSET		2

#define CTRL_REPORT_ID			0x03
#define AQUAERO_CTRL_REPORT_ID		0x0b

#define CTRL_REPORT_DELAY		200	/* ms */

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

#define FAN_CURVE_HOLD_MIN_POWER_BIT_POS	1
#define FAN_CURVE_START_BOOST_BIT_POS		2

/* Report IDs for legacy devices */
#define AQUASTREAMXT_STATUS_REPORT_ID	0x04
#define AQUASTREAMXT_CTRL_REPORT_ID	0x06

#define POWERADJUST3_STATUS_REPORT_ID	0x03

#define HIGHFLOW_STATUS_REPORT_ID	0x02

/* Info, sensor sizes and offsets for most Aquacomputer devices */
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
#define AQC_FAN_CTRL_CURVE_NUM_POINTS	16

/* Report offsets for fan control */
#define AQC_FAN_CTRL_PWM_OFFSET		0x01
#define AQC_FAN_CTRL_TEMP_SELECT_OFFSET	0x03
#define AQC_FAN_CTRL_TEMP_CURVE_START	0x15
#define AQC_FAN_CTRL_PWM_CURVE_START	0x35

/* Specs of the Aquaero fan controllers */
#define AQUAERO_SERIAL_START			0x07
#define AQUAERO_FIRMWARE_VERSION		0x0B
#define AQUAERO_HARDWARE_VERSION		0x0F
#define AQUAERO_NUM_FANS			4
#define AQUAERO_NUM_SENSORS			8
#define AQUAERO_NUM_AQUABUS_SENSORS		20
#define AQUAERO_NUM_VIRTUAL_SENSORS		8
#define AQUAERO_NUM_CALC_VIRTUAL_SENSORS	4
#define AQUAERO_NUM_FLOW_SENSORS		2
#define AQUAERO_NUM_AQUABUS_FLOW_SENSORS	12
#define AQUAERO_CTRL_REPORT_SIZE		0xa93
#define AQUAERO_CTRL_PRESET_ID			0x5c
#define AQUAERO_CTRL_PRESET_SIZE		0x02
#define AQUAERO_CTRL_PRESET_START		0x55c
#define AQUAERO_5_HW_VERSION			5600
#define AQUAERO_6_HW_VERSION			6000

/* Sensor report offsets for Aquaero fan controllers */
#define AQUAERO_SENSOR_START			0x65
#define AQUAERO_VIRTUAL_SENSOR_START		0x85
#define AQUAERO_CALC_VIRTUAL_SENSOR_START	0x95
#define AQUAERO_AQUABUS_SENSOR_START		0x9D
#define AQUAERO_FLOW_SENSORS_START		0xF9
#define AQUAERO_AQUABUS_FLOW_SENSORS_START	0xFD
#define AQUAERO_FAN_VOLTAGE_OFFSET		0x04
#define AQUAERO_FAN_CURRENT_OFFSET		0x06
#define AQUAERO_FAN_POWER_OFFSET		0x08
#define AQUAERO_FAN_SPEED_OFFSET		0x00
static u16 aquaero_sensor_fan_offsets[] = { 0x167, 0x173, 0x17f, 0x18B };
#define AQUAERO_CURRENT_UPTIME_OFFSET		0x11
#define AQUAERO_TOTAL_UPTIME_OFFSET		0x15

/* Control report offsets for the Aquaero fan controllers */
#define AQUAERO_TEMP_CTRL_OFFSET	0xdb
#define AQUAERO_FAN_CTRL_MIN_RPM_OFFSET	0x00
#define AQUAERO_FAN_CTRL_MAX_RPM_OFFSET	0x02
#define AQUAERO_FAN_CTRL_MIN_PWR_OFFSET	0x04
#define AQUAERO_FAN_CTRL_MAX_PWR_OFFSET	0x06
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
/* Fan curve "hold min power" and "start boost" offsets, only for the fan, first value is unused */
static u8 d5next_ctrl_fan_curve_hold_start_offsets[] = { 0x00, 0x2F };
/* Fan curve min power */
static u8 d5next_ctrl_fan_curve_min_power_offsets[] = { 0x39, 0x30 };
/* Fan curve max power */
static u8 d5next_ctrl_fan_curve_max_power_offsets[] = { 0x3B, 0x32 };
/* Fan curve fallback power */
static u8 d5next_ctrl_fan_curve_fallback_power_offsets[] = { 0x3D, 0x34 };

/* Specs of the Aquastream Ultimate pump */
/* Pump does not follow the standard structure, so only consider the fan */
#define AQUASTREAMULT_NUM_FANS		1
#define AQUASTREAMULT_NUM_SENSORS	2

/* Sensor report offsets for the Aquastream Ultimate pump */
#define AQUASTREAMULT_SENSOR_START		0x2D
#define AQUASTREAMULT_PUMP_OFFSET		0x51
#define AQUASTREAMULT_PUMP_VOLTAGE		0x3D
#define AQUASTREAMULT_PUMP_CURRENT		0x53
#define AQUASTREAMULT_PUMP_POWER		0x55
#define AQUASTREAMULT_FAN_OFFSET		0x41
#define AQUASTREAMULT_PRESSURE_OFFSET		0x57
#define AQUASTREAMULT_FLOW_SENSOR_OFFSET	0x37
#define AQUASTREAMULT_FAN_VOLTAGE_OFFSET	0x02
#define AQUASTREAMULT_FAN_CURRENT_OFFSET	0x00
#define AQUASTREAMULT_FAN_POWER_OFFSET		0x04
#define AQUASTREAMULT_FAN_SPEED_OFFSET		0x06
static u16 aquastreamult_sensor_fan_offsets[] = { AQUASTREAMULT_FAN_OFFSET };

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

/* Fan curve "hold min power" and "start boost" offsets */
static u8 octo_ctrl_fan_curve_hold_start_offsets[] = {
	0x12, 0x1B, 0x24, 0x2D, 0x36, 0x3F, 0x48, 0x51
};

static u8 octo_ctrl_fan_curve_min_power_offsets[] = {
	0x13, 0x1C, 0x25, 0x2E, 0x37, 0x40, 0x49, 0x52
};

static u8 octo_ctrl_fan_curve_max_power_offsets[] = {
	0x15, 0x1E, 0x27, 0x30, 0x39, 0x42, 0x4B, 0x54
};

static u8 octo_ctrl_fan_curve_fallback_power_offsets[] = {
	0x17, 0x20, 0x29, 0x32, 0x3B, 0x44, 0x4D, 0x56
};

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
/* Fan curve "hold min power" and "start boost" offsets */
static u8 quadro_ctrl_fan_curve_hold_start_offsets[] = { 0x12, 0x1B, 0x24, 0x2D };
static u8 quadro_ctrl_fan_curve_min_power_offsets[] = { 0x13, 0x1C, 0x25, 0x2E };
static u8 quadro_ctrl_fan_curve_max_power_offsets[] = { 0x15, 0x1E, 0x27, 0x30 };
static u8 quadro_ctrl_fan_curve_fallback_power_offsets[] = { 0x17, 0x20, 0x29, 0x32 };

/* Specs of High Flow Next flow sensor */
#define HIGHFLOWNEXT_NUM_SENSORS	2
#define HIGHFLOWNEXT_NUM_FLOW_SENSORS	1

/* Sensor report offsets for the High Flow Next */
#define HIGHFLOWNEXT_SENSOR_START	85
#define HIGHFLOWNEXT_FLOW		81
#define HIGHFLOWNEXT_WATER_QUALITY	89
#define HIGHFLOWNEXT_POWER		91
#define HIGHFLOWNEXT_CONDUCTIVITY	95
#define HIGHFLOWNEXT_5V_VOLTAGE		97
#define HIGHFLOWNEXT_5V_VOLTAGE_USB	99

/* Specs of the Leakshield */
#define LEAKSHIELD_NUM_SENSORS		2
#define LEAKSHIELD_USB_REPORT_LENGTH	49
#define LEAKSHIELD_USB_REPORT_ENDPOINT	2

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

/* Specs of the Poweradjust 3 */
#define POWERADJUST3_NUM_SENSORS	1
#define POWERADJUST3_SENSOR_REPORT_SIZE	0x32

/* Sensor report offsets for the Poweradjust 3 */
#define POWERADJUST3_SENSOR_START	0x03

/* Specs of the High Flow USB */
#define HIGHFLOW_NUM_SENSORS		2
#define HIGHFLOW_SENSOR_REPORT_SIZE	0x76

/* Sensor report offsets for the High Flow USB */
#define HIGHFLOW_FIRMWARE_VERSION	0x3
#define HIGHFLOW_SERIAL_START		0x9
#define HIGHFLOW_SENSOR_START		0x2b

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

static const char *const label_aquaero_aquabus_temp_sensors[] = {
	"Aquabus sensor 1",
	"Aquabus sensor 2",
	"Aquabus sensor 3",
	"Aquabus sensor 4",
	"Aquabus sensor 5",
	"Aquabus sensor 6",
	"Aquabus sensor 7",
	"Aquabus sensor 8",
	"Aquabus sensor 9",
	"Aquabus sensor 10",
	"Aquabus sensor 11",
	"Aquabus sensor 12",
	"Aquabus sensor 13",
	"Aquabus sensor 14",
	"Aquabus sensor 15",
	"Aquabus sensor 16",
	"Aquabus sensor 17",
	"Aquabus sensor 18",
	"Aquabus sensor 19",
	"Aquabus sensor 20"
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
	"Flow sensor 2 [dL/h]",
	"Aquabus flow 1 [dL/h]",
	"Aquabus flow 2 [dL/h]",
	"Aquabus flow 3 [dL/h]",
	"Aquabus flow 4 [dL/h]",
	"Aquabus flow 5 [dL/h]",
	"Aquabus flow 6 [dL/h]",
	"Aquabus flow 7 [dL/h]",
	"Aquabus flow 8 [dL/h]",
	"Aquabus flow 9 [dL/h]",
	"Aquabus flow 10 [dL/h]",
	"Aquabus flow 11 [dL/h]",
	"Aquabus flow 12 [dL/h]"
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
	"Pressure [ubar]",
	"User-Provided Pump Speed",
	"User-Provided Flow [dL/h]",
	"Reservoir Volume [ml]",
	"Reservoir Filled [ml]",
};

/* Labels for Aquastream XT */
static const char *const label_aquastreamxt_temp_sensors[] = {
	"Fan IC temp",
	"External sensor",
	"Coolant temp"
};

/* Labels for Aquastream Ultimate */
static const char *const label_aquastreamult_temp[] = {
	"Coolant temp",
	"External temp"
};

static const char *const label_aquastreamult_speeds[] = {
	"Fan speed",
	"Pump speed",
	"Pressure [mbar]",
	"Flow speed [dL/h]"
};

static const char *const label_aquastreamult_power[] = {
	"Fan power",
	"Pump power"
};

static const char *const label_aquastreamult_voltages[] = {
	"Fan voltage",
	"Pump voltage"
};

static const char *const label_aquastreamult_current[] = {
	"Fan current",
	"Pump current"
};

/* Labels for Poweradjust 3 */
static const char *const label_poweradjust3_temp_sensors[] = {
	"External sensor"
};

/* Labels for Highflow */
static const char *const label_highflow_temp[] = {
	"External temp",
	"Internal temp"
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

/* Fan structure offsets for Aquastream Ultimate */
static struct aqc_fan_structure_offsets aqc_aquastreamult_fan_structure = {
	.voltage = AQUASTREAMULT_FAN_VOLTAGE_OFFSET,
	.curr = AQUASTREAMULT_FAN_CURRENT_OFFSET,
	.power = AQUASTREAMULT_FAN_POWER_OFFSET,
	.speed = AQUASTREAMULT_FAN_SPEED_OFFSET
};

/* Fan structure offsets for all devices except those above */
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
	const struct attribute_group *groups[8];	/* For max 8 fans */

	int status_report_id;	/* Used for legacy devices, report is stored in buffer */
	int ctrl_report_id;
	int secondary_ctrl_report_id;
	int secondary_ctrl_report_size;
	u8 *secondary_ctrl_report;

	ktime_t last_ctrl_report_op;
	int ctrl_report_delay;	/* Delay between two ctrl report operations, in ms */

	int buffer_size;
	/*
	 * Used for writing reports (where supported) and reading
	 * sensor reports on legacy devices
	 */
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
	int num_calc_virt_temp_sensors;
	int calc_virt_temp_sensor_start_offset;
	int num_aquabus_temp_sensors;
	int aquabus_temp_sensor_start_offset;
	u16 temp_ctrl_offset;
	u16 power_cycle_count_offset;
	int num_flow_sensors;
	u8 flow_sensors_start_offset;
	int num_aquabus_flow_sensors;
	u8 aquabus_flow_sensors_start_offset;
	u8 flow_pulses_ctrl_offset;
	struct aqc_fan_structure_offsets *fan_structure;
	u8 *fan_curve_min_power_offsets;
	u8 *fan_curve_max_power_offsets;
	/* Used for both "hold min power" and "start boost" parameters */
	u8 *fan_curve_hold_start_offsets;
	u8 *fan_curve_fallback_power_offsets;

	/* For differentiating between Aquaero 5 and 6 */
	enum aquaero_hw_kinds aquaero_hw_kind;
	int aquaero_hw_version;

	/* General info, available across all devices */
	u8 serial_number_start_offset;
	u32 serial_number[2];
	u8 firmware_version_offset;
	u16 firmware_version;

	/* How many times the device was powered on */
	u32 power_cycles;

	/* Aquaero only, in seconds */
	u32 current_uptime;
	u32 total_uptime;

	/*
	 * Sensor values. temp_input has a maximum of 4 physical + 16 virtual + 20 aquabus,
	 * or 8 physical + 12 virtual + 20 aquabus sensors, depending on the device
	 */
	s32 temp_input[40];
	s32 speed_input[20];	/* Max 8 physical + 12 aquabus */
	u32 speed_input_min[20];
	u32 speed_input_target[1];
	u32 speed_input_max[20];
	u32 power_input[8];
	u16 voltage_input[8];
	u16 current_input[8];

	/* Label values */
	const char *const *temp_label;
	const char *const *virtual_temp_label;
	const char *const *calc_virtual_temp_label;	/* For Aquaero */
	const char *const *aquabus_temp_label;		/* For Aquaero */
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

/* Gets bit value on given position in an int */
static int aqc_get_bit_at_pos(int val, int pos)
{
	return (val >> pos) & 1;
}

/* Sets bit value on given position in an int */
static int aqc_set_bit_at_pos(int val, int pos, int bit_value)
{
	return (val & ~(1 << pos)) | (bit_value << pos);
}

/* Converts RPM to Aquastream XT PWM representation */
static int aqc_aquastreamxt_rpm_to_pwm(long val)
{
	return DIV_ROUND_CLOSEST((val - AQUASTREAMXT_PUMP_MIN_RPM) * 255,
				 AQUASTREAMXT_PUMP_MAX_RPM - AQUASTREAMXT_PUMP_MIN_RPM);
}

/* Converts to RPM between 3000 and 6000, where the output is a multiple of 60 */
static int aqc_aquastreamxt_pwm_to_rpm(long val)
{
	return DIV_ROUND_CLOSEST(val * 50, 255) * 60 + AQUASTREAMXT_PUMP_MIN_RPM;
}

/* Converts raw value for Aquastream XT pump speed to RPM */
static int aqc_aquastreamxt_convert_pump_rpm(u16 val)
{
	if (val > 0)
		return DIV_ROUND_CLOSEST(AQUASTREAMXT_PUMP_CONVERSION_CONST, val);
	return 0;
}

/* Converts raw value for Aquastream XT fan speed to RPM */
static int aqc_aquastreamxt_convert_fan_rpm(u16 val)
{
	if (val > 0)
		return DIV_ROUND_CLOSEST(AQUASTREAMXT_FAN_CONVERSION_CONST, val);
	return 0;
}

static void aqc_delay_ctrl_report(struct aqc_data *priv)
{
	/*
	 * If previous read or write is too close to this one, delay the current operation
	 * to give the device enough time to process the previous one.
	 */
	if (priv->ctrl_report_delay) {
		s64 delta = ktime_ms_delta(ktime_get(), priv->last_ctrl_report_op);

		if (delta < priv->ctrl_report_delay)
			msleep(priv->ctrl_report_delay - delta);
	}
}

/* Expects the mutex to be locked */
static int aqc_get_ctrl_data(struct aqc_data *priv)
{
	int ret;

	aqc_delay_ctrl_report(priv);

	memset(priv->buffer, 0x00, priv->buffer_size);
	ret = hid_hw_raw_request(priv->hdev, priv->ctrl_report_id, priv->buffer, priv->buffer_size,
				 HID_FEATURE_REPORT, HID_REQ_GET_REPORT);
	if (ret < 0)
		ret = -ENODATA;

	priv->last_ctrl_report_op = ktime_get();

	return ret;
}

/* Expects the mutex to be locked */
static int aqc_send_ctrl_data(struct aqc_data *priv)
{
	int ret;
	u16 checksum;

	aqc_delay_ctrl_report(priv);

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
		goto record_access_and_ret;

	/* The official software sends this report after every change, so do it here as well */
	ret =
	    hid_hw_raw_request(priv->hdev, priv->secondary_ctrl_report_id,
			       priv->secondary_ctrl_report, priv->secondary_ctrl_report_size,
			       HID_FEATURE_REPORT, HID_REQ_SET_REPORT);
record_access_and_ret:
	priv->last_ctrl_report_op = ktime_get();

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
		    priv->num_calc_virt_temp_sensors + priv->num_aquabus_temp_sensors)
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
					return 0644;
				case hwmon_pwm_mode:
					/*
					 * Wait until the first Aquaero sensor report is received,
					 * to be able to differentiate between Aquaero 5 and 6
					 * by looking at the hardware version. While the v6 supports
					 * both DC and PWM mode for all four fans, v5 supports PWM
					 * mode only for the fourth fan.
					 */
					if (!wait_for_completion_timeout
					    (&aquaero_sensor_report_received,
					     STATUS_UPDATE_INTERVAL))
						return 0;

					if ((priv->aquaero_hw_kind == aquaero5 && channel == 3) ||
					    priv->aquaero_hw_kind == aquaero6)
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
			case d5next:
			case octo:
			case quadro:
				switch (attr) {
				case hwmon_pwm_enable:
					return 0644;
				}
				fallthrough;
			default:
				switch (attr) {
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
			case aquastreamult:
				/*
				 * Special case to support pump RPM, fan RPM,
				 * pressure and flow sensor
				 */
				if (channel < 4)
					return 0444;
				break;
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
				if (channel < priv->num_fans +
				    priv->num_flow_sensors +
				    priv->num_aquabus_flow_sensors)
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
		case aquastreamult:
			/* Special case to support pump and fan power */
			if (channel < 2)
				return 0444;
			break;
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
		case aquastreamult:
			/* Special case to support pump and fan current */
			if (channel < 2)
				return 0444;
			break;
		case aquastreamxt:
			/* Special case to support pump current */
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
		case aquastreamult:
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

/* Read device sensors by manually requesting the sensor report (legacy way) */
static int aqc_legacy_read(struct aqc_data *priv)
{
	int ret, i, sensor_value;

	mutex_lock(&priv->mutex);

	memset(priv->buffer, 0x00, priv->buffer_size);
	ret = hid_hw_raw_request(priv->hdev, priv->status_report_id, priv->buffer,
				 priv->buffer_size, HID_FEATURE_REPORT, HID_REQ_GET_REPORT);
	if (ret < 0)
		goto unlock_and_return;

	/* Temperature sensor readings */
	for (i = 0; i < priv->num_temp_sensors; i++) {
		sensor_value = get_unaligned_le16(priv->buffer + priv->temp_sensor_start_offset +
						  i * AQC_SENSOR_SIZE);
		if (sensor_value == AQC_SENSOR_NA)
			priv->temp_input[i] = -ENODATA;
		else
			priv->temp_input[i] = sensor_value * 10;
	}

	/* Special-case sensor readings */
	switch (priv->kind) {
	case aquastreamxt:
		/* Info provided with every report */
		priv->serial_number[0] = get_unaligned_le16(priv->buffer +
							    priv->serial_number_start_offset);
		priv->firmware_version =
		    get_unaligned_le16(priv->buffer + priv->firmware_version_offset);

		/* Read pump speed in RPM */
		sensor_value = get_unaligned_le16(priv->buffer + priv->fan_sensor_offsets[0]);
		priv->speed_input[0] = aqc_aquastreamxt_convert_pump_rpm(sensor_value);

		/* Read fan speed in RPM, if available */
		sensor_value = get_unaligned_le16(priv->buffer + AQUASTREAMXT_FAN_STATUS_OFFSET);
		if (sensor_value == AQUASTREAMXT_FAN_STOPPED) {
			priv->speed_input[1] = 0;
		} else {
			sensor_value =
			    get_unaligned_le16(priv->buffer + priv->fan_sensor_offsets[1]);
			priv->speed_input[1] = aqc_aquastreamxt_convert_fan_rpm(sensor_value);
		}

		/* Calculation derived from linear regression */
		sensor_value = get_unaligned_le16(priv->buffer + AQUASTREAMXT_PUMP_CURR_OFFSET);
		priv->current_input[0] = DIV_ROUND_CLOSEST(sensor_value * 176, 100) - 52;

		sensor_value = get_unaligned_le16(priv->buffer + AQUASTREAMXT_PUMP_VOLTAGE_OFFSET);
		priv->voltage_input[0] = DIV_ROUND_CLOSEST(sensor_value * 1000, 61);

		sensor_value = get_unaligned_le16(priv->buffer + AQUASTREAMXT_FAN_VOLTAGE_OFFSET);
		priv->voltage_input[1] = DIV_ROUND_CLOSEST(sensor_value * 1000, 63);
		break;
	case highflow:
		/* Info provided with every report */
		priv->serial_number[0] = get_unaligned_le16(priv->buffer +
							    priv->serial_number_start_offset);
		priv->firmware_version =
		    get_unaligned_le16(priv->buffer + priv->firmware_version_offset);
		break;
	default:
		break;
	}

	priv->updated = jiffies;

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
		if (priv->status_report_id != 0) {
			/* Legacy devices require manual reads */
			ret = aqc_legacy_read(priv);
			if (ret < 0)
				return -ENODATA;
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
					*val = aqc_aquastreamxt_convert_pump_rpm(*val);
					*val = aqc_aquastreamxt_rpm_to_pwm(*val);
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
						     priv->fan_ctrl_offsets[channel] +
						     AQC_FAN_CTRL_PWM_OFFSET, val, AQC_BE16);
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

	/* Number of sensors that are native */
	int num_native_sensors = priv->num_calc_virt_temp_sensors + num_non_calc_sensors;

	switch (type) {
	case hwmon_temp:
		if (channel < priv->num_temp_sensors) {
			*str = priv->temp_label[channel];
		} else {
			if (priv->kind == aquaero && channel >= num_native_sensors)
				*str =
				    priv->aquabus_temp_label[channel - num_native_sensors];
			else if (priv->kind == aquaero && channel >= num_non_calc_sensors)
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
	int ctrl_values_offsets[4];
	long ctrl_values[4];
	int ctrl_values_types[4];
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

			if (val == 0) {
				/* Set the fan to 100% as we don't control it anymore */
				ret =
				    aqc_set_ctrl_val(priv,
						     priv->fan_ctrl_offsets[channel] +
						     AQC_FAN_CTRL_PWM_OFFSET,
						     aqc_pwm_to_percent(255), AQC_BE16);
				if (ret < 0)
					return ret;
			} else {
				/* Decrement to convert from hwmon to aqc */
				val--;
			}

			ret = aqc_set_ctrl_val(priv, priv->fan_ctrl_offsets[channel], val, AQC_8);
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

				/*
				 * Set maximum power to 100% to allow the fan to
				 * reach maximum speed
				 */
				ctrl_values_offsets[3] = priv->fan_ctrl_offsets[channel] +
				    AQUAERO_FAN_CTRL_MAX_PWR_OFFSET;
				ctrl_values[3] = aqc_pwm_to_percent(255);
				ctrl_values_types[3] = AQC_BE16;

				ret = aqc_set_ctrl_vals(priv, ctrl_values_offsets, ctrl_values,
							ctrl_values_types, 4);
				if (ret < 0)
					return ret;
				break;
			case aquastreamxt:
				if (channel == 0) {
					pwm_value = aqc_aquastreamxt_pwm_to_rpm(val);
					pwm_value = aqc_aquastreamxt_convert_pump_rpm(pwm_value);
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
			break;
		default:
			break;
		}
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

/* Curve templates and atribute generation function, remixed from nct6683.c */
struct sensor_device_template {
	struct device_attribute dev_attr;
	union {
		struct {
			u8 nr;
			u8 index;
		} s;
		int index;
	} u;
	bool s2;	/* true if both index and nr are used */
};

struct sensor_device_attr_u {
	union {
		struct sensor_device_attribute a1;
		struct sensor_device_attribute_2 a2;
	} u;
	char name[32];
};

#define __TEMPLATE_ATTR(_template, _mode, _show, _store) {	\
	.attr = {.name = _template, .mode = _mode },		\
	.show	= _show,					\
	.store	= _store,					\
}

#define SENSOR_DEVICE_TEMPLATE(_template, _mode, _show, _store, _index)	\
	{ .dev_attr = __TEMPLATE_ATTR(_template, _mode, _show, _store),	\
	  .u.index = _index,						\
	  .s2 = false }

#define SENSOR_DEVICE_TEMPLATE_2(_template, _mode, _show, _store,	\
				 _nr, _index)				\
	{ .dev_attr = __TEMPLATE_ATTR(_template, _mode, _show, _store),	\
	  .u.s.index = _index,						\
	  .u.s.nr = _nr,						\
	  .s2 = true }

#define SENSOR_TEMPLATE(_name, _template, _mode, _show, _store, _index)	\
static struct sensor_device_template sensor_dev_template_##_name =	\
	SENSOR_DEVICE_TEMPLATE(_template, _mode, _show, _store,		\
				 _index)

#define SENSOR_TEMPLATE_2(_name, _template, _mode, _show, _store,	\
			  _nr, _index)					\
static struct sensor_device_template sensor_dev_template_##_name =	\
	SENSOR_DEVICE_TEMPLATE_2(_template, _mode, _show, _store,	\
				 _nr, _index)

struct sensor_template_group {
	struct sensor_device_template **templates;
	umode_t (*is_visible)(struct kobject *kobj, struct attribute *attr, int index);
	int base;
};

static struct attribute_group *aqc_create_attr_group(struct device *dev,
						     const struct sensor_template_group *tg,
						     int repeat)
{
	struct sensor_device_attribute_2 *a2;
	struct sensor_device_attribute *a;
	struct sensor_device_template **t;
	struct sensor_device_attr_u *su;
	struct attribute_group *group;
	struct attribute **attrs;
	int i, count;

	if (repeat <= 0)
		return ERR_PTR(-EINVAL);

	t = tg->templates;
	for (count = 0; *t; t++, count++)
		;

	if (count == 0)
		return ERR_PTR(-EINVAL);

	group = devm_kzalloc(dev, sizeof(*group), GFP_KERNEL);
	if (!group)
		return ERR_PTR(-ENOMEM);

	attrs = devm_kcalloc(dev, repeat * count + 1, sizeof(*attrs), GFP_KERNEL);
	if (!attrs)
		return ERR_PTR(-ENOMEM);

	su = devm_kzalloc(dev, array3_size(repeat, count, sizeof(*su)), GFP_KERNEL);
	if (!su)
		return ERR_PTR(-ENOMEM);

	group->attrs = attrs;
	group->is_visible = tg->is_visible;

	for (i = 0; i < repeat; i++) {
		t = tg->templates;
		while (*t) {
			snprintf(su->name, sizeof(su->name),
				 (*t)->dev_attr.attr.name, tg->base + i);
			if ((*t)->s2) {
				a2 = &su->u.a2;
				sysfs_attr_init(&a2->dev_attr.attr);
				a2->dev_attr.attr.name = su->name;
				a2->nr = (*t)->u.s.nr + i;
				a2->index = (*t)->u.s.index;
				a2->dev_attr.attr.mode = (*t)->dev_attr.attr.mode;
				a2->dev_attr.show = (*t)->dev_attr.show;
				a2->dev_attr.store = (*t)->dev_attr.store;
				*attrs = &a2->dev_attr.attr;
			} else {
				a = &su->u.a1;
				sysfs_attr_init(&a->dev_attr.attr);
				a->dev_attr.attr.name = su->name;
				a->index = (*t)->u.index + i;
				a->dev_attr.attr.mode = (*t)->dev_attr.attr.mode;
				a->dev_attr.show = (*t)->dev_attr.show;
				a->dev_attr.store = (*t)->dev_attr.store;
				*attrs = &a->dev_attr.attr;
			}
			attrs++;
			su++;
			t++;
		}
	}

	return group;
}

/* Temp-PWM curve show and store functions */
static ssize_t show_auto_temp(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aqc_data *priv = dev_get_drvdata(dev);
	struct sensor_device_attribute_2 *sattr = to_sensor_dev_attr_2(attr);
	int nr = sattr->nr;
	int point = sattr->index;

	unsigned long val;
	int ret = aqc_get_ctrl_val(priv,
				   priv->fan_ctrl_offsets[nr] + AQC_FAN_CTRL_TEMP_CURVE_START +
				   point * AQC_SENSOR_SIZE, &val, AQC_BE16);
	if (ret < 0)
		return -ENODATA;

	return sprintf(buf, "%d\n", (s16)val);
}

static ssize_t
store_auto_temp(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct aqc_data *priv = dev_get_drvdata(dev);
	struct sensor_device_attribute_2 *sattr = to_sensor_dev_attr_2(attr);
	int nr = sattr->nr;
	int point = sattr->index;
	unsigned long val;
	int ret = kstrtoul(buf, 10, &val);

	if (ret < 0)
		return ret;

	ret = aqc_set_ctrl_val(priv,
			       priv->fan_ctrl_offsets[nr] + AQC_FAN_CTRL_TEMP_CURVE_START +
			       point * AQC_SENSOR_SIZE, val, AQC_BE16);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t show_auto_pwm(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aqc_data *priv = dev_get_drvdata(dev);
	struct sensor_device_attribute_2 *sattr = to_sensor_dev_attr_2(attr);
	int nr = sattr->nr;
	int point = sattr->index;

	unsigned long val;
	int ret = aqc_get_ctrl_val(priv,
				   priv->fan_ctrl_offsets[nr] + AQC_FAN_CTRL_PWM_CURVE_START +
				   point * AQC_SENSOR_SIZE, &val, AQC_BE16);
	if (ret < 0)
		return -ENODATA;

	return sprintf(buf, "%d\n", aqc_percent_to_pwm(val));
}

static ssize_t
store_auto_pwm(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct aqc_data *priv = dev_get_drvdata(dev);
	struct sensor_device_attribute_2 *sattr = to_sensor_dev_attr_2(attr);
	int nr = sattr->nr;
	int point = sattr->index;
	unsigned long val;
	int pwm_value;

	int ret = kstrtoul(buf, 10, &val);

	if (ret < 0)
		return ret;
	if (val > 255)
		return -EINVAL;

	pwm_value = aqc_pwm_to_percent(val);
	ret = aqc_set_ctrl_val(priv,
			       priv->fan_ctrl_offsets[nr] + AQC_FAN_CTRL_PWM_CURVE_START +
			       point * AQC_SENSOR_SIZE, pwm_value, AQC_BE16);
	if (ret < 0)
		return ret;

	return count;
}

SENSOR_TEMPLATE_2(temp_auto_point1_pwm, "temp%d_auto_point1_pwm",
		  0644, show_auto_pwm, store_auto_pwm, 0, 0);
SENSOR_TEMPLATE_2(temp_auto_point1_temp, "temp%d_auto_point1_temp",
		  0644, show_auto_temp, store_auto_temp, 0, 0);

SENSOR_TEMPLATE_2(temp_auto_point2_pwm, "temp%d_auto_point2_pwm",
		  0644, show_auto_pwm, store_auto_pwm, 0, 1);
SENSOR_TEMPLATE_2(temp_auto_point2_temp, "temp%d_auto_point2_temp",
		  0644, show_auto_temp, store_auto_temp, 0, 1);

SENSOR_TEMPLATE_2(temp_auto_point3_pwm, "temp%d_auto_point3_pwm",
		  0644, show_auto_pwm, store_auto_pwm, 0, 2);
SENSOR_TEMPLATE_2(temp_auto_point3_temp, "temp%d_auto_point3_temp",
		  0644, show_auto_temp, store_auto_temp, 0, 2);

SENSOR_TEMPLATE_2(temp_auto_point4_pwm, "temp%d_auto_point4_pwm",
		  0644, show_auto_pwm, store_auto_pwm, 0, 3);
SENSOR_TEMPLATE_2(temp_auto_point4_temp, "temp%d_auto_point4_temp",
		  0644, show_auto_temp, store_auto_temp, 0, 3);

SENSOR_TEMPLATE_2(temp_auto_point5_pwm, "temp%d_auto_point5_pwm",
		  0644, show_auto_pwm, store_auto_pwm, 0, 4);
SENSOR_TEMPLATE_2(temp_auto_point5_temp, "temp%d_auto_point5_temp",
		  0644, show_auto_temp, store_auto_temp, 0, 4);

SENSOR_TEMPLATE_2(temp_auto_point6_pwm, "temp%d_auto_point6_pwm",
		  0644, show_auto_pwm, store_auto_pwm, 0, 5);
SENSOR_TEMPLATE_2(temp_auto_point6_temp, "temp%d_auto_point6_temp",
		  0644, show_auto_temp, store_auto_temp, 0, 5);

SENSOR_TEMPLATE_2(temp_auto_point7_pwm, "temp%d_auto_point7_pwm",
		  0644, show_auto_pwm, store_auto_pwm, 0, 6);
SENSOR_TEMPLATE_2(temp_auto_point7_temp, "temp%d_auto_point7_temp",
		  0644, show_auto_temp, store_auto_temp, 0, 6);

SENSOR_TEMPLATE_2(temp_auto_point8_pwm, "temp%d_auto_point8_pwm",
		  0644, show_auto_pwm, store_auto_pwm, 0, 7);
SENSOR_TEMPLATE_2(temp_auto_point8_temp, "temp%d_auto_point8_temp",
		  0644, show_auto_temp, store_auto_temp, 0, 7);

SENSOR_TEMPLATE_2(temp_auto_point9_pwm, "temp%d_auto_point9_pwm",
		  0644, show_auto_pwm, store_auto_pwm, 0, 8);
SENSOR_TEMPLATE_2(temp_auto_point9_temp, "temp%d_auto_point9_temp",
		  0644, show_auto_temp, store_auto_temp, 0, 8);

SENSOR_TEMPLATE_2(temp_auto_point10_pwm, "temp%d_auto_point10_pwm",
		  0644, show_auto_pwm, store_auto_pwm, 0, 9);
SENSOR_TEMPLATE_2(temp_auto_point10_temp, "temp%d_auto_point10_temp",
		  0644, show_auto_temp, store_auto_temp, 0, 9);

SENSOR_TEMPLATE_2(temp_auto_point11_pwm, "temp%d_auto_point11_pwm",
		  0644, show_auto_pwm, store_auto_pwm, 0, 10);
SENSOR_TEMPLATE_2(temp_auto_point11_temp, "temp%d_auto_point11_temp",
		  0644, show_auto_temp, store_auto_temp, 0, 10);

SENSOR_TEMPLATE_2(temp_auto_point12_pwm, "temp%d_auto_point12_pwm",
		  0644, show_auto_pwm, store_auto_pwm, 0, 11);
SENSOR_TEMPLATE_2(temp_auto_point12_temp, "temp%d_auto_point12_temp",
		  0644, show_auto_temp, store_auto_temp, 0, 11);

SENSOR_TEMPLATE_2(temp_auto_point13_pwm, "temp%d_auto_point13_pwm",
		  0644, show_auto_pwm, store_auto_pwm, 0, 12);
SENSOR_TEMPLATE_2(temp_auto_point13_temp, "temp%d_auto_point13_temp",
		  0644, show_auto_temp, store_auto_temp, 0, 12);

SENSOR_TEMPLATE_2(temp_auto_point14_pwm, "temp%d_auto_point14_pwm",
		  0644, show_auto_pwm, store_auto_pwm, 0, 13);
SENSOR_TEMPLATE_2(temp_auto_point14_temp, "temp%d_auto_point14_temp",
		  0644, show_auto_temp, store_auto_temp, 0, 13);

SENSOR_TEMPLATE_2(temp_auto_point15_pwm, "temp%d_auto_point15_pwm",
		  0644, show_auto_pwm, store_auto_pwm, 0, 14);
SENSOR_TEMPLATE_2(temp_auto_point15_temp, "temp%d_auto_point15_temp",
		  0644, show_auto_temp, store_auto_temp, 0, 14);

SENSOR_TEMPLATE_2(temp_auto_point16_pwm, "temp%d_auto_point16_pwm",
		  0644, show_auto_pwm, store_auto_pwm, 0, 15);
SENSOR_TEMPLATE_2(temp_auto_point16_temp, "temp%d_auto_point16_temp",
		  0644, show_auto_temp, store_auto_temp, 0, 15);

static umode_t aqc_curve_is_visible(struct kobject *kobj, struct attribute *attr, int index)
{
	/* Each fan always has 16 points available */
	return attr->mode;
}

static struct sensor_device_template *aqc_attributes_curve_template[] = {
	&sensor_dev_template_temp_auto_point1_pwm,
	&sensor_dev_template_temp_auto_point1_temp,
	&sensor_dev_template_temp_auto_point2_pwm,
	&sensor_dev_template_temp_auto_point2_temp,
	&sensor_dev_template_temp_auto_point3_pwm,
	&sensor_dev_template_temp_auto_point3_temp,
	&sensor_dev_template_temp_auto_point4_pwm,
	&sensor_dev_template_temp_auto_point4_temp,
	&sensor_dev_template_temp_auto_point5_pwm,
	&sensor_dev_template_temp_auto_point5_temp,
	&sensor_dev_template_temp_auto_point6_pwm,
	&sensor_dev_template_temp_auto_point6_temp,
	&sensor_dev_template_temp_auto_point7_pwm,
	&sensor_dev_template_temp_auto_point7_temp,
	&sensor_dev_template_temp_auto_point8_pwm,
	&sensor_dev_template_temp_auto_point8_temp,
	&sensor_dev_template_temp_auto_point9_pwm,
	&sensor_dev_template_temp_auto_point9_temp,
	&sensor_dev_template_temp_auto_point10_pwm,
	&sensor_dev_template_temp_auto_point10_temp,
	&sensor_dev_template_temp_auto_point11_pwm,
	&sensor_dev_template_temp_auto_point11_temp,
	&sensor_dev_template_temp_auto_point12_pwm,
	&sensor_dev_template_temp_auto_point12_temp,
	&sensor_dev_template_temp_auto_point13_pwm,
	&sensor_dev_template_temp_auto_point13_temp,
	&sensor_dev_template_temp_auto_point14_pwm,
	&sensor_dev_template_temp_auto_point14_temp,
	&sensor_dev_template_temp_auto_point15_pwm,
	&sensor_dev_template_temp_auto_point15_temp,
	&sensor_dev_template_temp_auto_point16_pwm,
	&sensor_dev_template_temp_auto_point16_temp,
	NULL
};

static const struct sensor_template_group aqc_curve_template_group = {
	.templates = aqc_attributes_curve_template,
	.is_visible = aqc_curve_is_visible,
	.base = 1,
};

/* Fan curve parameters (for both PID mode and temp-PWM curves) */
static ssize_t show_curve_power_min(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aqc_data *priv = dev_get_drvdata(dev);
	struct sensor_device_attribute *sattr = to_sensor_dev_attr(attr);
	int index = sattr->index;

	unsigned long val;
	int ret = aqc_get_ctrl_val(priv,
				   priv->fan_curve_min_power_offsets[index], &val, AQC_BE16);
	if (ret < 0)
		return -ENODATA;

	return sprintf(buf, "%d\n", aqc_percent_to_pwm(val));
}

static ssize_t
store_curve_power_min(struct device *dev, struct device_attribute *attr, const char *buf,
		      size_t count)
{
	struct aqc_data *priv = dev_get_drvdata(dev);
	struct sensor_device_attribute *sattr = to_sensor_dev_attr(attr);
	int index = sattr->index;
	unsigned long val;
	int ret = kstrtoul(buf, 10, &val);
	int pwm_value;

	if (ret < 0)
		return ret;
	if (val > 255)
		return -EINVAL;

	pwm_value = aqc_pwm_to_percent(val);
	ret = aqc_set_ctrl_val(priv, priv->fan_curve_min_power_offsets[index], pwm_value, AQC_BE16);
	if (ret < 0)
		return ret;

	return count;
}

/* Fan curve parameters (for both PID and temp-PWM curves) */
static ssize_t show_curve_power_max(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aqc_data *priv = dev_get_drvdata(dev);
	struct sensor_device_attribute *sattr = to_sensor_dev_attr(attr);
	int index = sattr->index;

	unsigned long val;
	int ret = aqc_get_ctrl_val(priv,
				   priv->fan_curve_max_power_offsets[index], &val, AQC_BE16);
	if (ret < 0)
		return -ENODATA;

	return sprintf(buf, "%d\n", aqc_percent_to_pwm(val));
}

static ssize_t
store_curve_power_max(struct device *dev, struct device_attribute *attr, const char *buf,
		      size_t count)
{
	struct aqc_data *priv = dev_get_drvdata(dev);
	struct sensor_device_attribute *sattr = to_sensor_dev_attr(attr);
	int index = sattr->index;
	unsigned long val;
	int ret = kstrtoul(buf, 10, &val);
	int pwm_value;

	if (ret < 0)
		return ret;
	if (val > 255)
		return -EINVAL;

	pwm_value = aqc_pwm_to_percent(val);
	ret = aqc_set_ctrl_val(priv, priv->fan_curve_max_power_offsets[index], pwm_value, AQC_BE16);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t show_curve_power_fallback(struct device *dev, struct device_attribute *attr,
					 char *buf)
{
	struct aqc_data *priv = dev_get_drvdata(dev);
	struct sensor_device_attribute *sattr = to_sensor_dev_attr(attr);
	int index = sattr->index;

	unsigned long val;
	int ret = aqc_get_ctrl_val(priv,
				   priv->fan_curve_fallback_power_offsets[index], &val, AQC_BE16);
	if (ret < 0)
		return -ENODATA;

	return sprintf(buf, "%d\n", aqc_percent_to_pwm(val));
}

static ssize_t
store_curve_power_fallback(struct device *dev, struct device_attribute *attr, const char *buf,
			   size_t count)
{
	struct aqc_data *priv = dev_get_drvdata(dev);
	struct sensor_device_attribute *sattr = to_sensor_dev_attr(attr);
	int index = sattr->index;
	unsigned long val;
	int ret = kstrtoul(buf, 10, &val);
	int pwm_value;

	if (ret < 0)
		return ret;
	if (val > 255)
		return -EINVAL;

	pwm_value = aqc_pwm_to_percent(val);
	ret = aqc_set_ctrl_val(priv,
			       priv->fan_curve_fallback_power_offsets[index], pwm_value, AQC_BE16);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t show_curve_start_boost(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aqc_data *priv = dev_get_drvdata(dev);
	struct sensor_device_attribute *sattr = to_sensor_dev_attr(attr);
	int index = sattr->index, ret;
	unsigned long val;

	ret = aqc_get_ctrl_val(priv,
			       priv->fan_curve_hold_start_offsets[index], &val, AQC_8);
	if (ret < 0)
		return -ENODATA;

	return sprintf(buf, "%d\n", aqc_get_bit_at_pos(val, FAN_CURVE_START_BOOST_BIT_POS));
}

static ssize_t
store_curve_start_boost(struct device *dev, struct device_attribute *attr, const char *buf,
			size_t count)
{
	struct aqc_data *priv = dev_get_drvdata(dev);
	struct sensor_device_attribute *sattr = to_sensor_dev_attr(attr);
	int index = sattr->index;
	unsigned long val, new_val;
	int ret = kstrtoul(buf, 10, &val);

	if (ret < 0)
		return ret;
	if (val > 2)
		return -EINVAL;

	ret = aqc_get_ctrl_val(priv, priv->fan_curve_hold_start_offsets[index], &new_val, AQC_8);
	if (ret < 0)
		return ret;

	new_val = aqc_set_bit_at_pos(new_val, FAN_CURVE_START_BOOST_BIT_POS, val);

	ret = aqc_set_ctrl_val(priv, priv->fan_curve_hold_start_offsets[index], new_val, AQC_8);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t show_curve_power_hold_min(struct device *dev, struct device_attribute *attr,
					 char *buf)
{
	struct aqc_data *priv = dev_get_drvdata(dev);
	struct sensor_device_attribute *sattr = to_sensor_dev_attr(attr);
	int index = sattr->index, ret;
	unsigned long val;

	ret = aqc_get_ctrl_val(priv,
			       priv->fan_curve_hold_start_offsets[index], &val, AQC_8);
	if (ret < 0)
		return -ENODATA;

	return sprintf(buf, "%d\n", aqc_get_bit_at_pos(val, FAN_CURVE_HOLD_MIN_POWER_BIT_POS));
}

static ssize_t
store_curve_power_hold_min(struct device *dev, struct device_attribute *attr, const char *buf,
			   size_t count)
{
	struct aqc_data *priv = dev_get_drvdata(dev);
	struct sensor_device_attribute *sattr = to_sensor_dev_attr(attr);
	int index = sattr->index;
	unsigned long val, new_val;
	int ret = kstrtoul(buf, 10, &val);

	if (ret < 0)
		return ret;
	if (val > 2)
		return -EINVAL;

	ret = aqc_get_ctrl_val(priv, priv->fan_curve_hold_start_offsets[index], &new_val, AQC_8);
	if (ret < 0)
		return ret;

	new_val = aqc_set_bit_at_pos(new_val, FAN_CURVE_HOLD_MIN_POWER_BIT_POS, val);

	ret = aqc_set_ctrl_val(priv, priv->fan_curve_hold_start_offsets[index], new_val, AQC_8);
	if (ret < 0)
		return ret;

	return count;
}

SENSOR_TEMPLATE(curve_power_min, "curve%d_power_min",
		0644, show_curve_power_min, store_curve_power_min, 0);
SENSOR_TEMPLATE(curve_power_max, "curve%d_power_max",
		0644, show_curve_power_max, store_curve_power_max, 0);
SENSOR_TEMPLATE(curve_power_fallback, "curve%d_power_fallback",
		0644, show_curve_power_fallback, store_curve_power_fallback, 0);
SENSOR_TEMPLATE(curve_start_boost, "curve%d_start_boost",
		0644, show_curve_start_boost, store_curve_start_boost, 0);
SENSOR_TEMPLATE(curve_power_hold_min, "curve%d_power_hold_min",
		0644, show_curve_power_hold_min, store_curve_power_hold_min, 0);

static umode_t aqc_params_is_visible(struct kobject *kobj, struct attribute *attr, int index)
{
	/*
	 * Pump channel on the D5 Next does not support the last two features,
	 * "start boost" and "hold min power". Every other fan curve supports
	 * all parameters.
	 */
	struct device *dev = kobj_to_dev(kobj);
	struct aqc_data *priv = dev_get_drvdata(dev);
	int nr = index % 5;

	if (priv->kind == d5next && nr > 2)
		return 0;

	return attr->mode;
}

static struct sensor_device_template *aqc_attributes_params_template[] = {
	&sensor_dev_template_curve_power_min,
	&sensor_dev_template_curve_power_max,
	&sensor_dev_template_curve_power_fallback,
	&sensor_dev_template_curve_start_boost,
	&sensor_dev_template_curve_power_hold_min,
	NULL
};

static const struct sensor_template_group aqc_curve_params_template_group = {
	.templates = aqc_attributes_params_template,
	.is_visible = aqc_params_is_visible,
	.base = 1,
};

static const struct hwmon_ops aqc_hwmon_ops = {
	.is_visible = aqc_is_visible,
	.read = aqc_read,
	.read_string = aqc_read_string,
	.write = aqc_write
};

static const struct hwmon_channel_info * const aqc_info[] = {
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
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
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
		/* Read hardware version (for v5: 5600, for v6: 6000) */
		priv->aquaero_hw_version = get_unaligned_be16(data + AQUAERO_HARDWARE_VERSION);

		switch (priv->aquaero_hw_version) {
		case AQUAERO_5_HW_VERSION:
			priv->aquaero_hw_kind = aquaero5;
			break;
		case AQUAERO_6_HW_VERSION:
			priv->aquaero_hw_kind = aquaero6;
			break;
		default:
			priv->aquaero_hw_kind = unknown;
			break;
		}

		priv->current_uptime = get_unaligned_be32(data + AQUAERO_CURRENT_UPTIME_OFFSET);
		priv->total_uptime = get_unaligned_be32(data + AQUAERO_TOTAL_UPTIME_OFFSET);

		/* Read Aquabus flow sensors */
		for (j = 0; j < priv->num_aquabus_flow_sensors; j++) {
			sensor_value = get_unaligned_be16(data +
							  priv->aquabus_flow_sensors_start_offset +
							  j * AQC_SENSOR_SIZE);

			if (sensor_value == AQC_SENSOR_NA)
				priv->speed_input[i] = -ENODATA;
			else
				priv->speed_input[i] = sensor_value;
			i++;
		}

		/* Read calculated virtual temp sensors */
		i = priv->num_temp_sensors + priv->num_virtual_temp_sensors;
		for (j = 0; j < priv->num_calc_virt_temp_sensors; j++) {
			sensor_value = get_unaligned_be16(data +
							  priv->calc_virt_temp_sensor_start_offset
							  + j * AQC_SENSOR_SIZE);
			if (sensor_value == AQC_SENSOR_NA)
				priv->temp_input[i] = -ENODATA;
			else
				priv->temp_input[i] = sensor_value * 10;
			i++;
		}

		/* Read Aquabus temp sensors */
		for (j = 0; j < priv->num_aquabus_temp_sensors; j++) {
			sensor_value = get_unaligned_be16(data +
							  priv->aquabus_temp_sensor_start_offset
							  + j * AQC_SENSOR_SIZE);
			if (sensor_value == AQC_SENSOR_NA)
				priv->temp_input[i] = -ENODATA;
			else
				priv->temp_input[i] = sensor_value * 10;
			i++;
		}

		if (!completion_done(&aquaero_sensor_report_received))
			complete_all(&aquaero_sensor_report_received);
		break;
	case aquastreamult:
		priv->speed_input[1] = get_unaligned_be16(data + AQUASTREAMULT_PUMP_OFFSET);
		priv->speed_input[2] = get_unaligned_be16(data + AQUASTREAMULT_PRESSURE_OFFSET);
		priv->speed_input[3] = get_unaligned_be16(data + AQUASTREAMULT_FLOW_SENSOR_OFFSET);

		priv->power_input[1] = get_unaligned_be16(data + AQUASTREAMULT_PUMP_POWER) * 10000;

		priv->voltage_input[1] = get_unaligned_be16(data + AQUASTREAMULT_PUMP_VOLTAGE) * 10;

		priv->current_input[1] = get_unaligned_be16(data + AQUASTREAMULT_PUMP_CURRENT);
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

static int hw_version_show(struct seq_file *seqf, void *unused)
{
	struct aqc_data *priv = seqf->private;

	wait_for_completion(&aquaero_sensor_report_received);

	seq_printf(seqf, "%u\n", priv->aquaero_hw_version);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hw_version);

static int current_uptime_show(struct seq_file *seqf, void *unused)
{
	struct aqc_data *priv = seqf->private;

	wait_for_completion(&aquaero_sensor_report_received);

	seq_printf(seqf, "%u\n", priv->current_uptime);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(current_uptime);

static int total_uptime_show(struct seq_file *seqf, void *unused)
{
	struct aqc_data *priv = seqf->private;

	wait_for_completion(&aquaero_sensor_report_received);

	seq_printf(seqf, "%u\n", priv->total_uptime);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(total_uptime);

static void aqc_debugfs_init(struct aqc_data *priv)
{
	char name[64];

	scnprintf(name, sizeof(name), "%s_%s-%s", "aquacomputer", priv->name,
		  dev_name(&priv->hdev->dev));

	priv->debugfs = debugfs_create_dir(name, NULL);

	if (priv->serial_number_start_offset != 0)
		debugfs_create_file("serial_number", 0444, priv->debugfs, priv,
				    &serial_number_fops);
	if (priv->firmware_version_offset != 0)
		debugfs_create_file("firmware_version", 0444, priv->debugfs, priv,
				    &firmware_version_fops);
	if (priv->power_cycle_count_offset != 0)
		debugfs_create_file("power_cycles", 0444, priv->debugfs, priv, &power_cycles_fops);

	if (priv->kind == aquaero) {
		debugfs_create_file("hw_version", 0444, priv->debugfs, priv, &hw_version_fops);
		debugfs_create_file("current_uptime", 0444, priv->debugfs, priv,
				    &current_uptime_fops);
		debugfs_create_file("total_uptime", 0444, priv->debugfs, priv, &total_uptime_fops);
	}
}

#else

static void aqc_debugfs_init(struct aqc_data *priv)
{
}

#endif

static int aqc_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct aqc_data *priv;
	struct attribute_group *group;
	int ret, groups;

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
		priv->num_calc_virt_temp_sensors = AQUAERO_NUM_CALC_VIRTUAL_SENSORS;
		priv->calc_virt_temp_sensor_start_offset = AQUAERO_CALC_VIRTUAL_SENSOR_START;
		priv->num_aquabus_temp_sensors = AQUAERO_NUM_AQUABUS_SENSORS;
		priv->aquabus_temp_sensor_start_offset = AQUAERO_AQUABUS_SENSOR_START;
		priv->num_flow_sensors = AQUAERO_NUM_FLOW_SENSORS;
		priv->flow_sensors_start_offset = AQUAERO_FLOW_SENSORS_START;
		priv->num_aquabus_flow_sensors = AQUAERO_NUM_AQUABUS_FLOW_SENSORS;
		priv->aquabus_flow_sensors_start_offset = AQUAERO_AQUABUS_FLOW_SENSORS_START;

		priv->buffer_size = AQUAERO_CTRL_REPORT_SIZE;
		priv->temp_ctrl_offset = AQUAERO_TEMP_CTRL_OFFSET;
		priv->ctrl_report_delay = CTRL_REPORT_DELAY;

		priv->temp_label = label_temp_sensors;
		priv->virtual_temp_label = label_virtual_temp_sensors;
		priv->calc_virtual_temp_label = label_aquaero_calc_temp_sensors;
		priv->aquabus_temp_label = label_aquaero_aquabus_temp_sensors;
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
		priv->fan_curve_min_power_offsets = d5next_ctrl_fan_curve_min_power_offsets;
		priv->fan_curve_max_power_offsets = d5next_ctrl_fan_curve_max_power_offsets;
		priv->fan_curve_hold_start_offsets = d5next_ctrl_fan_curve_hold_start_offsets;
		priv->fan_curve_fallback_power_offsets =
		    d5next_ctrl_fan_curve_fallback_power_offsets;

		priv->num_temp_sensors = D5NEXT_NUM_SENSORS;
		priv->temp_sensor_start_offset = D5NEXT_COOLANT_TEMP;
		priv->num_virtual_temp_sensors = D5NEXT_NUM_VIRTUAL_SENSORS;
		priv->virtual_temp_sensor_start_offset = D5NEXT_VIRTUAL_SENSORS_START;

		priv->power_cycle_count_offset = AQC_POWER_CYCLES;
		priv->buffer_size = D5NEXT_CTRL_REPORT_SIZE;
		priv->temp_ctrl_offset = D5NEXT_TEMP_CTRL_OFFSET;
		priv->ctrl_report_delay = CTRL_REPORT_DELAY;

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
		priv->fan_curve_min_power_offsets = octo_ctrl_fan_curve_min_power_offsets;
		priv->fan_curve_max_power_offsets = octo_ctrl_fan_curve_max_power_offsets;
		priv->fan_curve_hold_start_offsets = octo_ctrl_fan_curve_hold_start_offsets;
		priv->fan_curve_fallback_power_offsets = octo_ctrl_fan_curve_fallback_power_offsets;

		priv->num_temp_sensors = OCTO_NUM_SENSORS;
		priv->temp_sensor_start_offset = OCTO_SENSOR_START;
		priv->num_virtual_temp_sensors = OCTO_NUM_VIRTUAL_SENSORS;
		priv->virtual_temp_sensor_start_offset = OCTO_VIRTUAL_SENSORS_START;

		priv->power_cycle_count_offset = AQC_POWER_CYCLES;
		priv->buffer_size = OCTO_CTRL_REPORT_SIZE;
		priv->temp_ctrl_offset = OCTO_TEMP_CTRL_OFFSET;
		priv->ctrl_report_delay = CTRL_REPORT_DELAY;

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
		priv->fan_curve_min_power_offsets = quadro_ctrl_fan_curve_min_power_offsets;
		priv->fan_curve_max_power_offsets = quadro_ctrl_fan_curve_max_power_offsets;
		priv->fan_curve_hold_start_offsets = quadro_ctrl_fan_curve_hold_start_offsets;
		priv->fan_curve_fallback_power_offsets =
		    quadro_ctrl_fan_curve_fallback_power_offsets;

		priv->num_temp_sensors = QUADRO_NUM_SENSORS;
		priv->temp_sensor_start_offset = QUADRO_SENSOR_START;
		priv->num_virtual_temp_sensors = QUADRO_NUM_VIRTUAL_SENSORS;
		priv->virtual_temp_sensor_start_offset = QUADRO_VIRTUAL_SENSORS_START;
		priv->num_flow_sensors = QUADRO_NUM_FLOW_SENSORS;
		priv->flow_sensors_start_offset = QUADRO_FLOW_SENSOR_OFFSET;

		priv->power_cycle_count_offset = AQC_POWER_CYCLES;
		priv->buffer_size = QUADRO_CTRL_REPORT_SIZE;
		priv->ctrl_report_delay = CTRL_REPORT_DELAY;
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
		priv->num_flow_sensors = HIGHFLOWNEXT_NUM_FLOW_SENSORS;
		priv->flow_sensors_start_offset = HIGHFLOWNEXT_FLOW;

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

		/*
		 * Since we use the same buffer for both sensor and control
		 * report storage on legacy devices, reserve enough space
		 */
		priv->buffer_size = max(AQUASTREAMXT_SENSOR_REPORT_SIZE,
					AQUASTREAMXT_CTRL_REPORT_SIZE);

		priv->temp_label = label_aquastreamxt_temp_sensors;
		priv->speed_label = label_d5next_speeds;
		priv->voltage_label = label_d5next_voltages;
		priv->current_label = label_d5next_current;
		break;
	case USB_PRODUCT_ID_AQUASTREAMULT:
		priv->kind = aquastreamult;

		priv->num_fans = AQUASTREAMULT_NUM_FANS;
		priv->fan_sensor_offsets = aquastreamult_sensor_fan_offsets;

		priv->num_temp_sensors = AQUASTREAMULT_NUM_SENSORS;
		priv->temp_sensor_start_offset = AQUASTREAMULT_SENSOR_START;

		priv->temp_label = label_aquastreamult_temp;
		priv->speed_label = label_aquastreamult_speeds;
		priv->power_label = label_aquastreamult_power;
		priv->voltage_label = label_aquastreamult_voltages;
		priv->current_label = label_aquastreamult_current;
		break;
	case USB_PRODUCT_ID_POWERADJUST3:
		priv->kind = poweradjust3;

		priv->num_fans = 0;

		priv->num_temp_sensors = POWERADJUST3_NUM_SENSORS;
		priv->temp_sensor_start_offset = POWERADJUST3_SENSOR_START;
		priv->buffer_size = POWERADJUST3_SENSOR_REPORT_SIZE;

		priv->temp_label = label_poweradjust3_temp_sensors;
		break;
	case USB_PRODUCT_ID_HIGHFLOW:
		priv->kind = highflow;

		priv->num_fans = 0;

		priv->num_temp_sensors = HIGHFLOW_NUM_SENSORS;
		priv->temp_sensor_start_offset = HIGHFLOW_SENSOR_START;
		priv->buffer_size = HIGHFLOW_SENSOR_REPORT_SIZE;

		priv->temp_label = label_highflow_temp;
		break;
	default:
		break;
	}

	switch (priv->kind) {
	case aquaero:
		priv->serial_number_start_offset = AQUAERO_SERIAL_START;
		priv->firmware_version_offset = AQUAERO_FIRMWARE_VERSION;

		priv->fan_structure = &aqc_aquaero_fan_structure;

		priv->ctrl_report_id = AQUAERO_CTRL_REPORT_ID;
		priv->secondary_ctrl_report_id = AQUAERO_SECONDARY_CTRL_REPORT_ID;
		priv->secondary_ctrl_report_size = AQUAERO_SECONDARY_CTRL_REPORT_SIZE;
		priv->secondary_ctrl_report = aquaero_secondary_ctrl_report;
		break;
	case aquastreamxt:
		priv->serial_number_start_offset = AQUASTREAMXT_SERIAL_START;
		priv->firmware_version_offset = AQUASTREAMXT_FIRMWARE_VERSION;

		priv->status_report_id = AQUASTREAMXT_STATUS_REPORT_ID;
		priv->ctrl_report_id = AQUASTREAMXT_CTRL_REPORT_ID;
		priv->secondary_ctrl_report_id = AQUASTREAMXT_SECONDARY_CTRL_REPORT_ID;
		priv->secondary_ctrl_report_size = AQUASTREAMXT_SECONDARY_CTRL_REPORT_SIZE;
		priv->secondary_ctrl_report = aquastreamxt_secondary_ctrl_report;
		break;
	case poweradjust3:
		priv->status_report_id = POWERADJUST3_STATUS_REPORT_ID;
		break;
	case highflow:
		priv->serial_number_start_offset = HIGHFLOW_SERIAL_START;
		priv->firmware_version_offset = HIGHFLOW_FIRMWARE_VERSION;

		priv->status_report_id = HIGHFLOW_STATUS_REPORT_ID;
		break;
	default:
		priv->serial_number_start_offset = AQC_SERIAL_START;
		priv->firmware_version_offset = AQC_FIRMWARE_VERSION;

		if (priv->kind == aquastreamult) {
			priv->fan_structure = &aqc_aquastreamult_fan_structure;
		} else {
			priv->fan_structure = &aqc_general_fan_structure;

			priv->ctrl_report_id = CTRL_REPORT_ID;
			priv->secondary_ctrl_report_id = SECONDARY_CTRL_REPORT_ID;
			priv->secondary_ctrl_report_size = SECONDARY_CTRL_REPORT_SIZE;
			priv->secondary_ctrl_report = secondary_ctrl_report;
		}
		break;
	}

	/* Set up temp-PWM curves and their parameters for devices that support them */
	if (priv->fan_ctrl_offsets) {
		switch (priv->kind) {
		case d5next:
		case octo:
		case quadro:
			/* Temp-PWM curve */
			group =
			    aqc_create_attr_group(&hdev->dev, &aqc_curve_template_group,
						  priv->num_fans);
			if (IS_ERR(group))
				return PTR_ERR(group);
			priv->groups[groups++] = group;

			/* General curve parameters */
			group =
			    aqc_create_attr_group(&hdev->dev, &aqc_curve_params_template_group,
						  priv->num_fans);
			if (IS_ERR(group))
				return PTR_ERR(group);
			priv->groups[groups++] = group;
			break;
		default:
			break;
		}
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

	hid_device_io_start(hdev);
	priv->hwmon_dev = hwmon_device_register_with_info(&hdev->dev, priv->name, priv,
							  &aqc_chip_info, priv->groups);

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
	{ HID_USB_DEVICE(USB_VENDOR_ID_AQUACOMPUTER, USB_PRODUCT_ID_AQUASTREAMULT) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_AQUACOMPUTER, USB_PRODUCT_ID_POWERADJUST3) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_AQUACOMPUTER, USB_PRODUCT_ID_HIGHFLOW) },
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
