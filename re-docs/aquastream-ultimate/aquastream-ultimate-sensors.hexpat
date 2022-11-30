struct Fan {
 be u16 current;
 be u16 voltage;
 be u16 power;
 be u16 speed;
 be u16 torque;
 be u16 percent;
};

be u16 serial[2] @ 0x3;
be u16 firmware @ 0xd;
be u16 temp[2] @ 0x2d;
be u16 flow @ 0x37;
be u16 pump_voltage @ 0x3d;
Fan fan @ 0x41;
be u16 pump_speed @ 0x51;
be u16 pump_current @ 0x53;
be u16 pump_power @ 0x55;
be u16 pressure @ 0x57;
