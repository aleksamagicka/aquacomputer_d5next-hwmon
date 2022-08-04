struct Flow_sensor {
    be u16 ticks_per_liter;
    u16 correction_factor;
};

bitfield Fan_flags {
	hold_min_power : 1;
	start_boost : 1;
};

struct Fan_setup {
    Fan_flags flags;
    be u16 min_percent;
    be u16 max_percent;
    be u16 fallback;
    be u16 graph_rpm;
};

enum Fan_ctrl_mode : u8 {
    PWM,
    TEMP_TARGET,
    CURVE,
    FAN1,
    FAN2,
    FAN3,
    FAN4
};

struct TEMP_TARGET_mode {
    be u16 temp_target;
    be u16 P;
    be u16 I;
    be u16 D1;
    be u16 D2;
    be u16 hysteresis;
};

struct CURVE_mode {
    be u16 start_temp;
    be u16 temp[16];
    be u16 percent[16];
};

struct Fan {
    Fan_ctrl_mode mode;
    be u16 pwm;
    u16 temp_sensor;
    TEMP_TARGET_mode temp_target_vars;
    padding[2];
    CURVE_mode curve_mode_vars;
};


struct RGB {
    u8 brightness;
    padding[1];
    u8 rgb_off;

};

u8 aquabus @ 0x3;
Flow_sensor flow_sensor@ 0x6;
u16 temp_sensors[4] @ 0xa;
Fan_setup fan_setups[4] @ 0x12;
Fan fans[4] @ 0x36;
RGB rgb @ 0x18a;

u8 profile @ 0x3bd;
be u16 checksum @ 0x3bf;

