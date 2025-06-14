// Copyright 2013, Fredrik Hultin.
// Copyright 2013, Jakob Bornecrantz.
// Copyright 2015, Joey Ferwerda.
// Copyright 2020-2023, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Android sensors driver code.
 * @author Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
 * @ingroup drv_android
 */

#include "android_sensors.h"

#include "util/u_debug.h"
#include "util/u_device.h"
#include "util/u_distortion_mesh.h"
#include "util/u_var.h"
#include "util/u_visibility_mask.h"

#include "android/android_globals.h"
#include "android/android_custom_surface.h"

#include <xrt/xrt_config_android.h>

// Workaround to avoid the inclusion of "android_native_app_glue.h.
#ifndef LOOPER_ID_USER
#define LOOPER_ID_USER 3
#endif

// 60 events per second (in us).
#define POLL_RATE_USEC (1000L / 60) * 1000


DEBUG_GET_ONCE_LOG_OPTION(android_log, "ANDROID_SENSORS_LOG", U_LOGGING_WARN)

static inline struct android_device *
android_device(struct xrt_device *xdev)
{
	return (struct android_device *)xdev;
}

// Callback for the Android sensor event queue
static int
android_sensor_callback(ASensorEvent *event, struct android_device *d)
{
	struct xrt_vec3 gyro;
	struct xrt_vec3 accel;

	switch (event->type) {
	case ASENSOR_TYPE_ACCELEROMETER: {
		accel.x = event->acceleration.y;
		accel.y = -event->acceleration.x;
		accel.z = event->acceleration.z;

		ANDROID_TRACE(d, "accel %" PRId64 " %.2f %.2f %.2f", event->timestamp, accel.x, accel.y, accel.z);
		break;
	}
	case ASENSOR_TYPE_GYROSCOPE: {
		gyro.x = -event->data[1];
		gyro.y = event->data[0];
		gyro.z = event->data[2];

		ANDROID_TRACE(d, "gyro %" PRId64 " %.2f %.2f %.2f", event->timestamp, gyro.x, gyro.y, gyro.z);

		// TODO: Make filter handle accelerometer
		struct xrt_vec3 null_accel;

		// Lock last and the fusion.
		os_mutex_lock(&d->lock);

		m_imu_3dof_update(&d->fusion, event->timestamp, &null_accel, &gyro);

		// Now done.
		os_mutex_unlock(&d->lock);
	}
	default: ANDROID_TRACE(d, "Unhandled event type %d", event->type);
	}

	return 1;
}

static inline int32_t
android_get_sensor_poll_rate(const struct android_device *d)
{
	const float freq_multiplier = 1.0f / 3.0f;
	return (d == NULL) ? POLL_RATE_USEC
	                   : (int32_t)(d->base.hmd->screens[0].nominal_frame_interval_ns * freq_multiplier * 0.001f);
}

