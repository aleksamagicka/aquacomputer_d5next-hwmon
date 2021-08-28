# d5next-hwmon

*A hwmon Linux kernel driver for exposing sensors of the Aquacomputer D5 Next watercooling pump.*

Status: **[Accepted to hwmon-next](https://git.kernel.org/pub/scm/linux/kernel/git/groeck/linux-staging.git/commit/?h=hwmon-next&id=0e35f63f7f4eebd268ec236fd1bbf4e561ce8de5)**

Supports reading coolant temperature, as well as speed, power, voltage and current of the pump and optionally attached fan. Being a standard `hwmon` driver, it provides readings via `sysfs`, which are easily accessible through `lm-sensors` as usual:

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

Macro alignment using tabs does not always display correctly, such as here on Github. It's fine in my kernel Git repo, though.
