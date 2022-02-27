.. SPDX-License-Identifier: GPL-2.0-or-later

Kernel driver aquacomputer_d5next
=================================

Supported devices:

* Aquacomputer D5 Next watercooling pump

Authors: Aleksa Savic, Jack Doan

Description
-----------

This driver exposes hardware sensors of the Aquacomputer D5 Next watercooling
pump. Available sensors are pump and fan speed, power, voltage and current, as
well as coolant temperature. Also available through debugfs are the serial
number, firmware version and power-on count. Attaching a fan to the pump is
optional, so the entries regarding the fan port may report zero values.

Pump and fan speed can be controlled using PWM, or via the physical interface
of the pump. Configuring other aspects of the pump, such as RGB LEDs or RPM
curves is not supported as there is no standard sysfs interface for them (in 
a manner that the pump requires). The pump also requires sending it a complete
configuration for every change, so speed control is implemented in a way that
should preserve all other current settings.

Usage Notes
-----------

The pump communicates via HID reports. The driver is loaded automatically by
the kernel and supports hotswapping.

Sysfs entries
-------------

============            =============================================
temp1_input             Coolant temperature (in millidegrees Celsius)
fan1_input              Pump speed (in RPM)
fan2_input              Fan speed (in RPM)
power1_input            Pump power (in micro Watts)
power2_input            Fan power (in micro Watts)
in0_input               Pump voltage (in milli Volts)
in1_input               Fan voltage (in milli Volts)
in2_input               +5V rail voltage (in milli Volts)
curr1_input             Pump current (in milli Amperes)
curr2_input             Fan current (in milli Amperes)
pwm1                    Pump speed setpoint (PWM)
pwm2                    Fan speed setpoint (PWM)
pwm*_enable             Used to set the control mode for the respective PWM entry.
                            * 0: manual control
                            * 1: PID control (not supported by this driver)
                            * 2: Curve-based automatic control (setpoint taken from temp1_input)
pwm1_auto_point*_pwm    Pump speed setting for the respective temperature point
pwm1_auto_point*_temp   Temperature point along the pump's control curve
pwm2_auto_point*_pwm    Fan speed setting for the respective temperature point
pwm2_auto_point*_temp   Temperature point along the fan's control curve
pwm*_auto_start_temp    The floor for temperature in the corresponding PWM curve
=====================   ========================================================

Debugfs entries
---------------

================ ===============================================
serial_number    Serial number of the pump
firmware_version Version of installed firmware
power_cycles     Count of how many times the pump was powered on
================ ===============================================
