# Aquacomputer protocols

Aquacomputer devices (usually) share the same HID report philosophy:

* A sensor report with ID `0x01` is sent every second to the host, detailing current sensor readings
* A HID feature report with ID `0x03` contains the device settings which can be requested, modified and written back to the device
* A control/configuration report that can be requested and sent back to the device, controlling its settings and mode of operation. Contains a CRC-16/USB checksum in the last two bytes
* A save report, which is always constant and is sent after a configuration report (the devices seem to work fine without it, but the official software always sends it)

These devices also share some substructures in their reports. All listed values are two bytes long and in big endian, unless noted otherwise. If something is unclear, refer to the code.

### Legacy devices?

Aquastream XT and Poweradjust 3 fall in this category. They are not _actually legacy_ (as in - obsolete and not being sold), they don't send the sensor report themselves and therefore
don't fully conform to the list of points above.

### Sensor report details & substructures

There's one important substructure that keeps recurring in sensor reports, and it concerns fan info. The definition of fan here also includes pumps, not only DC/PWM fans in the literal
sense. Here's what it's known to contain:

| What               | Where (relative offset) |
| ------------------ | ----------------------- |
| Fan speed (0-100%) | 0x00                    |
| Fan voltage        | 0x02                    |
| Fan current        | 0x04                    |
| Fan power          | 0x06                    |
| Fan speed (RPM)    | 0x08                    |

Temperature sensors, if not connected, will report `0x7FFF` as their value and are two bytes long.

### Control report details & substructures

