.. SPDX-License-Identifier: GPL-2.0-or-later

Kernel driver aquacomputer-d5next
=================================

Supported devices:

* Aquacomputer Aquaero 5/6 fan controllers
* Aquacomputer D5 Next watercooling pump
* Aquacomputer Farbwerk RGB controller
* Aquacomputer Farbwerk 360 RGB controller
* Aquacomputer Octo fan controller
* Aquacomputer Quadro fan controller
* Aquacomputer High Flow Next sensor
* Aquacomputer Leakshield leak prevention system
* Aquacomputer Aquastream XT watercooling pump
* Aquacomputer Aquastream Ultimate watercooling pump
* Aquacomputer Poweradjust 3 fan controller
* Aquacomputer High Flow USB flow meter
* Aquacomputer MPS Flow devices

Author: Aleksa Savic

Description
-----------

This driver exposes hardware sensors of listed Aquacomputer devices, which
communicate through proprietary USB HID protocols.

The Aquaero devices expose eight physical, eight virtual and four calculated
virtual temperature sensors, as well as two flow sensors. The fans expose their
speed (in RPM), power, voltage and current. The four fans can also be controlled
directly, as well as configured as DC or PWM using pwm[1-4]_mode. Aquaero 6 supports
PWM for all four fans, while the Aquaero 5 supports it only for the fourth fan.
Temperature offsets can also be controlled.

Additionally, Aquaero devices also expose twenty temperature sensors and twelve flow
sensors from devices connected via Aquabus. The assigned sensor number is
predetermined by the Aquabus address of the device.

For the D5 Next pump, available sensors are pump and fan speed, power, voltage
and current, as well as coolant temperature and eight virtual temp sensors. Also
available through debugfs are the serial number, firmware version and power-on
count. Attaching a fan to it is optional and allows it to be controlled using
temperature curves directly from the pump. If it's not connected, the fan-related
sensors will report zeroes. The pump can be configured either through software or
via its physical interface.

The Octo exposes four physical and sixteen virtual temperature sensors, as well as
eight PWM controllable fans, along with their speed (in RPM), power, voltage and
current. Flow sensor pulses are also available.

The Quadro exposes four physical and sixteen virtual temperature sensors, a flow
sensor and four PWM controllable fans, along with their speed (in RPM), power,
voltage and current. Flow sensor pulses are also available.

The Farbwerk and Farbwerk 360 expose four temperature sensors. Additionally,
sixteen virtual temperature sensors of the Farbwerk 360 are exposed.

The High Flow Next exposes +5V voltages, water quality, conductivity and flow readings.
A temperature sensor can be connected to it, in which case it provides its reading
and an estimation of the dissipated/absorbed power in the liquid cooling loop.

The Leakshield exposes two temperature sensors and coolant pressure (current, min, max and
target readings). It also exposes the estimated reservoir volume and how much of it is
filled with coolant. Pump RPM and flow can be set to enhance on-device calculations.

The Aquastream XT pump exposes temperature readings for the coolant, external sensor
and fan IC. It also exposes pump and fan speeds (in RPM), voltages, as well as pump
current. Pump and fan speed can be controlled using PWM.

The Aquastream Ultimate pump exposes coolant temp and an external temp sensor, along
with speed, power, voltage and current of both the pump and optionally connected fan.
It also exposes pressure and flow speed readings.

The Poweradjust 3 controller exposes a single external temperature sensor.

The High Flow USB exposes an internal and external temperature sensor and a flow meter.

The MPS Flow devices expose the same entries as the High Flow USB because they have the
same USB product ID and report sensors equivalently.

Configuring listed devices through this driver is not implemented completely, as
some features include addressable RGB LEDs, for which there is no standard sysfs interface.
Thus, some tasks are better suited for userspace tools.

Depending on the device, not all sysfs and debugfs entries will be available.
Writing to virtual temperature sensors is not currently supported.

Usage notes
-----------

The devices communicate via HID reports. The driver is loaded automatically by
the kernel and supports hotswapping.

Configuring fan curves is available on the D5 Next, Quadro and Octo. Possible
pwm_enable values are:

====== ==========================================================
0      Set fan to 100%
1      Direct PWM mode (applies value in corresponding PWM entry)
2      PID control mode
3      Fan curve mode
[4-11] Follow fan[1-8], if available and device supports
====== ==========================================================

Sysfs entries
-------------

=============================== ====================================================================
temp[1-40]_input                Physical/virtual temperature sensors (in millidegrees Celsius)
temp[1-4]_offset                Temperature sensor correction offset (in millidegrees Celsius)
fan[1-20]_input                 Pump/fan speed (in RPM) / Flow speed (in dL/h)
fan[1-4]_min                    Minimal fan speed (in RPM)
fan[1-4]_max                    Maximal fan speed (in RPM)
fan1_target                     Target fan speed (in RPM)
fan5_pulses                     Quadro flow sensor pulses
fan9_pulses                     Octo flow sensor pulses
power[1-8]_input                Pump/fan power (in micro Watts)
in[0-7]_input                   Pump/fan voltage (in milli Volts)
curr[1-8]_input                 Pump/fan current (in milli Amperes)
pwm[1-8]                        Fan PWM (0 - 255)
pwm[1-8]_enable                 Fan control mode
pwm[1-8]_auto_channels_temp     Fan control temperature sensors select
pwm[1-4]_mode                   Fan mode (DC or PWM)
temp[1-8]_auto_point[1-16]_temp Temperature value of point on curve for given fan
temp[1-8]_auto_point[1-16]_pwm  PWM value of point on curve for given fan
curve[1-8]_power_min            Minimum curve power (curve scales to this)
curve[1-8]_power_max            Maximum curve power (curve scales to this)
curve[1-8]_power_fallback       Fallback power (if sensor/data is unavailable)
curve[1-8]_start_boost          Shortly run fan at 100% until firmware loads curve (0 - no, 1 - yes)
curve[1-8]_power_hold_min       Hold minimum power (0 - no, 1 - yes)
=============================== ====================================================================

Debugfs entries
---------------

================ =========================================================
serial_number    Serial number of the device
firmware_version Version of installed firmware
power_cycles     Count of how many times the device was powered on
hw_version       Hardware version/revision of device (Aquaero only)
current_uptime   Current power on device uptime (in seconds, Aquaero only)
total_uptime     Total device uptime (in seconds, Aquaero only)
================ =========================================================