static void *
android_run_thread(void *ptr)
{
	struct android_device *d = (struct android_device *)ptr;
	const int32_t poll_rate_usec = android_get_sensor_poll_rate(d);
	// Maximum waiting time for sensor events.
	static const int max_wait_milliseconds = 100;
	ASensorManager *sensor_manager = NULL;
	const ASensor *accelerometer = NULL;
	const ASensor *gyroscope = NULL;
	ASensorEventQueue *event_queue = NULL;
#if __ANDROID_API__ >= 26
	sensor_manager = ASensorManager_getInstanceForPackage(XRT_ANDROID_PACKAGE);
#else
	sensor_manager = ASensorManager_getInstance();
#endif

	accelerometer = ASensorManager_getDefaultSensor(sensor_manager, ASENSOR_TYPE_ACCELEROMETER);
	gyroscope = ASensorManager_getDefaultSensor(sensor_manager, ASENSOR_TYPE_GYROSCOPE);

	ALooper *event_looper = ALooper_forThread();
	if (event_looper == NULL) {
		event_looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
		ANDROID_INFO(d,
		             "Created new event event_looper for "
		             "sensor capture thread.");
	}

	event_queue = ASensorManager_createEventQueue(sensor_manager, event_looper, LOOPER_ID_USER, NULL, (void *)d);


	/*
	 * Start sensors in case this was not done already.
	 *
	 * On some Android devices, such as Pixel 4 and Meizu 20 series, running
	 * apps was not smooth due to the failure in setting the sensor's event
	 * rate. This was caused by the calculated sensor's event rate based on
	 * the screen refresh rate, which could be smaller than the sensor's
	 * minimum delay value. Make sure to set it to a valid value.
	 */
	if (accelerometer != NULL) {
		int32_t accelerometer_min_delay = ASensor_getMinDelay(accelerometer);
		int32_t accelerometer_poll_rate_usec = MAX(poll_rate_usec, accelerometer_min_delay);

		ASensorEventQueue_enableSensor(event_queue, accelerometer);
		ASensorEventQueue_setEventRate(event_queue, accelerometer, accelerometer_poll_rate_usec);
	}
	if (gyroscope != NULL) {
		int32_t gyroscope_min_delay = ASensor_getMinDelay(gyroscope);
		int32_t gyroscope_poll_rate_usec = MAX(poll_rate_usec, gyroscope_min_delay);

		ASensorEventQueue_enableSensor(event_queue, gyroscope);
		ASensorEventQueue_setEventRate(event_queue, gyroscope, gyroscope_poll_rate_usec);
	}

	while (os_thread_helper_is_running(&d->oth)) {
		int num_events = 0;
		const int looper_id = ALooper_pollOnce(max_wait_milliseconds, NULL, &num_events, NULL);
		// The device may have enabled a power-saving policy, causing the sensor to sleep and return
		// ALOOPER_POLL_ERROR. However, we want to continue reading data when it wakes up.
		if (looper_id != LOOPER_ID_USER) {
			ANDROID_ERROR(d, "ALooper_pollAll failed with looper_id: %d", looper_id);
			continue;
		}
		if (num_events <= 0) {
			ANDROID_ERROR(d, "ALooper_pollAll returned zero events");
			continue;
		}
		// read event
		ASensorEvent event;
		while (ASensorEventQueue_getEvents(event_queue, &event, 1) > 0) {
			android_sensor_callback(&event, d);
		}
	}
	// Disable sensors.
	if (accelerometer != NULL) {
		ASensorEventQueue_disableSensor(event_queue, accelerometer);
	}
	if (gyroscope != NULL) {
		ASensorEventQueue_disableSensor(event_queue, gyroscope);
	}
	// Destroy the event queue.
	ASensorManager_destroyEventQueue(sensor_manager, event_queue);
	ANDROID_INFO(d, "android_run_thread exit");
	return NULL;
}


/*
 *
 * Device functions.
 *
 */

static void
android_device_destroy(struct xrt_device *xdev)
{
	struct android_device *android = android_device(xdev);

	// Destroy the thread object.
	os_thread_helper_destroy(&android->oth);

	// Now that the thread is not running we can destroy the lock.
	os_mutex_destroy(&android->lock);

	// Destroy the fusion.
	m_imu_3dof_close(&android->fusion);

	// Remove the variable tracking.
	u_var_remove_root(android);

	free(android);
}

static xrt_result_t
android_device_get_tracked_pose(struct xrt_device *xdev,
                                enum xrt_input_name name,
                                int64_t at_timestamp_ns,
                                struct xrt_space_relation *out_relation)
{
	(void)at_timestamp_ns;

	struct android_device *d = android_device(xdev);

	struct xrt_space_relation new_relation = XRT_SPACE_RELATION_ZERO;
	new_relation.pose.orientation = d->fusion.rot;

	//! @todo assuming that orientation is actually currently tracked.
	new_relation.relation_flags = (enum xrt_space_relation_flags)(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
	                                                              XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
	                                                              XRT_SPACE_RELATION_POSITION_VALID_BIT);

	*out_relation = new_relation;
	return XRT_SUCCESS;
}


/*
 *
 * Prober functions.
 *
 */

static bool
android_device_compute_distortion(
    struct xrt_device *xdev, uint32_t view, float u, float v, struct xrt_uv_triplet *result)
{
	struct android_device *d = android_device(xdev);
	return u_compute_distortion_cardboard(&d->cardboard.values[view], u, v, result);
}