The control report can be requested from and sent to the device using `GET_FEATURE_REPORT` and `SET_FEATURE_REPORT`, respectively. What it contains depends on the device, but it always
carries a two byte CRC-16/USB checksum in the last two places (unless it's an Aquaero). The checksum is calculated from the data between the starting `0x03` report ID at the very
beginning and the (existing) checksum at the end.

Fan speed control subgroups can be found in the control report, and it's currently known that they look like this:

| What             | Where (relative offset) |
| ---------------- | ----------------------- |
| Speed curve type | 0x00                    |
| Speed (0-100%)   | 0x01                    |

The `Speed curve type` above understands these values (list may be incomplete):

| Value  | Meaning                                                      |
| ------ | ------------------------------------------------------------ |
| 0      | Manual mode (directly honor percentage value)                |
| 1      | PID control mode                                             |
| 2      | Fan curve mode                                               |
| [3-10] | Follow behavior of fan [1-8], depending of its configured and device supports it |

## D5 Next pump

`0x0c70:0xf00e`

The D5 Next pump can, aside from itself, control and monitor an optionally connected fan.

### Sensor report

An example sensor report of the D5 Next looks like this:

```
01 00 03 0B B8 4E 20 00 01 00 00 00 64 03 FB 00 00 00 51 00 00 00 0E A4 00 00 00 45 00 10 42 C4 00 00 00 00 30 C9 00 00 00 A8 9C C2 B9 00 00 01 49 18 87 A1 3C 5C AB 04 BA 01 F8 00 00 00 52 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 00 00 00 00 00 00 00 00 0A 31 7F FF 00 00 7F FF 00 00 00 00 00 00 00 00 00 00 00 00 00 1B DC 04 B9 00 C5 00 EE 0B A8 00 00 00 1B DC 09 AA 08 4C 09 A9 08 4A 00 03 00 06 00 00 00 00 00 00 00 00 00 00 00 00 01 08 E5 00 E7 27 10 27 10 7F 7F
```

Its ID is `0x01` and its length is `0x9e`.

Here is what it's currently known to contain:

| What                               | Where/starts at (offset) |
| ---------------------------------- | ------------------------ |
| Serial number (first part)         | 0x03                     |
| Serial number (second part)        | 0x05                     |
| Firmware version                   | 0xD                      |
| Number of power cycles *[4 bytes]* | 0x18                     |
| Liquid (water) temperature         | 0x57                     |
| Flow sensor                        | 0x59                     |
| Pump info substructure             | 0x74                     |
| Fan info substructure              | 0x67                     |
| +5V voltage                        | 0x39                     |
| +12V voltage                       | 0x37                     |
| Virtual temp sensor 1              | 0x3F                     |
| Virtual temp sensor 2              | 0x41                     |
| Virtual temp sensor 3              | 0x43                     |
| Virtual temp sensor 4              | 0x45                     |
| Virtual temp sensor 5              | 0x47                     |
| Virtual temp sensor 6              | 0x49                     |
| Virtual temp sensor 7              | 0x4B                     |
| Virtual temp sensor 8              | 0x4D                     |

### Control report

An example control report of the D5 Next looks like this:

```
03 00 03 1E 00 00 00 00 00 0A C0 00 7F FF 00 00 00 00 02 02 0E 10 0B B8 00 00 00 00 0A 00 01 00 0A 00 06 00 0A 00 0C 00 0A 00 00 00 00 00 00 01 01 F4 27 10 27 10 07 D0 00 00 00 27 10 27 10 13 88 02 07 D2 00 00 0C 80 01 F4 01 2C 00 00 00 64 00 1E 00 01 0A F0 0A 8C 0A FD 0B 4C 0B 9D 0B E9 0C 46 0C 9F 0C F3 0D 3C 0D A2 0D E5 0E 42 0E 8A 0E E6 0F 35 0F 70 00 00 00 00 00 00 02 D6 04 D6 06 D6 09 81 0A 01 0D AC 12 02 16 2D 17 AD 19 D8 1E AE 22 2E 23 2E 02 12 D3 00 00 0D 48 01 F4 01 2C 00 00 00 64 00 1E 00 01 0A F0 0A 8C 0A FA 0B 4C 0B A4 0C 00 0C 4F 0C A3 0D 11 0D 51 0D A6 0D FD 0E 56 0E 9E 0E EE 0F 20 10 82 00 00 00 8C 00 00 00 00 00 00 00 00 00 00 01 00 01 80 03 54 07 81 0A 81 0B 01 0C 81 0D D7 0E AC 03 E8 FF 00 00 00 00 0F 03 00 00 FF FF 0F 19 00 00 03 E8 01 64 00 00 03 E8 01 FF 00 32 00 64 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 0F 0F 08 00 00 FF FF 0F 19 00 00 03 E8 01 64 00 00 03 E8 01 FF 00 19 00 28 00 14 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 0F 03 E7 FF FF 00 FE FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 1E 0F 0B 00 00 FF FF 0F 19 00 00 03 E8 01 64 00 00 03 E8 01 FF 00 1E 00 28 00 01 00 06 00 50 00 00 00 00 00 00 00 00 00 00 00 00 00 00 02 FF 02 FF 01 FB FF FF 05 25 FF FF 00 C5 FF FF 03 F5 FF FF 05 F3 FF FF 00 2D 0F 04 00 06 FF FF 0F 19 00 00 03 E8 01 64 00 00 03 E8 01 FF 00 28 00 05 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 0F 00 00 FF FF 01 FD FF FF 03 FF FF FF 00 FA FF FF 01 CE 10 FF 00 3C 0F 04 00 06 FF FF 0F 19 00 00 03 E8 01 64 00 00 03 E8 01 FF 00 28 00 05 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 0F 00 FA FF FF 05 DC FF FF 01 C2 FF FF 00 00 FF FF 07 D0 10 FF 00 4B 0F 04 00 06 FF FF 0F 19 00 00 03 E8 01 64 00 00 03 E8 01 FF 00 28 00 05 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 0F 03 E8 FF FF 01 C2 FF FF 00 00 FF FF 00 64 FF FF 03 20 10 FF 01 00 06 03 00 00 FF FF 0F 19 00 00 03 E8 01 64 00 00 03 E8 01 FF 00 1E 00 64 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 01 00 06 00 00 00 FF FF 0F 19 00 00 03 E8 01 64 00 00 03 E8 01 64 00 1E 00 64 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF C0 04 01 C2 0F A0 01 10 FB
```

Its ID is `0x03` and its length is `0x329`.

Here is what it's currently known to contain:

| What                                                | Where/starts at (offset) |
| --------------------------------------------------- | ------------------------ |
| Pump speed control subgroup                         | 0x96                     |
| Fan speed control subgroup                          | 0x41                     |
| Fan curve "hold min power" and "start boost" - pump | Unused                   |
| Fan curve "hold min power" and "start boost" - fan  | 0x2F                     |
| Fan curve min power subgroup - pump                 | 0x39                     |
| Fan curve min power subgroup - fan                  | 0x30                     |
| Fan curve max power subgroup - pump                 | 0x3B                     |
| Fan curve max power subgroup - fan                  | 0x32                     |
| Fan curve fallback power subgroup - pump            | 0x3D                     |
| Fan curve fallback power subgroup - fan             | 0x34                     |

## Farbwerk 360 RGB controller

`0x0c70:0f010`

The Farbwerk 360 exposes four temperature sensors through its sensor report.

### Sensor report

An example sensor report of the Farbwerk 360 looks like this:

```
01 00 01 41 BB DE 92 03 E8 00 00 00 64 03 FE 00 00 00 11 00 00 00 09 D3 00 00 00 5E 00 08 A4 DD 00 00 00 24 BF E6 C0 34 A2 B4 FF D7 FF D5 FF D6 5A EC 0A 1F 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 01 FA 01 FB 00 07 00 00 00 03 00 00 00 0B 00 00 00 00 00 15 20 1A 00 00 00 00 00 00 00 00 27 10 27 10 27 10 27 10 27 10 03 E8 00 00 03 E8 00 00 03 E8 00 00 03 E8 00 00 00 00 00 00 00 00 00 00 00 06 00 06 00 05 00 06 01 17 00 06
```

Its ID is `0x01` and its length is `0xb6`.

Here is what it's currently known to contain:

| What                   | Where/starts at (offset) |
| ---------------------- | ------------------------ |
| Temp sensor 1          | 0x32                     |
| Temp sensor 2          | 0x34                     |
| Temp sensor 3          | 0x36                     |
| Temp sensor 4          | 0x38                     |
| Virtual temp sensor 1  | 0x3A                     |
| Virtual temp sensor 2  | 0x3C                     |
| Virtual temp sensor 3  | 0x3E                     |
| Virtual temp sensor 4  | 0x40                     |
| Virtual temp sensor 5  | 0x42                     |
| Virtual temp sensor 6  | 0x44                     |
| Virtual temp sensor 7  | 0x46                     |
| Virtual temp sensor 8  | 0x48                     |
| Virtual temp sensor 9  | 0x4A                     |
| Virtual temp sensor 10 | 0x4C                     |
| Virtual temp sensor 11 | 0x4E                     |
| Virtual temp sensor 12 | 0x50                     |
| Virtual temp sensor 13 | 0x52                     |
| Virtual temp sensor 14 | 0x54                     |
| Virtual temp sensor 15 | 0x56                     |
| Virtual temp sensor 16 | 0x58                     |

## Octo

`0x0c70:0xf011`

The Octo exposes four physical and sixteen virtual temperature sensors and eight groups of fan sensor data (outlined in the preamble) through its sensor report.

### Sensor report

An example sensor report of the Octo looks like this:

```
01 00 02 3A 92 C9 EA 03 E8 00 01 00 65 03 FB 00 00 00 01 00 00 00 48 8E 00 00 00 C2 00 3A DF 11 01 00 01 00 00 00 00 00 00 00 00 00 00 00 00 00 04 9D 84 FF DB FF DC FF DD A7 B0 5B FC 10 17 7F FF 7F FF 7F FF 0F 00 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 03 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 04 B9 00 02 00 02 00 00 05 5D 04 B9 00 00 00 00 00 00 00 00 08 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 21 3B 04 B9 00 02 00 02 00 00 00 00 08 00 00 00 00 03 E8 05 5D 00 00 00 00 03 E8 00 00 00 00 00 00 03 E8 00 00 00 00 00 00 03 E8 00 00 00 00 00 00 03 E8 00 00 00 00 00 00 03 E8 00 00 00 00 00 00 03 E8 00 00 00 00 00 00 03 E8 21 3B 00 00 00 00 03 E8 27 10 00 00 00 00 03 E8 27 10 00 00 00 00 00 00 00 00 15 B3 15 0A 27 10 27 10 FF FF
```

Its ID is `0x01` and its length is `0x147`.

Here is what it's currently known to contain:

| What                               | Where/starts at (offset) |
| ---------------------------------- | ------------------------ |
| Serial number (first part)         | 0x03                     |
| Serial number (second part)        | 0x05                     |
| Firmware version                   | 0xD                      |
| Number of power cycles *[4 bytes]* | 0x18                     |
| Temp sensor 1                      | 0x3D                     |
| Temp sensor 2                      | 0x3F                     |
| Temp sensor 3                      | 0x41                     |
| Temp sensor 4                      | 0x43                     |
| Fan 1 substructure                 | 0x7D                     |
| Fan 2 substructure                 | 0x8A                     |
| Fan 3 substructure                 | 0x97                     |
| Fan 4 substructure                 | 0xA4                     |
| Fan 5 substructure                 | 0xB1                     |
| Fan 6 substructure                 | 0xBE                     |
| Fan 7 substructure                 | 0xCB                     |
| Fan 8 substructure                 | 0xD8                     |
| Flow sensor                        | 0x7B                     |
| Virtual temp sensor 1              | 0x45                     |
| Virtual temp sensor 2              | 0x47                     |
| Virtual temp sensor 3              | 0x49                     |
| Virtual temp sensor 4              | 0x4B                     |
| Virtual temp sensor 5              | 0x4D                     |
| Virtual temp sensor 6              | 0x4F                     |
| Virtual temp sensor 7              | 0x51                     |
| Virtual temp sensor 8              | 0x53                     |
| Virtual temp sensor 9              | 0x55                     |
| Virtual temp sensor 10             | 0x57                     |
| Virtual temp sensor 11             | 0x59                     |
| Virtual temp sensor 12             | 0x5B                     |
| Virtual temp sensor 13             | 0x5D                     |
| Virtual temp sensor 14             | 0x5F                     |
| Virtual temp sensor 15             | 0x61                     |
| Virtual temp sensor 16             | 0x63                     |

### Control report

An example control report of the Octo looks like this:

```
03 00 02 28 00 00 00 A9 00 00 05 14 02 BC 00 00 00 00 00 01 F4 27 10 27 10 07 D0 02 01 F4 27 10 27 10 07 D0 02 01 F4 27 10 27 10 07 D0 02 01 F4 27 10 27 10 07 D0 02 01 F4 27 10 27 10 07 D0 02 01 F4 27 10 27 10 07 D0 02 01 F4 27 10 27 10 07 D0 00 01 F4 27 10 27 10 07 D0 00 05 5D FF FF 0D AC 05 78 04 B0 00 00 00 28 00 14 00 01 0A F0 0A 8C 0A FA 0B 4A 0B A4 0B F4 0C 4E 0C 9D 0C F8 0D 48 0D A2 0D F2 0E 4C 0E 9C 0E F5 0F 46 0F A0 00 00 00 8C 01 18 01 F4 03 20 04 B0 06 90 08 D4 0B 68 0E 4C 11 94 15 2C 19 28 1D 74 22 10 27 10 00 00 00 FF FF 0D AC 05 78 04 B0 00 00 00 28 00 14 00 01 0A F0 0A 8C 0A FA 0B 4A 0B A4 0B F4 0C 4E 0C 9D 0C F8 0D 48 0D A2 0D F2 0E 4C 0E 9C 0E F5 0F 46 0F A0 00 00 00 8C 01 18 01 F4 03 20 04 B0 06 90 08 D4 0B 68 0E 4C 11 94 15 2C 19 28 1D 74 22 10 27 10 00 00 00 FF FF 0D AC 05 78 04 B0 00 00 00 28 00 14 00 01 0A F0 0A 8C 0A FA 0B 4A 0B A4 0B F4 0C 4E 0C 9D 0C F8 0D 48 0D A2 0D F2 0E 4C 0E 9C 0E F5 0F 46 0F A0 00 00 00 8C 01 18 01 F4 03 20 04 B0 06 90 08 D4 0B 68 0E 4C 11 94 15 2C 19 28 1D 74 22 10 27 10 00 00 00 FF FF 0D AC 05 78 04 B0 00 00 00 28 00 14 00 01 0A F0 0A 8C 0A FA 0B 4A 0B A4 0B F4 0C 4E 0C 9D 0C F8 0D 48 0D A2 0D F2 0E 4C 0E 9C 0E F5 0F 46 0F A0 00 00 00 8C 01 18 01 F4 03 20 04 B0 06 90 08 D4 0B 68 0E 4C 11 94 15 2C 19 28 1D 74 22 10 27 10 00 00 00 FF FF 0D AC 05 78 04 B0 00 00 00 28 00 14 00 01 0A F0 0A 8C 0A FA 0B 4A 0B A4 0B F4 0C 4E 0C 9D 0C F8 0D 48 0D A2 0D F2 0E 4C 0E 9C 0E F5 0F 46 0F A0 00 00 00 8C 01 18 01 F4 03 20 04 B0 06 90 08 D4 0B 68 0E 4C 11 94 15 2C 19 28 1D 74 22 10 27 10 00 00 00 FF FF 0D AC 05 78 04 B0 00 00 00 28 00 14 00 01 0A F0 0A 8C 0A FA 0B 4A 0B A4 0B F4 0C 4E 0C 9D 0C F8 0D 48 0D A2 0D F2 0E 4C 0E 9C 0E F5 0F 46 0F A0 00 00 00 8C 01 18 01 F4 03 20 04 B0 06 90 08 D4 0B 68 0E 4C 11 94 15 2C 19 28 1D 74 22 10 27 10 00 00 00 FF FF 0D AC 05 78 04 B0 00 00 00 28 00 14 00 01 0A F0 0A 8C 0A FA 0B 4A 0B A4 0B F4 0C 4E 0C 9D 0C F8 0D 48 0D A2 0D F2 0E 4C 0E 9C 0E F5 0F 46 0F A0 00 00 00 8C 01 18 01 F4 03 20 04 B0 06 90 08 D4 0B 68 0E 4C 11 94 15 2C 19 28 1D 74 22 10 27 10 00 21 3B FF FF 0D AC 05 78 04 B0 00 00 00 28 00 14 00 01 0A F0 0A 8C 0A FA 0B 4A 0B A4 0B F4 0C 4E 0C 9D 0C F8 0D 48 0D A2 0D F2 0E 4C 0E 9C 0E F5 0F 46 0F A0 00 00 00 8C 01 18 01 F4 03 20 04 B0 06 90 08 D4 0B 68 0E 4C 11 94 15 2C 19 28 1D 74 22 10 27 10 00 00 FF 00 00 00 00 0F 03 00 00 FF FF 0F 19 00 00 03 E8 01 64 00 00 03 E8 01 FF 00 32 00 64 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 0F 0F 08 00 00 FF FF 0F 19 00 00 03 E8 01 64 00 00 03 E8 01 FF 00 19 00 28 00 14 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 0F 03 E7 FF FF 00 FE FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 1E 0F 0B 00 00 FF FF 0F 19 00 00 03 E8 01 64 00 00 03 E8 01 FF 00 1E 00 28 00 01 00 06 00 50 00 00 00 00 00 00 00 00 00 00 00 00 00 00 02 FF 02 FF 01 FB FF FF 05 25 FF FF 00 C5 FF FF 03 F5 FF FF 05 F3 FF FF 00 2D 0F 13 00 00 FF FF 0F 19 00 00 03 E8 01 64 00 00 03 E8 01 FF 00 19 00 0A 00 05 00 05 00 19 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 FF 02 00 FF 78 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 3C 0F 04 00 06 FF FF 0F 19 00 00 03 E8 01 64 00 00 03 E8 01 FF 00 28 00 05 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 0F 00 00 FF FF 01 FD FF FF 03 FF FF FF 00 FA FF FF 01 CE 10 FF 00 4B 0F 0F 00 00 FF FF 0F 19 00 00 03 E8 01 64 00 00 03 E8 01 FF 00 28 00 04 00 1E 00 1E 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 78 00 78 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 01 00 0F 03 00 00 FF FF 0F 19 00 00 03 E8 01 64 00 00 03 E8 01 FF 00 32 00 64 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 01 0F 0F 08 00 00 FF FF 0F 19 00 00 03 E8 01 64 00 00 03 E8 01 FF 00 19 00 28 00 14 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 0F 03 E7 FF FF 00 FE FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 01 1E 0F 0B 00 00 FF FF 0F 19 00 00 03 E8 01 64 00 00 03 E8 01 FF 00 1E 00 28 00 01 00 06 00 50 00 00 00 00 00 00 00 00 00 00 00 00 00 00 02 FF 02 FF 01 FB FF FF 05 25 FF FF 00 C5 FF FF 03 F5 FF FF 05 F3 FF FF 01 2D 0F 13 00 00 FF FF 0F 19 00 00 03 E8 01 64 00 00 03 E8 01 FF 00 19 00 0A 00 05 00 05 00 19 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 FF 02 00 FF 78 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 01 3C 0F 04 00 06 FF FF 0F 19 00 00 03 E8 01 64 00 00 03 E8 01 FF 00 28 00 05 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 0F 00 00 FF FF 01 FD FF FF 03 FF FF FF 00 FA FF FF 01 CE 10 FF 01 4B 0F 0F 00 00 FF FF 0F 19 00 00 03 E8 01 64 00 00 03 E8 01 FF 00 28 00 04 00 1E 00 1E 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 78 00 78 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 01 00 00 13 88 13 88 13 88 13 88 01 5E 01 AB 59
```

Its ID is `0x03` and its length is `0x65F`.

Here is what it's currently known to contain:

| What                                         | Where/starts at (offset)                         |
| -------------------------------------------- | ------------------------------------------------ |
| Fan 1 ctrl substructure                      | 0x5A                                             |
| Fan 2 ctrl substructure                      | 0xAF                                             |
| Fan 3 ctrl substructure                      | 0x104                                            |
| Fan 4 ctrl substructure                      | 0x159                                            |
| Fan 5 ctrl substructure                      | 0x1AE                                            |
| Fan 6 ctrl substructure                      | 0x203                                            |
| Fan 7 ctrl substructure                      | 0x258                                            |
| Fan 8 ctrl substructure                      | 0x2AD                                            |
| Temp offset ctrl substructure                | 0xA                                              |
| Flow sensors pulses                          | 0x6                                              |
| Fan curve "hold min power" and "start boost" | {0x12, 0x1B, 0x24, 0x2D, 0x36, 0x3F, 0x48, 0x51} |
| Fan curve min power subgroup                 | {0x13, 0x1C, 0x25, 0x2E, 0x37, 0x40, 0x49, 0x52} |
| Fan curve max power subgroup                 | {0x15, 0x1E, 0x27, 0x30, 0x39, 0x42, 0x4B, 0x54} |
| Fan curve fallback power subgroup            | {0x17, 0x20, 0x29, 0x32, 0x3B, 0x44, 0x4D, 0x56} |

## Quadro

The Quadro exposes four physical and sixteen virtual temperature sensors, and four groups of fan sensor data (outlined in the preamble) through its sensor report. It also exposes a flow sensor.

### Sensor report

An example sensor report of the Quadro looks like this:

```
01 00 03 5B 72 FF 40 00 01 00 00 00 65 04 08 00 00 00 01 00 00 00 13 C5 00 00 00 91 00 32 CB B0 00 00 00 00 00 00 00 00 FF D5 FF D6 9B 54 FF D8 A6 FD 5B 97 7F FF 7F FF 06 51 7F FF 09 59 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 13 88 7F FF 7F FF 7F FF 03 00 00 00 00 00 00 00 00 00 00 00 03 00 00 00 04 B9 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 27 10 04 B9 00 00 00 00 00 00 00 00 08 05 BB 04 B9 00 00 00 00 01 64 00 00 00 15 E0 04 B9 00 00 00 00 00 00 00 00 08 00 00 00 00 03 E8 00 00 00 00 00 00 03 E8 27 10 00 00 00 00 03 E8 05 BB 00 00 00 00 03 E8 15 E0 00 00 00 00 03 E8 27 10 00 0A 00 00 00 0E 00 00 00 00 27 10 FF 00 00 01
```

Its ID is `0x01` and its length is `0xDC`.

Here is what it's currently known to contain:

| What                               | Where/starts at (offset) |
| ---------------------------------- | ------------------------ |
| Serial number (first part)         | 0x03                     |
| Serial number (second part)        | 0x05                     |
| Firmware version                   | 0xD                      |
| Number of power cycles *[4 bytes]* | 0x18                     |
| Temp sensor 1                      | 0x34                     |
| Temp sensor 2                      | 0x36                     |
| Temp sensor 3                      | 0x38                     |
| Temp sensor 4                      | 0x3A                     |
| Fan 1 substructure                 | 0x70                     |
| Fan 2 substructure                 | 0x7D                     |
| Fan 3 substructure                 | 0x8A                     |
| Fan 4 substructure                 | 0x97                     |
| Flow sensor                        | 0x6E                     |
| Virtual temp sensor 1              | 0x3C                     |
| Virtual temp sensor 2              | 0x3E                     |
| Virtual temp sensor 3              | 0x40                     |
| Virtual temp sensor 4              | 0x42                     |
| Virtual temp sensor 5              | 0x44                     |
| Virtual temp sensor 6              | 0x46                     |
| Virtual temp sensor 7              | 0x48                     |
| Virtual temp sensor 8              | 0x4A                     |
| Virtual temp sensor 9              | 0x4C                     |
| Virtual temp sensor 10             | 0x4E                     |
| Virtual temp sensor 11             | 0x50                     |
| Virtual temp sensor 12             | 0x52                     |
| Virtual temp sensor 13             | 0x54                     |
| Virtual temp sensor 14             | 0x56                     |
| Virtual temp sensor 15             | 0x58                     |
| Virtual temp sensor 16             | 0x5A                     |

### Control report

An example control report of the Quadro looks like this:

```
03 00 03 1C 00 00 00 A9 00 00 02 58 05 14 FA EC 05 DC 00 01 F4 27 10 27 10 07 D0 00 01 F4 27 10 27 10 07 D0 00 01 F4 27 10 27 10 07 D0 00 01 F4 27 10 27 10 07 D0 00 00 00 FF FF 0D AC 05 78 04 B0 00 00 00 28 00 14 00 01 0A F0 0A 8C 0A FA 0B 4A 0B A4 0B F4 0C 4E 0C 9D 0C F8 0D 48 0D A2 0D F2 0E 4C 0E 9C 0E F5 0F 46 0F A0 00 00 00 8C 01 18 01 F4 03 20 04 B0 06 90 08 D4 0B 68 0E 4C 11 94 15 2C 19 28 1D 74 22 10 27 10 00 4C D0 FF FF 0D AC 05 78 04 B0 00 00 00 28 00 14 00 01 0A F0 0A 8C 0A FA 0B 4A 0B A4 0B F4 0C 4E 0C 9D 0C F8 0D 48 0D A2 0D F2 0E 4C 0E 9C 0E F5 0F 46 0F A0 00 00 00 8C 01 18 01 F4 03 20 04 B0 06 90 08 D4 0B 68 0E 4C 11 94 15 2C 19 28 1D 74 22 10 27 10 00 05 BB 00 03 0D AC 05 78 04 B0 00 00 00 28 00 14 00 01 0A F0 0A 8C 0A FA 0B 4A 0B A4 0B F4 0C 4E 0C 9D 0C F8 0D 48 0D A2 0D F2 0E 4C 0E 9C 0E F5 0F 46 0F A0 00 00 00 8C 01 18 01 F4 03 20 04 B0 06 90 08 D4 0B 68 0E 4C 11 94 15 2C 19 28 1D 74 22 10 27 10 00 15 E0 FF FF 0D AC 05 78 04 B0 00 00 00 28 00 14 00 01 0A F0 0A 8C 0A FA 0B 4A 0B A4 0B F4 0C 4E 0C 9D 0C F8 0D 48 0D A2 0D F2 0E 4C 0E 9C 0E F5 0F 46 0F A0 00 00 00 8C 01 18 01 F4 03 20 04 B0 06 90 08 D4 0B 68 0E 4C 11 94 15 2C 19 28 1D 74 22 10 27 10 FF 00 02 00 00 0F 03 00 00 FF FF 0F 19 00 00 03 E8 01 64 00 00 03 E8 01 FF 00 32 00 64 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 0F 0F 08 00 00 FF FF 0F 19 00 00 03 E8 01 64 00 00 03 E8 01 FF 00 19 00 28 00 14 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 0F 03 E7 FF FF 00 FE FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 1E 0F 0B 00 00 FF FF 0F 19 00 00 03 E8 01 64 00 00 03 E8 01 FF 00 1E 00 28 00 01 00 06 00 50 00 00 00 00 00 00 00 00 00 00 00 00 00 00 02 FF 02 FF 01 FB FF FF 05 25 FF FF 00 C5 FF FF 03 F5 FF FF 05 F3 FF FF 00 2D 0F 04 00 06 FF FF 0F 19 00 00 03 E8 01 64 00 00 03 E8 01 FF 00 28 00 05 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 0F 00 00 FF FF 01 FD FF FF 03 FF FF FF 00 FA FF FF 01 CE 10 FF 00 3C 0F 04 00 06 FF FF 0F 19 00 00 03 E8 01 64 00 00 03 E8 01 FF 00 28 00 02 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 0F 03 FF FF FF 07 D0 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 4B 0F 04 00 06 FF FF 0F 19 00 00 03 E8 01 64 00 00 03 E8 01 FF 00 28 00 02 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 0F 01 CE FF FF 03 FF FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 2D 0F 00 00 06 FF FF 0F 19 00 00 03 E8 01 64 00 00 03 E8 01 64 00 28 00 02 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 0F 00 FA FF FF 01 CE 10 FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 2D 0F 00 00 06 FF FF 0F 19 00 00 03 E8 01 64 00 00 03 E8 01 64 00 28 00 05 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 0F 00 00 FF FF 01 FD FF FF 03 FF FF FF 00 FA FF FF 01 CE 10 FF 01 00 E0 A8```
```

Its ID is `0x03` and its length is `0x3C1`.

Here is what it's currently known to contain:

| What                                         | Where/starts at (offset) |
| -------------------------------------------- | ------------------------ |
| Fan 1 ctrl substructure                      | 0x36                     |
| Fan 2 ctrl substructure                      | 0x8B                     |
| Fan 3 ctrl substructure                      | 0xE0                     |
| Fan 4 ctrl substructure                      | 0x135                    |
| Temp offset ctrl substructure                | 0xA                      |
| Flow sensors pulses                          | 0x6                      |
| Fan curve "hold min power" and "start boost" | {0x12, 0x1B, 0x24, 0x2D} |
| Fan curve min power subgroup                 | {0x13, 0x1C, 0x25, 0x2E} |
| Fan curve max power subgroup                 | {0x15, 0x1E, 0x27, 0x30} |
| Fan curve fallback power subgroup            | {0x17, 0x20, 0x29, 0x32} |

## Highflow Next

`0x0c70:0xf012`

The High Flow Next exposes +5V voltages, water quality, conductivity and flow readings.
A temperature sensor can be connected to it, in which case it provides its reading
and an estimation of the dissipated/absorbed power in the liquid cooling loop.

### Sensor report

Here is what it's currently known to contain:

| What                 | Where/starts at (offset) |
| -------------------- | ------------------------ |
| Coolant temp         | 85                       |
| External sensor      | 87                       |
| Flow sensor          | 81                       |
| Water quality sensor | 89                       |
| Dissipated power     | 91                       |
| Conductivity         | 95                       |
| +5V voltage          | 97                       |
| +5V USB voltage      | 99                       |

## Leakshield

`0x0c70:0xf014`

The Leakshield exposes two temperature sensors and coolant pressure (current, min, max and
target readings). It also exposes the estimated reservoir volume and how much of it is
filled with coolant. Pump RPM and flow can be set to enhance on-device calculations.

### Sensor report

Here is what it's currently known to contain:

| What                     | Where/starts at (offset) |
| ------------------------ | ------------------------ |
| Pressure                 | 285                      |
| Temp sensor 1            | 265                      |
| Temp sensor 2            | 287                      |
| Pressure (min)           | 291                      |
| Pressure (target)        | 293                      |
| Pressure (max)           | 295                      |
| User-provided pump speed | 101                      |
| User-provided flow       | 111                      |
| Reservoir volume         | 313                      |
| Reservoir filled         | 311                      |

### Control report

Leakshield is unusual as it receives control data through pure USB, not HID. Consult the code for offsets regarding this one.

## Aquastream XT

`0x0c70:0xf0b6`

The Aquastream XT pump exposes temperature readings for the coolant, external sensor
and fan IC. It also exposes pump and fan speeds (in RPM), voltages, as well as pump
current. Pump and fan speed can be controlled using PWM.

### Sensor report

Size of this report is `0x42`.

Here is what it's currently known to contain:

| What               | Where/starts at (offset) |
| ------------------ | ------------------------ |
| Serial number      | 0x3a                     |
| Firmware version   | 0x32                     |
| Fan IC temp        | 0xD                      |
| External sensor    | 0xF                      |
| Coolant temp       | 0x11                     |
| Fan voltage        | 0x7                      |
| Fan status         | 0x1d                     |
| Pump voltage       | 0x9                      |
| Pump current       | 0xb                      |
| Fan 1 substructure | 0x13                     |
| Fan 2 substructure | 0x1B                     |


### Control report

Size of this report is `0x34`.

| What                    | Where/starts at (offset) |
| ----------------------- | ------------------------ |
| Pump mode ctrl offset   | 0x3                      |
| Fan mode ctrl offset    | 0x1a                     |
| Fan 1 ctrl substructure | 0x8                      |
| Fan 2 ctrl substructure | 0x1B                     |

## Aquastream Ultimate

`0x0c70:0xf00b`

The Aquastream Ultimate pump exposes coolant temp and an external temp sensor, along
with speed, power, voltage and current of both the pump and optionally connected fan.
It also exposes pressure and flow speed readings.

### Sensor report

Here is what it's currently known to contain:

| What             | Where/starts at (offset) |
| ---------------- | ------------------------ |
| Pump speed       | 0x51                     |
| Pump voltage     | 0x3D                     |
| Pump current     | 0x53                     |
| Pump power       | 0x55                     |
| Fan substructure | 0x41                     |
| Pressure         | 0x57                     |

Refer to the code for the pump and fan structures.

## Farbwerk

`0x0c70:0xf00a`

The Farbwerk exposes four temperature sensors.

### Sensor report

Here is what it's currently known to contain:

| What          | Where/starts at (offset) |
| ------------- | ------------------------ |
| Temp sensor 1 | 0x2F                     |
| Temp sensor 2 | 0x31                     |
| Temp sensor 3 | 0x33                     |
| Temp sensor 4 | 0x35                     |

## Poweradjust 3

`0x0c70:0xf0bd`

The Poweradjust 3 exposes a temperature sensor.

### Sensor report

Here is what it's currently known to contain:

| What            | Where/starts at (offset) |
| --------------- | ------------------------ |
| External sensor | 0x3                      |


## Aquaero 5/6

`0x0c70:0xf001`

The Aquaero devices expose eight physical, eight virtual and four calculated
virtual temperature sensors, as well as two flow sensors. The fans expose their
speed (in RPM), power, voltage and current. The four fans can also be
controlled directly, as well as configured as DC or PWM using pwm[1-4]_mode.
Temperature offsets can also be controlled.

Additionally, Aquaero devices also expose twenty temperature sensors and twelve flow
sensors from devices connected via Aquabus. The assigned sensor number is
predetermined by the Aquabus address of the device.

## Sensor report

Complex and still in R&D mode. Refer to the code.

## Control report

Complex and still in R&D mode. Refer to the code.
