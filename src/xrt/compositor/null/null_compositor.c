// Copyright 2019-2024, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Null compositor implementation.
 *
 * Originally based on src/xrt/compositor/main/comp_compositor.c
 *
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @ingroup comp_null
 */

#include "null_compositor.h"
#include "null_interfaces.h"

#include "os/os_time.h"

#include "util/u_misc.h"
#include "util/u_pacing.h"
#include "util/u_time.h"
#include "util/u_debug.h"
#include "util/u_verify.h"
#include "util/u_handles.h"
#include "util/u_trace_marker.h"

#include "util/comp_vulkan.h"

#include "multi/comp_multi_interface.h"
#include "xrt/xrt_compositor.h"
#include "xrt/xrt_device.h"


#include <stdint.h>
#include <stdio.h>

static const uint64_t RECOMMENDED_VIEW_WIDTH = 320;
static const uint64_t RECOMMENDED_VIEW_HEIGHT = 240;

static const uint64_t MAX_VIEW_WIDTH = 1920;
static const uint64_t MAX_VIEW_HEIGHT = 1080;

DEBUG_GET_ONCE_LOG_OPTION(log, "XRT_COMPOSITOR_LOG", U_LOGGING_INFO)


/*
 *
 * Helper functions.
 *
 */

static struct vk_bundle *
get_vk(struct null_compositor *c)
{
	return &c->base.vk;
}


/*
 *
 * Vulkan extensions.
 *
 */

static const char *instance_extensions_common[] = {
    VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME,      //
    VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,     //
    VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,  //
    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, //
};

static const char *required_device_extensions[] = {
    VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,      //
    VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME,            //
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,           //
    VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,        //
    VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, //

// Platform version of "external_memory"
#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_FD)
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,

#elif defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_AHARDWAREBUFFER)
    VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
    VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
    VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
    VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,

#elif defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_WIN32_HANDLE)
    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,

#else
#error "Need port!"
#endif

// Platform version of "external_fence" and "external_semaphore"
#if defined(XRT_GRAPHICS_SYNC_HANDLE_IS_FD) // Optional

#elif defined(XRT_GRAPHICS_SYNC_HANDLE_IS_WIN32_HANDLE)
    VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME, //
    VK_KHR_EXTERNAL_FENCE_WIN32_EXTENSION_NAME,     //

#else
#error "Need port!"
#endif
};

static const char *optional_device_extensions[] = {
    VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME, //
    VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME,   //

// Platform version of "external_fence" and "external_semaphore"
#if defined(XRT_GRAPHICS_SYNC_HANDLE_IS_FD)      // Optional
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, //
    VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,     //

#elif defined(XRT_GRAPHICS_SYNC_HANDLE_IS_WIN32_HANDLE) // Not optional

#else
#error "Need port!"
#endif

#ifdef VK_KHR_global_priority
    VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME,
#endif
#ifdef VK_KHR_image_format_list
    VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
#endif
#ifdef VK_KHR_maintenance1
    VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
#endif
#ifdef VK_KHR_maintenance2
    VK_KHR_MAINTENANCE_2_EXTENSION_NAME,
#endif
#ifdef VK_KHR_timeline_semaphore
    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
#endif
#ifdef VK_EXT_calibrated_timestamps
    VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,
#endif
#ifdef VK_EXT_robustness2
    VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,
#endif
};

static VkResult
select_instances_extensions(struct null_compositor *c, struct u_string_list *required, struct u_string_list *optional)
{
#ifdef VK_EXT_display_surface_counter
	u_string_list_append(optional, VK_EXT_DISPLAY_SURFACE_COUNTER_EXTENSION_NAME);
#endif

	return VK_SUCCESS;
}

