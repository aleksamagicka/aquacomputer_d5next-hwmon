le u16 temp_sensor[3] @ 0xd;
u8 fan_pwm @ 0x1f;
le u16 firmware @ 0x32;
le u16 serial_number @ 0x3a;
u8 device_key[6] @ 0x3c;

le u16 pump_voltage @ 0x9;
le u16 fan_voltage @ 0x7;
le u16 pump_curr @ 0xb;
le u16 pump_speed @ 0x13;
le u16 fan_speed @ 0x1b;
le u16 fan_status @ 0x1d;
