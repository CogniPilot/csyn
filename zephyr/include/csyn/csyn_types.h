#ifndef CSYN_TYPES_H_
#define CSYN_TYPES_H_

#include <stdbool.h>
#include <stdint.h>

struct csyn_rc_channels16 {
	int32_t ch0;
	int32_t ch1;
	int32_t ch2;
	int32_t ch3;
	int32_t ch4;
	int32_t ch5;
	int32_t ch6;
	int32_t ch7;
	int32_t ch8;
	int32_t ch9;
	int32_t ch10;
	int32_t ch11;
	int32_t ch12;
	int32_t ch13;
	int32_t ch14;
	int32_t ch15;
};

typedef struct csyn_rc_channels16 csyn_rc_channels16_t;

struct csyn_mocap_rigid_body {
	int32_t id;
	float x;
	float y;
	float z;
	float qw;
	float qx;
	float qy;
	float qz;
	bool valid;
};

typedef struct csyn_mocap_rigid_body csyn_mocap_rigid_body_t;

/* Wire-compatible with synapse_fbs 0.3.3 VehicleCommandData (removed in
 * 0.5.0); the electrode ground station still decodes this 40-byte layout
 * from the vehicle_command key.
 */
struct csyn_vehicle_command {
	uint64_t timestamp_us;
	float arg0;
	float arg1;
	float arg2;
	float arg3;
	float arg4;
	float arg5;
	float arg6;
	uint16_t command_id;
	uint8_t target_system;
	uint8_t target_component;
};

typedef struct csyn_vehicle_command csyn_vehicle_command_t;

struct csyn_manual_control {
	csyn_rc_channels16_t rc;
	bool valid;
	int64_t stamp_ms;
};

static inline const int32_t *csyn_rc_channels_data(const csyn_rc_channels16_t *rc)
{
	return &rc->ch0;
}

#endif