struct android_device *
android_device_create(void)
{
	enum u_device_alloc_flags flags =
	    (enum u_device_alloc_flags)(U_DEVICE_ALLOC_HMD | U_DEVICE_ALLOC_TRACKING_NONE);
	struct android_device *d = U_DEVICE_ALLOCATE(struct android_device, flags, 1, 0);

	d->base.name = XRT_DEVICE_GENERIC_HMD;
	d->base.destroy = android_device_destroy;
	d->base.update_inputs = u_device_noop_update_inputs;
	d->base.get_tracked_pose = android_device_get_tracked_pose;
	d->base.get_view_poses = u_device_get_view_poses;
	d->base.get_visibility_mask = u_device_get_visibility_mask;
	d->base.compute_distortion = android_device_compute_distortion;
	d->base.inputs[0].name = XRT_INPUT_GENERIC_HEAD_POSE;
	d->base.device_type = XRT_DEVICE_TYPE_HMD;
	snprintf(d->base.str, XRT_DEVICE_NAME_LEN, "Android Sensors");
	snprintf(d->base.serial, XRT_DEVICE_NAME_LEN, "Android Sensors");

	d->log_level = debug_get_log_option_android_log();

	m_imu_3dof_init(&d->fusion, M_IMU_3DOF_USE_GRAVITY_DUR_20MS);

	int ret = os_mutex_init(&d->lock);
	if (ret != 0) {
		U_LOG_E("Failed to init mutex!");
		android_device_destroy(&d->base);
		return 0;
	}

	struct xrt_android_display_metrics metrics;
	if (!android_custom_surface_get_display_metrics(android_globals_get_vm(), android_globals_get_context(),
	                                                &metrics)) {
		U_LOG_E("Could not get Android display metrics.");
		/* Fallback to default values (Pixel 3) */
		metrics.width_pixels = 2960;
		metrics.height_pixels = 1440;
		metrics.density_dpi = 572;
		metrics.refresh_rate = 60.0f;
	}

	d->base.hmd->screens[0].nominal_frame_interval_ns = time_s_to_ns(1.0f / metrics.refresh_rate);

	// Everything done, finally start the thread.
	os_thread_helper_init(&d->oth);
	ret = os_thread_helper_start(&d->oth, android_run_thread, d);
	if (ret != 0) {
		ANDROID_ERROR(d, "Failed to start thread!");
		android_device_destroy(&d->base);
		return NULL;
	}

	const uint32_t w_pixels = metrics.width_pixels;
	const uint32_t h_pixels = metrics.height_pixels;

	const float angle = 45 * M_PI / 180.0; // 0.698132; // 40Deg in rads
	const float w_meters = ((float)w_pixels / (float)metrics.xdpi) * 0.0254f;
	const float h_meters = ((float)h_pixels / (float)metrics.ydpi) * 0.0254f;

	struct u_cardboard_distortion_arguments args = {
	    .distortion_k = {0.441f, 0.156f, 0.f, 0.f, 0.f},
	    .screen =
	        {
	            .w_pixels = w_pixels,
	            .h_pixels = h_pixels,
	            .w_meters = w_meters,
	            .h_meters = h_meters,
	        },
	    .inter_lens_distance_meters = 0.06f,
	    .screen_to_lens_distance_meters = 0.042f,
	    .tray_to_lens_distance_meters = 0.035f,
	    .fov =
	        {
	            .angle_left = -angle,
	            .angle_right = angle,
	            .angle_up = angle,
	            .angle_down = -angle,
	        },
	};

	u_distortion_cardboard_calculate(&args, d->base.hmd, &d->cardboard);


	u_var_add_root(d, "Android phone", true);
	u_var_add_ro_vec3_f32(d, &d->fusion.last.accel, "last.accel");
	u_var_add_ro_vec3_f32(d, &d->fusion.last.gyro, "last.gyro");

	d->base.supported.orientation_tracking = true;
	d->base.supported.position_tracking = false;

	// Distortion information.
	u_distortion_mesh_fill_in_compute(&d->base);

	ANDROID_DEBUG(d, "Created device!");

	return d;
}
