/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <csyn/csyn.h>
#include <csyn/csyn_codec.h>

#include <synapse/control_reader.h>
#include <synapse/sensors_reader.h>
#include <synapse/state_reader.h>
#include <synapse/topic_catalog.h>

/* csyn no longer ships a topic list: the application (here, the test)
 * declares the topics it carries and csyn resolves them at init. */
CSYN_TOPIC_DEFINE(manual, "manual", CSYN_DIR_RX, sizeof(synapse_topic_ManualControlData_t));
CSYN_TOPIC_DEFINE(imu, "imu", CSYN_DIR_RX, sizeof(synapse_topic_InertialSampleData_t));
CSYN_TOPIC_DEFINE(external_pose, "external_pose", CSYN_DIR_RX,
		  sizeof(synapse_topic_ExternalOdometryData_t));
CSYN_TOPIC_DEFINE(pwm, "pwm", CSYN_DIR_TX, sizeof(synapse_topic_PwmSignalOutputsData_t));
CSYN_TOPIC_DEFINE(health, "health", CSYN_DIR_TX, sizeof(synapse_topic_VehicleHealthData_t));
CSYN_TOPIC_DEFINE(att, "att", CSYN_DIR_TX, sizeof(synapse_topic_AttitudeEstimateData_t));
CSYN_TOPIC_DEFINE(att_sp, "att_sp", CSYN_DIR_TX, sizeof(synapse_topic_AttitudeCommandData_t));
CSYN_TOPIC_DEFINE(loop, "loop", CSYN_DIR_TX, sizeof(synapse_topic_ControlLoopMetricsData_t));

ZTEST(csyn_store, test_registry_resolves_catalog)
{
	zassert_true(csyn_topic_count() > 0U);
	zassert_is_null(csyn_topic_at(csyn_topic_count()));
	zassert_is_null(csyn_topic_find("no_such_topic"));
	zassert_is_null(csyn_topic_find(NULL));

	for (size_t i = 0U; i < csyn_topic_count(); i++) {
		struct csyn_topic *topic = csyn_topic_at(i);

		zassert_not_null(topic);
		zassert_not_null(topic->info, "%s missing catalog info", topic->key);
		zassert_equal(topic, csyn_topic_find(topic->key));
		zassert_equal(topic, csyn_topic_find(topic->info->name));
		zassert_equal(topic, csyn_topic_find(topic->info->key));
		zassert_equal(topic, csyn_topic_by_catalog_id(topic->info->id));
		if (topic->info->fixed_layout) {
			zassert_equal(topic->max_size, topic->info->payload_size,
				      "%s slot size disagrees with catalog", topic->key);
		}
	}
}

ZTEST(csyn_store, test_publish_copy_generation)
{
	struct csyn_topic *topic = csyn_topic_find("att");
	synapse_topic_AttitudeEstimateData_t sample = {.timestamp_us = 1234U};
	uint8_t oversize[sizeof(sample) + 1U];
	uint8_t copy_buf[sizeof(sample)];
	uint32_t generation = 0U;
	size_t len = 0U;

	zassert_not_null(topic);

	/* Nothing published yet: copy reports no sample. */
	zassert_equal(csyn_topic_generation(topic), 0U);
	zassert_false(csyn_topic_copy(topic, copy_buf, sizeof(copy_buf), &len, &generation));

	/* First publish is visible with generation 1. */
	zassert_true(csyn_topic_publish(topic, &sample, sizeof(sample)));
	zassert_equal(csyn_topic_generation(topic), 1U);
	zassert_true(csyn_topic_copy(topic, copy_buf, sizeof(copy_buf), &len, &generation));
	zassert_equal(len, sizeof(sample));
	zassert_equal(generation, 1U);
	zassert_mem_equal(copy_buf, &sample, sizeof(sample));

	/* A second publish alternates slots and bumps the generation. */
	sample.timestamp_us = 5678U;
	zassert_true(csyn_topic_publish(topic, &sample, sizeof(sample)));
	zassert_true(csyn_topic_copy(topic, copy_buf, sizeof(copy_buf), &len, &generation));
	zassert_equal(generation, 2U);
	zassert_mem_equal(copy_buf, &sample, sizeof(sample));

	/* Invalid publishes and undersized copy buffers are rejected. */
	zassert_false(csyn_topic_publish(topic, oversize, sizeof(oversize)));
	zassert_false(csyn_topic_publish(topic, &sample, 0U));
	zassert_false(csyn_topic_publish(NULL, &sample, sizeof(sample)));
	zassert_false(csyn_topic_copy(topic, copy_buf, 1U, &len, &generation));
}

ZTEST(csyn_codec, test_pwm_axis_mapping)
{
	zassert_equal(csyn_pwm_from_centered_axis(0.0f), 1500);
	zassert_equal(csyn_pwm_from_centered_axis(1.0f), 2000);
	zassert_equal(csyn_pwm_from_centered_axis(-1.0f), 1000);
	zassert_equal(csyn_pwm_from_centered_axis(2.0f), 2000);
	zassert_equal(csyn_pwm_from_centered_axis(-2.0f), 1000);

	zassert_equal(csyn_pwm_from_throttle_axis(0.0f), 1000);
	zassert_equal(csyn_pwm_from_throttle_axis(1.0f), 2000);
	zassert_equal(csyn_pwm_from_throttle_axis(-1.0f), 1000);
	zassert_equal(csyn_pwm_from_throttle_axis(2.0f), 2000);

	zassert_within(csyn_centered_axis_from_pwm(1750), 0.5f, 1e-6f);
	zassert_within(csyn_centered_axis_from_pwm(1000), -1.0f, 1e-6f);
	zassert_within(csyn_throttle_axis_from_pwm(1500), 0.5f, 1e-6f);
	zassert_within(csyn_throttle_axis_from_pwm(2000), 1.0f, 1e-6f);
}

ZTEST(csyn_codec, test_quat_euler_roundtrip)
{
	const float roll = 0.3f;
	const float pitch = -0.4f;
	const float yaw = 1.2f;
	synapse_types_Quaternionf_t quat;
	float roll_out;
	float pitch_out;
	float yaw_out;

	csyn_quatf_from_euler(roll, pitch, yaw, &quat);
	csyn_euler_from_quatf(&quat, &roll_out, &pitch_out, &yaw_out);

	zassert_within(roll_out, roll, 1e-4f);
	zassert_within(pitch_out, pitch, 1e-4f);
	zassert_within(yaw_out, yaw, 1e-4f);

	csyn_quatf_from_euler(0.0f, 0.0f, 0.0f, &quat);
	zassert_within(quat.w, 1.0f, 1e-6f);
	zassert_within(quat.x, 0.0f, 1e-6f);
	zassert_within(quat.y, 0.0f, 1e-6f);
	zassert_within(quat.z, 0.0f, 1e-6f);
}

ZTEST_SUITE(csyn_store, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(csyn_codec, NULL, NULL, NULL, NULL, NULL);
