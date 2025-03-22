# aquaero

Use [imhex](https://github.com/WerWolv/ImHex) with the following files.

## sensor report
The file aquaero5_sensors.bin contains a sensor report sent by the Aquaero 5. The corresponding structures and addresses are in aquaero5_sensors.hexpat.

## control report
The file aquaero5_control.bin contains a control report sent by aquasuite. The corresponding structures and addresses are in aquaero5_sensors.hexpat.

## fan control
The aquaero 5 and 6 use a quite complicated system to control the fans. Aquasuite allows connecting each fans control input to any control source (fixed pwm value, pid, temp curve). To simplify this for pwm_enable, each fan can only be connected to the control sources corresponding to its channel number. For example fan1 uses only the first static pwm and the first pid controller. The drawback of this approach is that fan following is currently not supported.
