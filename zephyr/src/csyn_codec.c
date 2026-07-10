/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <csyn/csyn_codec.h>

#include <math.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <synapse/state_reader.h>

BUILD_ASSERT(sizeof(synapse_topic_ManualControlData_t) == 40U);
BUILD_ASSERT(sizeof(synapse_topic_ExternalOdometryData_t) == 64U);
BUILD_ASSERT(sizeof(synapse_topic_PwmSignalOutputsData_t) == 48U);
BUILD_ASSERT(sizeof(csyn_rc_channels16_t) == 64U);
BUILD_ASSERT(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);

void csyn_quatf_from_euler(float roll, float pitch, float yaw, synapse_types_Quaternionf_t *quat)
{
	float cr = cosf(roll * 0.5f);
	float sr = sinf(roll * 0.5f);
	float cp = cosf(pitch * 0.5f);
	float sp = sinf(pitch * 0.5f);
	float cy = cosf(yaw * 0.5f);
	float sy = sinf(yaw * 0.5f);

	*quat = (synapse_types_Quaternionf_t){
		.w = (cr * cp * cy) + (sr * sp * sy),
		.x = (sr * cp * cy) - (cr * sp * sy),
		.y = (cr * sp * cy) + (sr * cp * sy),
		.z = (cr * cp * sy) - (sr * sp * cy),
	};
}

void csyn_euler_from_quatf(const synapse_types_Quaternionf_t *quat, float *roll, float *pitch,
			   float *yaw)
{
	float qw = quat->w;
	float qx = quat->x;
	float qy = quat->y;
	float qz = quat->z;
	float sinr_cosp = 2.0f * ((qw * qx) + (qy * qz));
	float cosr_cosp = 1.0f - (2.0f * ((qx * qx) + (qy * qy)));
	float sinp = 2.0f * ((qw * qy) - (qz * qx));
	float siny_cosp = 2.0f * ((qw * qz) + (qx * qy));
	float cosy_cosp = 1.0f - (2.0f * ((qy * qy) + (qz * qz)));

	*roll = atan2f(sinr_cosp, cosr_cosp);
	*pitch = asinf(csyn_clampf(sinp, -1.0f, 1.0f));
	*yaw = atan2f(siny_cosp, cosy_cosp);
}

/*
 * Compact per-rigid-body pose published by mocap bridges
 * (synapse_qualisys_bridge and the electrode ground station) on
 * `synapse/mocap/rigid_body/<name>/pose`: 7 little-endian f32 values
 * [px, py, pz, qx, qy, qz, qw] — ENU metres, quaternion scalar (w) LAST.
 */
#define CSYN_MOCAP_COMPACT_POSE_SIZE (7U * sizeof(float))

static bool csyn_decode_compact_pose(const uint8_t *buf, struct csyn_mocap_rigid_body *rb)
{
	float values[7];

	memcpy(values, buf, sizeof(values));
	for (size_t i = 0U; i < ARRAY_SIZE(values); i++) {
		if (!isfinite(values[i])) {
			return false;
		}
	}

	*rb = (struct csyn_mocap_rigid_body){
		.id = 0,
		.x = values[0],
		.y = values[1],
		.z = values[2],
		.qx = values[3],
		.qy = values[4],
		.qz = values[5],
		.qw = values[6],
		.valid = true,
	};

	return true;
}

/* Only the compact pose is decoded: the 0.5.0 MocapFrame table carries raw
 * rotation-matrix samples meant for logging, and estimators consume the pose
 * stream directly.
 */
bool csyn_decode_mocap_frame(const uint8_t *buf, size_t buf_size, struct csyn_mocap_rigid_body *rb)
{
	if (buf == NULL || rb == NULL || buf_size != CSYN_MOCAP_COMPACT_POSE_SIZE) {
		return false;
	}

	return csyn_decode_compact_pose(buf, rb);
}

static float milli_to_axis(int16_t value, float min_value, float max_value)
{
	return csyn_clampf((float)value / 1000.0f, min_value, max_value);
}

