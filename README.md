# aquacomputer_d5next

## Overview

This is a hwmon Linux kernel driver supporting the following Aquacomputer devices:

|    Device    | Supported since kernel |                    Functionality                    | Microcontroller |
| :----------: | :--------------------: | :-------------------------------------------------: | :-------------: |
|   D5 Next    |          5.15          |                   Various sensors                   |        ?        |
| Farbwerk 360 |          5.18          |                 Temperature sensors                 |  MCF51JU128VHS  |
|   Farbwerk   |          5.19          |                 Temperature sensors                 |        ?        |
|     Octo     |          5.19          | Temperature and fan sensors, direct fan PWM control |  MCF51JU128VLH  |
|    Quadro    |     Available here     | Temperature and fan sensors, direct fan PWM control |  MCF51JU128VHS  |

The above table shows what devices this driver supports and starting from which kernel version, if applicable. Microcontrollers are noted for general reference, as this driver only communicates through HID reports and does not interact with the device CPU & electronics directly.

Being a standard `hwmon` driver, it provides readings via `sysfs`, which are easily accessible through `lm-sensors` as usual. Here's example output for some of the devices:

```shell
[aleksa@fedora linux]$ sensors
d5next-hid-3-3
Adapter: HID adapter
Pump voltage:  12.10 V
Fan voltage:   12.12 V
+5V voltage:    5.04 V
Pump speed:   1970 RPM
Fan speed:    1332 RPM
Coolant temp:  +25.2°C
Pump power:     2.73 W
Fan power:    420.00 mW
Pump current: 226.00 mA
Fan current:   35.00 mA

octo-hid-3-8
Adapter: HID adapter
Fan 1 voltage:   0.00 V  
Fan 2 voltage:   0.00 V  
Fan 3 voltage:   0.00 V  
Fan 4 voltage:   0.00 V  
Fan 5 voltage:   0.00 V  
Fan 6 voltage:   0.00 V  
Fan 7 voltage:   0.00 V  
Fan 8 voltage:  12.08 V  
Fan 1 speed:      0 RPM
Fan 2 speed:      0 RPM
Fan 3 speed:      0 RPM
Fan 4 speed:      0 RPM
Fan 5 speed:      0 RPM
Fan 6 speed:      0 RPM
Fan 7 speed:      0 RPM
Fan 8 speed:    354 RPM
Sensor 1:           N/A  
Sensor 2:           N/A  
Sensor 3:           N/A  
Sensor 4:       +31.9°C  
Fan 1 power:     0.00 W  
Fan 2 power:     0.00 W  
Fan 3 power:     0.00 W  
Fan 4 power:     0.00 W  
Fan 5 power:     0.00 W  
Fan 6 power:     0.00 W  
Fan 7 power:     0.00 W  
Fan 8 power:   100.00 mW 
Fan 1 current:   0.00 A  
Fan 2 current:   0.00 A  
Fan 3 current:   0.00 A  
Fan 4 current:   0.00 A  
Fan 5 current:   0.00 A  
Fan 6 current:   0.00 A  
Fan 7 current:   0.00 A  
Fan 8 current:   9.00 mA 
```

## Repository contents

Only notable parts are listed:

* _aquacomputer_d5next.c_ - the driver itself
* [Reverse engineering docs](re-docs) - WIP, documents explaining how the devices communicate to help understand what the driver does

* [Kernel docs](docs) - driver documentation for the kernel

## Installation and usage

Ideally, you are on a recent kernel and your distro includes it. If that's the case, you should already have this driver available! Refer to the table in the overview above to check.

If you're not, or your kernel does not have the driver support for your particular device, you can clone this repository and compile the driver yourself.

