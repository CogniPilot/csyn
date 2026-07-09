#ifndef CSYN_CODEC_H_
#define CSYN_CODEC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <csyn/csyn_types.h>
#include <synapse/control_reader.h>
#include <synapse/types_reader.h>

static inline float csyn_clampf(float value, float min_value, float max_value)
{
	if (value < min_value) {
		return min_value;
	}
	if (value > max_value) {
		return max_value;
	}
	return value;
}

/* Centered axes map [-1, 1] to [1000, 2000] us around 1500 us. */
static inline int32_t csyn_pwm_from_centered_axis(float value)
{
	return (int32_t)(1500.0f + (csyn_clampf(value, -1.0f, 1.0f) * 500.0f));
}

static inline float csyn_centered_axis_from_pwm(int32_t pwm)
{
	return ((float)pwm - 1500.0f) / 500.0f;
}

/* Throttle maps [0, 1] to [1000, 2000] us. */
static inline int32_t csyn_pwm_from_throttle_axis(float value)
{
	return (int32_t)(1000.0f + (csyn_clampf(value, 0.0f, 1.0f) * 1000.0f));
}

static inline float csyn_throttle_axis_from_pwm(int32_t pwm)
{
	return ((float)pwm - 1000.0f) / 1000.0f;
}

void csyn_quatf_from_euler(float roll, float pitch, float yaw, synapse_types_Quaternionf_t *quat);

void csyn_euler_from_quatf(const synapse_types_Quaternionf_t *quat, float *roll, float *pitch,
			   float *yaw);

bool csyn_decode_manual_control(const void *buf, size_t buf_size, csyn_rc_channels16_t *rc,
				bool *valid);

void csyn_pwm_outputs_from_rc(const csyn_rc_channels16_t *rc,
			      synapse_topic_PwmSignalOutputsData_t *outputs, int64_t timestamp_us);

#endif
