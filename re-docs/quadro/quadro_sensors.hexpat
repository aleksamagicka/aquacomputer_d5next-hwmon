struct Fan {
 be u16 percent;
 be u16 voltage;
 be u16 current;
 be u16 power;
 be u16 speed;
 be u16 torque;
};

be u16 serial_number[2] @ 0x3;
be u16 firmware @ 0xd;
be u16 power_cycles @ 0x18;
be u16 temp_sensor[4] @ 0x34;
be u16 temp_sensor_virt[16] @ 0x3C;
be u16 vcc @ 0x6c;
be u16 flow @ 0x6e;
Fan fan1 @ 0x70;
Fan fan2 @ 0x7D;
Fan fan3 @ 0x8A;
Fan fan4 @ 0x97;
be u16 pwm[4] @ 0xA4;
u8 brightness @ 0xB8;



