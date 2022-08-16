struct Fan {
 be u16 percent;
 be u16 voltage;
 be u16 current;
 be u16 power;
 be u16 speed;
 be u16 torque;
};

enum VirtType : u8 {
 Disabled = 0,
 Temperature = 3,
 Percentage = 5,
 Power = 7
};

be u16 serial_number[2] @ 0x3;
be u16 firmware @ 0xd;
be u32 uptime @ 0x14;
be u32 power_cycles @ 0x18;
be u16 temp_sensor[4] @ 0x34;
be u16 virt_sensor_val[16] @ 0x3C;
VirtType virt_sensor_type[16] @ 0x5C;
be u16 vcc @ 0x6c;
be u16 flow @ 0x6e;
Fan fan1 @ 0x70;
Fan fan2 @ 0x7D;
Fan fan3 @ 0x8A;
Fan fan4 @ 0x97;
be u16 pwm[4] @ 0xA4;
u8 brightness @ 0xB8;