static bool
compositor_init_vulkan(struct null_compositor *c)
{
	struct vk_bundle *vk = get_vk(c);
	VkResult ret;

	// every backend needs at least the common extensions
	struct u_string_list *required_instance_ext_list =
	    u_string_list_create_from_array(instance_extensions_common, ARRAY_SIZE(instance_extensions_common));

	struct u_string_list *optional_instance_ext_list = u_string_list_create();

	ret = select_instances_extensions(c, required_instance_ext_list, optional_instance_ext_list);
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "select_instances_extensions: %s\n\tFailed to select instance extensions.",
		         vk_result_string(ret));
		u_string_list_destroy(&required_instance_ext_list);
		u_string_list_destroy(&optional_instance_ext_list);
		return ret;
	}

	struct u_string_list *required_device_extension_list =
	    u_string_list_create_from_array(required_device_extensions, ARRAY_SIZE(required_device_extensions));

	struct u_string_list *optional_device_extension_list =
	    u_string_list_create_from_array(optional_device_extensions, ARRAY_SIZE(optional_device_extensions));

	struct comp_vulkan_arguments vk_args = {
	    .get_instance_proc_address = vkGetInstanceProcAddr,
	    .required_instance_version = VK_MAKE_VERSION(1, 0, 0),
	    .required_instance_extensions = required_instance_ext_list,
	    .optional_instance_extensions = optional_instance_ext_list,
	    .required_device_extensions = required_device_extension_list,
	    .optional_device_extensions = optional_device_extension_list,
	    .log_level = c->settings.log_level,
	    .only_compute_queue = false, // Regular GFX
	    .selected_gpu_index = -1,    // Auto
	    .client_gpu_index = -1,      // Auto
	    .timeline_semaphore = true,  // Flag is optional, not a hard requirement.
	};

	struct comp_vulkan_results vk_res = {0};
	bool bundle_ret = comp_vulkan_init_bundle(vk, &vk_args, &vk_res);

	u_string_list_destroy(&required_instance_ext_list);
	u_string_list_destroy(&optional_instance_ext_list);
	u_string_list_destroy(&required_device_extension_list);
	u_string_list_destroy(&optional_device_extension_list);

	if (!bundle_ret) {
		return false;
	}

	// clang-format off
	static_assert(ARRAY_SIZE(vk_res.client_gpu_deviceUUID.data) == XRT_UUID_SIZE, "array size mismatch");
	static_assert(ARRAY_SIZE(vk_res.selected_gpu_deviceUUID.data) == XRT_UUID_SIZE, "array size mismatch");
	static_assert(ARRAY_SIZE(vk_res.client_gpu_deviceUUID.data) == ARRAY_SIZE(c->sys_info.client_vk_deviceUUID.data), "array size mismatch");
	static_assert(ARRAY_SIZE(vk_res.selected_gpu_deviceUUID.data) == ARRAY_SIZE(c->sys_info.compositor_vk_deviceUUID.data), "array size mismatch");
	static_assert(ARRAY_SIZE(vk_res.client_gpu_deviceLUID.data) == XRT_LUID_SIZE, "array size mismatch");
	static_assert(ARRAY_SIZE(vk_res.client_gpu_deviceLUID.data) == ARRAY_SIZE(c->sys_info.client_d3d_deviceLUID.data), "array size mismatch");
	// clang-format on

	c->sys_info.client_vk_deviceUUID = vk_res.client_gpu_deviceUUID;
	c->sys_info.compositor_vk_deviceUUID = vk_res.selected_gpu_deviceUUID;
	c->sys_info.client_d3d_deviceLUID = vk_res.client_gpu_deviceLUID;
	c->sys_info.client_d3d_deviceLUID_valid = vk_res.client_gpu_deviceLUID_valid;

	// Tie the lifetimes of swapchains to Vulkan.
	xrt_result_t xret = comp_swapchain_shared_init(&c->base.cscs, vk);
	if (xret != XRT_SUCCESS) {
		return false;
	}

	return true;
}


/*
 *
 * Other init functions.
 *
 */

static bool
compositor_init_pacing(struct null_compositor *c)
{
	xrt_result_t xret = u_pc_fake_create(c->settings.frame_interval_ns, os_monotonic_get_ns(), &c->upc);
	if (xret != XRT_SUCCESS) {
		NULL_ERROR(c, "Failed to create fake pacing helper!");
		return false;
	}

	return true;
}

