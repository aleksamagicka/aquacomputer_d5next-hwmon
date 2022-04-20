# D5 Next RE findings

## General info

Product ID is `0xf00e`.

## HID reports and structures

There are three types of HID reports that you'll encounter when working with this pump, which we've called status, configuration and save (yes, it's not perfect) reports. All listed values are two bytes long and in big endian, unless noted otherwise.

### Sensor report

The pump autonomously sends this report to the host every second. It contains current sensor info (see table below for details) and looks like this:

```
01 00 03 0B B8 4E 20 00 01 00 00 00 64 03 FB 00 00 00 51 00 00 00 0E A4 00 00 00 45 00 10 42 C4 00 00 00 00 30 C9 00 00 00 A8 9C C2 B9 00 00 01 49 18 87 A1 3C 5C AB 04 BA 01 F8 00 00 00 52 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 7F FF 00 00 00 00 00 00 00 00 0A 31 7F FF 00 00 7F FF 00 00 00 00 00 00 00 00 00 00 00 00 00 1B DC 04 B9 00 C5 00 EE 0B A8 00 00 00 1B DC 09 AA 08 4C 09 A9 08 4A 00 03 00 06 00 00 00 00 00 00 00 00 00 00 00 00 01 08 E5 00 E7 27 10 27 10 7F 7F
```

As you can clearly see, its ID is `0x01` and its length is `0x9e`. Here is what it's currently known to contain:

|               What               | Where (offset) |
| :------------------------------: | :------------: |
|    Serial number (first part)    |      0x03      |
|   Serial number (second part)    |      0x05      |
|         Firmware version         |      0xD       |
| Number of power cycles [4 bytes] |      0x18      |
|   Coolant (water) temperature    |      0x57      |
|        Pump speed (0-100)        |      0x74      |
|        Fan speed (0-100)         |      0x67      |
|            Pump power            |      0x72      |
|            Fan power             |      0x65      |
|           Pump voltage           |      0x6E      |
|           Fan voltage            |      0x61      |
|           +5V voltage            |      0x39      |
|           Pump current           |      0x70      |
|           Fan current            |      0x63      |

From the example above, you can infer that the serial number is `3000-20000`.

### Configuration report

The config report is much bigger in size. It's ID is `0x03`. As far as we know, it contains the complete configuration of the pump. What makes it interesting to this driver (and possibly other software) is that it can be used to set pump and fan speeds directly, as well as set up curves for them on the device.

Here are the interesting bits of this report:

|          What           | Where (offset) |
| :---------------------: | :------------: |
|   Pump speed (0-100)    |      0x97      |
|    Fan speed (0-100)    |      0x42      |
| Pump speed control mode |      0x96      |
| Fan speed control mode  |      0x41      |
|     Checksum value      |     0x327      |

Since the speeds are expressed from 0 to 100 (percent), the driver converts them to and from the PWM range of [0, 255].

The checksum value is a bit peculiar, and it's calculated by taking the CRC (CRC-16/USB variant, to be precise) of the whole report - from `0x01` to `0x326`. To update the configuration of the pump, you always need to recalculate the checksum and place it into the last two bytes of the report. Otherwise, obviously, the pump will reject the new settings.

The `Pump speed control mode` in the table above refers to the mode that dictates how the pump will determine its speed - either via direct value (`0x00`), temperature curve (`0x01`), a curve based on a parameter (`0x02`) or based on flow (`0x03`).

`Fan speed control mode` works similarly - `0x00` for direct value, `0x01` for temperature curve and `0x02` for a custom curve.

### Save report

The official software sends a report with an ID `0x02` after every config report. It's value is always constant as far as I've observed. This drivers imitates the official software and sends it as well. I'm not completely sure _what_ it does, but we need to stay consistent.

