#ifndef CSYN_ZROS_H_
#define CSYN_ZROS_H_

#include <stdint.h>

#include <zros/zros_topic.h>

#include <csyn/csyn_types.h>
#include <synapse/control_reader.h>
#include <synapse/state_reader.h>

ZROS_TOPIC_DECLARE(manual_control, struct csyn_manual_control);
ZROS_TOPIC_DECLARE(mocap, struct csyn_mocap_rigid_body);
ZROS_TOPIC_DECLARE(external_odometry, synapse_topic_ExternalOdometryData_t);
ZROS_TOPIC_DECLARE(pwm_signal_outputs, synapse_topic_PwmSignalOutputsData_t);
ZROS_TOPIC_DECLARE(vehicle_health, synapse_topic_VehicleHealthData_t);
ZROS_TOPIC_DECLARE(attitude_estimate, synapse_topic_AttitudeEstimateData_t);
ZROS_TOPIC_DECLARE(attitude_command, synapse_topic_AttitudeCommandData_t);
ZROS_TOPIC_DECLARE(control_loop_metrics, synapse_topic_ControlLoopMetricsData_t);
ZROS_TOPIC_DECLARE(mission_progress, synapse_topic_MissionProgressData_t);
ZROS_TOPIC_DECLARE(local_position_command, synapse_topic_LocalPositionCommandData_t);
ZROS_TOPIC_DECLARE(vehicle_command, synapse_topic_VehicleCommandData_t);
ZROS_TOPIC_DECLARE(navigation_target, synapse_topic_NavigationTargetData_t);

uint32_t csyn_zros_generation(const struct zros_topic *topic);

#endif
