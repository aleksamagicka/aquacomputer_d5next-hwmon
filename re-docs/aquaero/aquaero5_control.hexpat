bitfield Fan_flags {
	hold_min_power : 1;
	fuse : 1;
	start_boost : 1;
};

enum Control_mode : u8 {
power,
speed,
pwm
};

struct Fan {
be u16 min_rpm;
be u16 max_rpm;
be u16 min_power;
be u16 max_power;
be u16 start_boost_power;
be u16 start_boost_duration;
padding[2];
Fan_flags fan_flag;
Control_mode control_mode;
be u16 ctrl_source;
be u16 fuse_current;
};

struct Two_point_ctrl {
be u16 temp_src;
be u16 switch_on;
be u16 switch_off;
};

struct Set_point_ctrl {
be u16 temp_src;
be u16 temp;
be u16 P;
be u16 I;
be u16 D;
be u16 reset_time;
be u16 hysteresis;
padding[2];
};

struct Curve_ctrl {
be u16 temp_src;
be u16 start_temp;
be u16 temp[16];
be u16 percent[16];
};

be u16 temp_sensor_offset[8] @ 0xdb;
be u16 fan_temp_sensors_offset[4] @ 0x139;
Fan fan[4] @ 0x20c;
Two_point_ctrl two_point_ctrl[16] @ 0x4fc;
be u16 controller_preset_value_pwm[32] @ 0x55c;
Set_point_ctrl set_point[8] @ 0x59c;
Curve_ctrl curve_ctrl[4] @ 0x61c;