static bool
compositor_init_info(struct null_compositor *c)
{
	struct xrt_compositor_info *info = &c->base.base.base.info;

	struct comp_vulkan_formats formats = {0};
	comp_vulkan_formats_check(get_vk(c), &formats);
	comp_vulkan_formats_copy_to_info(&formats, info);
	comp_vulkan_formats_log(c->settings.log_level, &formats);

	return true;
}

static bool
compositor_init_sys_info(struct null_compositor *c, struct xrt_device *xdev)
{
	struct xrt_system_compositor_info *sys_info = &c->sys_info;

	// Required by OpenXR spec.
	sys_info->max_layers = XRT_MAX_LAYERS;

	// UUIDs and LUID already set in vk init.
	(void)sys_info->compositor_vk_deviceUUID;
	(void)sys_info->client_vk_deviceUUID;
	(void)sys_info->client_d3d_deviceLUID;
	(void)sys_info->client_d3d_deviceLUID_valid;
	uint32_t view_count = xdev->hmd->view_count;
	// clang-format off
	for (uint32_t i = 0; i < view_count; ++i) {
		sys_info->views[i].recommended.width_pixels  = RECOMMENDED_VIEW_WIDTH;
		sys_info->views[i].recommended.height_pixels = RECOMMENDED_VIEW_HEIGHT;
		sys_info->views[i].recommended.sample_count  = 1;
		sys_info->views[i].max.width_pixels  = MAX_VIEW_WIDTH;
		sys_info->views[i].max.height_pixels = MAX_VIEW_HEIGHT;
		sys_info->views[i].max.sample_count  = 1;
	}
	// clang-format on

	// Copy the list directly.
	assert(xdev->hmd->blend_mode_count <= XRT_MAX_DEVICE_BLEND_MODES);
	assert(xdev->hmd->blend_mode_count != 0);
	assert(xdev->hmd->blend_mode_count <= ARRAY_SIZE(sys_info->supported_blend_modes));
	for (size_t i = 0; i < xdev->hmd->blend_mode_count; ++i) {
		assert(u_verify_blend_mode_valid(xdev->hmd->blend_modes[i]));
		sys_info->supported_blend_modes[i] = xdev->hmd->blend_modes[i];
	}
	sys_info->supported_blend_mode_count = (uint8_t)xdev->hmd->blend_mode_count;

	// Refresh rates.
	sys_info->refresh_rate_count = 1;
	sys_info->refresh_rates_hz[0] = (float)(1. / time_ns_to_s(c->settings.frame_interval_ns));

	return true;
}


/*
 *
 * Member functions.
 *
 */

static xrt_result_t
null_compositor_begin_session(struct xrt_compositor *xc, const struct xrt_begin_session_info *type)
{
	struct null_compositor *c = null_compositor(xc);
	NULL_DEBUG(c, "BEGIN_SESSION");

	/*
	 * No logic needed here for the null compositor, if using the null
	 * compositor as a base for a new compositor put desired logic here.
	 */

	return XRT_SUCCESS;
}

static xrt_result_t
null_compositor_end_session(struct xrt_compositor *xc)
{
	struct null_compositor *c = null_compositor(xc);
	NULL_DEBUG(c, "END_SESSION");

	/*
	 * No logic needed here for the null compositor, if using the null
	 * compositor as a base for a new compositor put desired logic here.
	 */

	return XRT_SUCCESS;
}

static xrt_result_t
null_compositor_predict_frame(struct xrt_compositor *xc,
                              int64_t *out_frame_id,
                              int64_t *out_wake_time_ns,
                              int64_t *out_predicted_gpu_time_ns,
                              int64_t *out_predicted_display_time_ns,
                              int64_t *out_predicted_display_period_ns)
{
	COMP_TRACE_MARKER();

	struct null_compositor *c = null_compositor(xc);
	NULL_TRACE(c, "PREDICT_FRAME");

	int64_t now_ns = os_monotonic_get_ns();
	int64_t null_desired_present_time_ns = 0;
	int64_t null_present_slop_ns = 0;
	int64_t null_min_display_period_ns = 0;

	u_pc_predict(                        //
	    c->upc,                          // upc
	    now_ns,                          // now_ns
	    out_frame_id,                    // out_frame_id
	    out_wake_time_ns,                // out_wake_up_time_ns
	    &null_desired_present_time_ns,   // out_desired_present_time_ns
	    &null_present_slop_ns,           // out_present_slop_ns
	    out_predicted_display_time_ns,   // out_predicted_display_time_ns
	    out_predicted_display_period_ns, // out_predicted_display_period_ns
	    &null_min_display_period_ns);    // out_min_display_period_ns

	return XRT_SUCCESS;
}

