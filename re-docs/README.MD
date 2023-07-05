# Reverse engineering docs for Aquacomputer devices

Here you'll find writeups of how the Aquacomputer devices communicate and the steps taken to understand them.

## PROTOCOLS.MD

The star of the show here is `PROTOCOLS.MD`. It contains short descriptions of capabilities of each device, examples of their HID reports
and explainers of currently known offsets in them. If it looks similar to liquidctl protocol docs for Aquacomputer devices, that is intentional.

## Captured reports

The subdirectories in this directory contain captured examples of captured HID reports (and possibly other behavior) of some of the devices.

Currently available:

* [Quadro (HID reports and structures)](quadro/readme.md)
* [Aquaero (HID reports and structures)](aquaero/readme.md)
* [Aquastream XT (HID reports and structures)](aquastreamxt/readme.md)
