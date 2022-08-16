enum VirtType : u8 {
	Disabled = 0,
	Temperature = 3,
	Percentage = 5,
	Power = 7
};

be u16 virt_sensor_val[16] @ 0x1;
VirtType virt_sensor_type[16] @ 0x21;
be u16 checksum @ 0x41;
