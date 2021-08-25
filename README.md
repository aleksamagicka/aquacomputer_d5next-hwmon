# d5next-hwmon

*A hwmon Linux kernel driver for exposing sensors of the Aquacomputer D5 Next watercooling pump.*

Supports reading coolant temperature, as well as speed, power, voltage and current of the pump and optionally attached fan. Being a standard `hwmon` driver, it provides readings via `sysfs`, which are easily accessible through `lm-sensors` as usual:

```shell
[aleksa@fedora linux]$ sensors
d5next-hid-3-3
Adapter: HID adapter
Pump voltage:  12.11 V
Fan voltage:   12.13 V
+5V voltage:    5.05 V
Pump speed:   1971 RPM
Fan speed:     231 RPM
Coolant temp:  +29.1°C
Pump power:     2.73 W
Fan power:      0.00 W
Pump current: 226.00 mA
Fan current:    0.00 A
```

