le u16 temp_sensor[2] @ 0x01;
le u16 fan_curr @ 0x05;
le u16 fan_voltage @ 0x7;
le u16 flow_speed @ 0x09;
le u16 fan_speed @ 0x0b;
u8 fan_pwm @ 0x0d;
le u16 firmware @ 0x21;
le u16 serial_number @ 0x27;
u8 device_key[6] @ 0x29;