static bool manual_control_flag_set(const synapse_topic_ManualControlData_t *data,
				    synapse_topic_ManualControlFlags_enum_t flag)
{
	return (synapse_topic_ManualControlData_flags(data) & flag) != 0U;
}

bool csyn_decode_manual_control(const void *buf, size_t buf_size, csyn_rc_channels16_t *rc,
				bool *valid)
{
	const synapse_topic_ManualControlData_t *data = buf;
	uint16_t active_axes;
	uint16_t required_axes;
	uint8_t flight_mode;
	bool active_manual;

	if (buf == NULL || rc == NULL || valid == NULL ||
	    buf_size != sizeof(synapse_topic_ManualControlData_t)) {
		return false;
	}

	flight_mode = synapse_topic_ManualControlData_flight_mode(data);
	active_axes = synapse_topic_ManualControlData_active_axes(data);
	required_axes =
		synapse_topic_ManualControlAxes_Roll | synapse_topic_ManualControlAxes_Pitch |
		synapse_topic_ManualControlAxes_Throttle | synapse_topic_ManualControlAxes_Yaw;
	active_manual = manual_control_flag_set(data, synapse_topic_ManualControlFlags_Active);
	*valid = manual_control_flag_set(data, synapse_topic_ManualControlFlags_Valid) &&
		 ((active_axes & required_axes) == required_axes);

	*rc = (csyn_rc_channels16_t){
		.ch0 = csyn_pwm_from_centered_axis(milli_to_axis(
			synapse_topic_ManualControlData_roll_milli(data), -1.0f, 1.0f)),
		.ch1 = csyn_pwm_from_centered_axis(milli_to_axis(
			synapse_topic_ManualControlData_pitch_milli(data), -1.0f, 1.0f)),
		.ch2 = csyn_pwm_from_throttle_axis(milli_to_axis(
			synapse_topic_ManualControlData_throttle_milli(data), 0.0f, 1.0f)),
		.ch3 = csyn_pwm_from_centered_axis(-milli_to_axis(
			synapse_topic_ManualControlData_yaw_milli(data), -1.0f, 1.0f)),
		.ch4 = flight_mode > 0U ? 2000 : 1000,
		.ch5 = active_manual ? 1000 : 2000,
		.ch6 = manual_control_flag_set(data, synapse_topic_ManualControlFlags_ArmSwitch)
			       ? 2000
			       : 1000,
		.ch7 = manual_control_flag_set(data, synapse_topic_ManualControlFlags_KillSwitch)
			       ? 2000
			       : 1000,
	};

	return true;
}

static uint16_t pwm_to_u16(int32_t value)
{
	return (uint16_t)csyn_clampf((float)value, 0.0f, 65535.0f);
}

void csyn_pwm_outputs_from_rc(const csyn_rc_channels16_t *rc,
			      synapse_topic_PwmSignalOutputsData_t *outputs, int64_t timestamp_us)
{
	if (rc == NULL || outputs == NULL) {
		return;
	}

	*outputs = (synapse_topic_PwmSignalOutputsData_t){
		.timestamp_us = (uint64_t)MAX(timestamp_us, 0),
		.active_mask = 0xffffU,
		.port = 0U,
		.output0_us = pwm_to_u16(rc->ch0),
		.output1_us = pwm_to_u16(rc->ch1),
		.output2_us = pwm_to_u16(rc->ch2),
		.output3_us = pwm_to_u16(rc->ch3),
		.output4_us = pwm_to_u16(rc->ch4),
		.output5_us = pwm_to_u16(rc->ch5),
		.output6_us = pwm_to_u16(rc->ch6),
		.output7_us = pwm_to_u16(rc->ch7),
		.output8_us = pwm_to_u16(rc->ch8),
		.output9_us = pwm_to_u16(rc->ch9),
		.output10_us = pwm_to_u16(rc->ch10),
		.output11_us = pwm_to_u16(rc->ch11),
		.output12_us = pwm_to_u16(rc->ch12),
		.output13_us = pwm_to_u16(rc->ch13),
		.output14_us = pwm_to_u16(rc->ch14),
		.output15_us = pwm_to_u16(rc->ch15),
	};
}
