struct Fan {
 be u16 speed;
 be u16 percent;
 be u16 voltage;
 be u16 current;
 be u16 power;
 be u16 torque;
};

be u16 serial[2] @ 0x07;
be u16 firmware @ 0xb;
be u16 bootloader_version @ 0xd;
be u16 hw_version @ 0xf;
be u16 temp_sensor[8] @ 0x65;
be u16 soft_sensor[8] @ 0x85;
be u16 temp_fan_amp[4] @ 0xbd;
be u16 flow_sensor1 @ 0xf9;
be u16 flow_sensor2 @ 0xfb;
Fan fans[4] @ 0x167;
Fan virt_fan[8] @ 0x197;
be u16 led[3] @ 0x23f;
be u16 relay @ 0x245;
be u16 power_output[2] @ 0x247;
be u16 preset_val[32] @ 0x2df;