static xrt_result_t
null_compositor_mark_frame(struct xrt_compositor *xc,
                           int64_t frame_id,
                           enum xrt_compositor_frame_point point,
                           int64_t when_ns)
{
	COMP_TRACE_MARKER();

	struct null_compositor *c = null_compositor(xc);
	NULL_TRACE(c, "MARK_FRAME %i", point);

	switch (point) {
	case XRT_COMPOSITOR_FRAME_POINT_WOKE:
		u_pc_mark_point(c->upc, U_TIMING_POINT_WAKE_UP, frame_id, when_ns);
		return XRT_SUCCESS;
	default: assert(false);
	}

	return XRT_SUCCESS;
}

static xrt_result_t
null_compositor_begin_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	struct null_compositor *c = null_compositor(xc);
	NULL_TRACE(c, "BEGIN_FRAME");

	/*
	 * No logic needed here for the null compositor, if using the null
	 * compositor as a base for a new compositor put desired logic here.
	 */

	return XRT_SUCCESS;
}

static xrt_result_t
null_compositor_discard_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	struct null_compositor *c = null_compositor(xc);
	NULL_TRACE(c, "DISCARD_FRAME");

	// Shouldn't be called.
	assert(false);

	return XRT_SUCCESS;
}

static xrt_result_t
null_compositor_layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	COMP_TRACE_MARKER();

	struct null_compositor *c = null_compositor(xc);
	NULL_TRACE(c, "LAYER_COMMIT");

	int64_t frame_id = c->base.layer_accum.data.frame_id;
	int64_t display_time_ns = c->base.layer_accum.layers[0].data.timestamp;

	// Default value from VRuska Engine, overridden by HMD device where possible.
	struct xrt_vec3 default_eye_relation = {0.063f, 0.f, 0.f};
	struct xrt_space_relation head_relation = {0};

	struct xrt_fov fovs[2] = {0};
	struct xrt_pose poses[2] = {0};
	xrt_device_get_view_poses(c->xdev, &default_eye_relation, display_time_ns, 2, &head_relation, fovs, poses);


	/*
	 * The null compositor doesn't render any frames, but needs to do
	 * minimal bookkeeping and handling of arguments. If using the null
	 * compositor as a base for a new compositor this is where you render
	 * frames to be displayed to devices or remote clients.
	 */

	// If you are using the system/multi-compositor (multiple client module), your native compositor
	// can just unref the sync handle. Otherwise please use it.
	u_graphics_sync_unref(&sync_handle);

	/*
	 * Time keeping needed to keep the pacer happy.
	 */

	// When we begin rendering.
	{
		int64_t now_ns = os_monotonic_get_ns();
		u_pc_mark_point(c->upc, U_TIMING_POINT_BEGIN, frame_id, now_ns);
	}

	// When we are submitting to the GPU.
	{
		int64_t now_ns = os_monotonic_get_ns();
		u_pc_mark_point(c->upc, U_TIMING_POINT_SUBMIT_BEGIN, frame_id, now_ns);

		now_ns = os_monotonic_get_ns();
		u_pc_mark_point(c->upc, U_TIMING_POINT_SUBMIT_END, frame_id, now_ns);
	}

	// Now is a good point to garbage collect.
	comp_swapchain_shared_garbage_collect(&c->base.cscs);

	return XRT_SUCCESS;
}

