# aquacomputer_d5next

A hwmon Linux kernel driver supporting the following Aquacomputer devices:

|    Device    | Supported since kernel |                Functionality                 |
| :----------: | :--------------------: | :------------------------------------------: |
|   D5 Next    |          5.15          |               Various sensors                |
| Farbwerk 360 |          5.18          |             Temperature sensors              |
|     Octo     |  Not yet in mainline   | Temperature and fan sensors, fan PWM control |

Being a standard `hwmon` driver, it provides readings via `sysfs`, which are easily accessible through `lm-sensors` as usual. Here's example output for the D5 Next pump, which supports reading coolant temperature, as well as speed, power, voltage and current of the pump and optionally attached fan:

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
```
