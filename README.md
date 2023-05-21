# aquacomputer_d5next

_Hwmon Linux kernel driver for monitoring and configuring Aquacomputer PC watercooling devices_

[![CII Best Practices](https://bestpractices.coreinfrastructure.org/projects/6592/badge)](https://bestpractices.coreinfrastructure.org/projects/6592) [![checkpatch](https://github.com/aleksamagicka/aquacomputer_d5next-hwmon/actions/workflows/checkpatch.yaml/badge.svg)](https://github.com/aleksamagicka/aquacomputer_d5next-hwmon/actions/workflows/checkpatch.yaml) [![compile-driver](https://github.com/aleksamagicka/aquacomputer_d5next-hwmon/actions/workflows/compile-driver.yaml/badge.svg)](https://github.com/aleksamagicka/aquacomputer_d5next-hwmon/actions/workflows/compile-driver.yaml)

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

[leo@manjaro]$ sensors
quadro-hid-3-3
Adapter: HID adapter
Fan 1 voltage:     12.13 V  
Fan 2 voltage:     12.13 V  
Fan 3 voltage:     12.13 V  
Fan 4 voltage:     12.13 V  
Fan 1 speed:       643 RPM
Fan 2 speed:      1756 RPM
Fan 3 speed:       659 RPM
Fan 4 speed:       650 RPM
Flow speed [l/h]:   60 RPM
Sensor 1:          +33.9°C  
Sensor 2:          +25.3°C  
Sensor 3:          +37.6°C  
Sensor 4:          +33.3°C  
Fan 1 power:      290.00 mW 
Fan 2 power:        0.00 W  
Fan 3 power:      260.00 mW 
Fan 4 power:      260.00 mW 
Fan 1 current:     24.00 mA 
Fan 2 current:      0.00 A  
Fan 3 current:     22.00 mA 
Fan 4 current:     22.00 mA

...
```

## Overview

The following devices are supported by this driver, and from which kernel version (if applicable):

|     Device     | Supported since kernel |                        Functionality                         | Microcontroller |
| :------------: | :--------------------: | :----------------------------------------------------------: | :-------------: |
|    D5 Next     |          5.15          |        Various sensors, direct fan PWM control (6.0+)        |        ?        |
|  Farbwerk 360  |          5.18          |                     Temperature sensors                      |  MCF51JU128VHS  |
|    Farbwerk    |          5.19          |                     Temperature sensors                      |        ?        |
|      Octo      |          5.19          |     Temperature and fan sensors, direct fan PWM control      |  MCF51JU128VLH  |
|     Quadro     |          6.0           | Temperature, flow and fan sensors, direct fan PWM control, flow sensor pulses |  MCF51JU128VHS  |
| High Flow Next |          6.1           |                       Various sensors                        |        ?        |
|  Aquaero 5/6   |     6.3 (sensors), 6.4 (fan control), rest here     | Temperature sensors, fan sensors, direct fan PWM control, DC/PWM mode setting, flow sensors | MCF51JM128EVLK  |
|   Leakshield   |     6.5 (sensors), rest here     |  Various sensors and setting parameters for higher accuracy  |        ?        |
| Aquastream XT  |     6.4 (sensors), rest here     | Temperature sensors, voltage sensors, pump and fan speed control | ATMEGA8-16MU |
| Aquastream Ultimate  |     6.3     | Temperature sensors, pump and fan sensors, pressure and flow | ? |
| Poweradjust 3  |     6.3    | External temperature sensor | ? |

Microcontrollers are noted for general reference, as this driver only communicates through HID reports and does not interact with the device CPU & electronics directly.

Being a standard `hwmon` driver, it provides readings via `sysfs`, which are easily accessible through `lm-sensors` as usual.

## Repository contents

Only notable parts are listed:

* _aquacomputer_d5next.c_ - the driver itself
* [Reverse engineering docs](re-docs) - WIP, documents explaining how the devices communicate to help understand what
  the driver does

* [Kernel docs](docs) - driver documentation for the kernel

## Installation and usage

Ideally, you are on a recent kernel and your distro includes it. If that's the case, you should already have this driver
available! Refer to the table in the overview above to check.

If you're not, or your kernel does not have the driver support for your particular device, you can clone this repository
and compile the driver yourself.

First, clone the repository by running:

```commandline
git clone https://github.com/aleksamagicka/aquacomputer_d5next-hwmon.git
```

Then, compile it and insert it into the running kernel, replacing the existing instance (if needed):

```commandline
make dev
```

You can then try running `sensors` and your devices should be listed there. If the compilation fails, you probably have
an older, possibly LTS kernel that does not have the functionality that this driver uses. In that case, you can modify the driver, following what the compiler says, or upgrade to a newer kernel (see [#28][#28] for an example).


[#28]: https://github.com/aleksamagicka/aquacomputer_d5next-hwmon/issues/28

## Contributing

Contributions in form of reporting issues and sending bug fixes and new functionality are very welcome! Without contributors, this driver would never be as feature rich as it is today. Please use the issue tracker and PR functionality for any feedback and patches. Code contributions must follow the [Linux code style rules](https://www.kernel.org/doc/html/v4.10/process/coding-style.html).