static void
null_compositor_destroy(struct xrt_compositor *xc)
{
	struct null_compositor *c = null_compositor(xc);
	struct vk_bundle *vk = get_vk(c);

	NULL_DEBUG(c, "NULL_COMP_DESTROY");

	// Make sure we don't have anything to destroy.
	comp_swapchain_shared_garbage_collect(&c->base.cscs);

	// Must be destroyed before Vulkan.
	comp_swapchain_shared_destroy(&c->base.cscs, vk);

	if (vk->device != VK_NULL_HANDLE) {
		vk->vkDestroyDevice(vk->device, NULL);
		vk->device = VK_NULL_HANDLE;
	}

	vk_deinit_mutex(vk);

	if (vk->instance != VK_NULL_HANDLE) {
		vk->vkDestroyInstance(vk->instance, NULL);
		vk->instance = VK_NULL_HANDLE;
	}

	comp_base_fini(&c->base);

	u_pc_destroy(&c->upc);

	free(c);
}

static xrt_result_t
null_compositor_get_display_refresh_rate(struct xrt_compositor *xc, float *out_display_refresh_rate_hz)
{
	struct null_compositor *c = null_compositor(xc);

	*out_display_refresh_rate_hz = c->sys_info.refresh_rates_hz[0];
	return XRT_SUCCESS;
}

static xrt_result_t
null_compositor_request_display_refresh_rate(struct xrt_compositor *xc, float display_refresh_rate_hz)
{
	return XRT_SUCCESS;
}

/*
 *
 * 'Exported' functions.
 *
 */

xrt_result_t
null_compositor_create_system(struct xrt_device *xdev, struct xrt_system_compositor **out_xsysc)
{
	struct null_compositor *c = U_TYPED_CALLOC(struct null_compositor);

	struct xrt_compositor *iface = &c->base.base.base;
	iface->begin_session = null_compositor_begin_session;
	iface->end_session = null_compositor_end_session;
	iface->predict_frame = null_compositor_predict_frame;
	iface->mark_frame = null_compositor_mark_frame;
	iface->begin_frame = null_compositor_begin_frame;
	iface->discard_frame = null_compositor_discard_frame;
	iface->layer_commit = null_compositor_layer_commit;
	iface->destroy = null_compositor_destroy;
	c->base.base.base.get_display_refresh_rate = null_compositor_get_display_refresh_rate;
	c->base.base.base.request_display_refresh_rate = null_compositor_request_display_refresh_rate;
	c->settings.log_level = debug_get_log_option_log();
	c->frame.waited.id = -1;
	c->frame.rendering.id = -1;
	c->settings.frame_interval_ns = U_TIME_1S_IN_NS / 20; // 20 FPS
	c->xdev = xdev;

	NULL_DEBUG(c, "Doing init %p", (void *)c);

	NULL_INFO(c,
	          "\n"
	          "################################################################################\n"
	          "# Null compositor starting, if you intended to use the null compositor (for CI #\n"
	          "# integration) then everything is mostly likely setup correctly. But if you    #\n"
	          "# intended to use VRuska Engine with real hardware it you probably built VRuska Engine       #\n"
	          "# without the main compositor, please check your build config and make sure    #\n"
	          "# that the main compositor is being built. Also make sure that the environment #\n"
	          "# variable XRT_COMPOSITOR_NULL is not set.                                     #\n"
	          "################################################################################");

	// Do this as early as possible
	comp_base_init(&c->base);


	/*
	 * Main init sequence.
	 */

	if (!compositor_init_pacing(c) ||         //
	    !compositor_init_vulkan(c) ||         //
	    !compositor_init_sys_info(c, xdev) || //
	    !compositor_init_info(c)) {           //
		NULL_DEBUG(c, "Failed to init compositor %p", (void *)c);
		c->base.base.base.destroy(&c->base.base.base);

		return XRT_ERROR_VULKAN;
	}


	NULL_DEBUG(c, "Done %p", (void *)c);

	// Standard app pacer.
	struct u_pacing_app_factory *upaf = NULL;
	XRT_MAYBE_UNUSED xrt_result_t xret = u_pa_factory_create(&upaf);
	assert(xret == XRT_SUCCESS && upaf != NULL);

	return comp_multi_create_system_compositor(&c->base.base, upaf, &c->sys_info, false, out_xsysc);
}
