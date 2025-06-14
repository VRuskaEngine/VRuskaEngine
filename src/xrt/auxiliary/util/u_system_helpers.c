// Copyright 2022-2023, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Helpers for system objects like @ref xrt_system_devices.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup aux_util
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_prober.h"

#include "util/u_misc.h"
#include "util/u_device.h"
#include "util/u_logging.h"
#include "util/u_system_helpers.h"

#include <assert.h>
#include <limits.h>


/*
 *
 * Helper functions.
 *
 */

static int32_t
get_index_for_device(const struct xrt_system_devices *xsysd, const struct xrt_device *xdev)
{
	assert(xsysd->xdev_count <= ARRAY_SIZE(xsysd->xdevs));
	assert(xsysd->xdev_count < INT_MAX);

	if (xdev == NULL) {
		return -1;
	}

	for (int32_t i = 0; i < (int32_t)xsysd->xdev_count; i++) {
		if (xsysd->xdevs[i] == xdev) {
			return i;
		}
	}

	return -1;
}

static const char *
type_to_small_string(enum xrt_device_feature_type type)
{
	switch (type) {
	case XRT_DEVICE_FEATURE_HAND_TRACKING_LEFT: return "hand_tracking_left";
	case XRT_DEVICE_FEATURE_HAND_TRACKING_RIGHT: return "hand_tracking_right";
	case XRT_DEVICE_FEATURE_EYE_TRACKING: return "eye_tracking";
	default: return "invalid";
	}
}


/*
 *
 * Internal functions.
 *
 */

static void
destroy(struct xrt_system_devices *xsysd)
{
	u_system_devices_close(xsysd);
	free(xsysd);
}

static xrt_result_t
get_roles(struct xrt_system_devices *xsysd, struct xrt_system_roles *out_roles)
{
	struct u_system_devices_static *usysds = u_system_devices_static(xsysd);

	assert(usysds->cached.generation_id == 1);

	*out_roles = usysds->cached;

	return XRT_SUCCESS;
}

static xrt_result_t
feature_inc(struct xrt_system_devices *xsysd, enum xrt_device_feature_type type)
{
	struct u_system_devices_static *usysds = u_system_devices_static(xsysd);

	if (type >= XRT_DEVICE_FEATURE_MAX_ENUM) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}

	// If it wasn't zero nothing to do.
	if (!xrt_reference_inc_and_was_zero(&usysds->feature_use[type])) {
		return XRT_SUCCESS;
	}

	xrt_result_t xret;
	if (type == XRT_DEVICE_FEATURE_HAND_TRACKING_LEFT) {
		xret = xrt_device_begin_feature(xsysd->static_roles.hand_tracking.left, type);
	} else if (type == XRT_DEVICE_FEATURE_HAND_TRACKING_RIGHT) {
		xret = xrt_device_begin_feature(xsysd->static_roles.hand_tracking.right, type);
	} else if (type == XRT_DEVICE_FEATURE_EYE_TRACKING) {
		xret = xrt_device_begin_feature(xsysd->static_roles.eyes, type);
	} else {
		xret = XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	U_LOG_D("Device-feature %s in use", type_to_small_string(type));

	return XRT_SUCCESS;
}

static xrt_result_t
feature_dec(struct xrt_system_devices *xsysd, enum xrt_device_feature_type type)
{
	struct u_system_devices_static *usysds = u_system_devices_static(xsysd);

	if (type >= XRT_DEVICE_FEATURE_MAX_ENUM) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}

	// If it is not zero we are done.
	if (!xrt_reference_dec_and_is_zero(&usysds->feature_use[type])) {
		return XRT_SUCCESS;
	}

	xrt_result_t xret;
	if (type == XRT_DEVICE_FEATURE_HAND_TRACKING_LEFT) {
		xret = xrt_device_end_feature(xsysd->static_roles.hand_tracking.left, type);
	} else if (type == XRT_DEVICE_FEATURE_HAND_TRACKING_RIGHT) {
		xret = xrt_device_end_feature(xsysd->static_roles.hand_tracking.right, type);
	} else if (type == XRT_DEVICE_FEATURE_EYE_TRACKING) {
		xret = xrt_device_end_feature(xsysd->static_roles.eyes, type);
	} else {
		xret = XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	U_LOG_D("Device-feature %s no longer in use", type_to_small_string(type));

	return XRT_SUCCESS;
}


/*
 *
 * 'Exported' functions.
 *
 */

struct u_system_devices *
u_system_devices_allocate(void)
{
	struct u_system_devices *usysd = U_TYPED_CALLOC(struct u_system_devices);
	usysd->base.destroy = destroy;

	return usysd;
}

void
u_system_devices_close(struct xrt_system_devices *xsysd)
{
	struct u_system_devices *usysd = u_system_devices(xsysd);

	for (uint32_t i = 0; i < ARRAY_SIZE(usysd->base.xdevs); i++) {
		xrt_device_destroy(&usysd->base.xdevs[i]);
	}

	xrt_frame_context_destroy_nodes(&usysd->xfctx);
}



struct u_system_devices_static *
u_system_devices_static_allocate(void)
{
	struct u_system_devices_static *usysds = U_TYPED_CALLOC(struct u_system_devices_static);
	usysds->base.base.destroy = destroy;
	usysds->base.base.get_roles = get_roles;
	usysds->base.base.feature_inc = feature_inc;
	usysds->base.base.feature_dec = feature_dec;

	return usysds;
}

void
u_system_devices_static_finalize(struct u_system_devices_static *usysds,
                                 struct xrt_device *left,
                                 struct xrt_device *right)
{
	struct xrt_system_devices *xsysd = &usysds->base.base;
	int32_t left_index = get_index_for_device(xsysd, left);
	int32_t right_index = get_index_for_device(xsysd, right);

	U_LOG_D(
	    "Devices:"
	    "\n\t%i: %p"
	    "\n\t%i: %p",
	    left_index, (void *)left,    //
	    right_index, (void *)right); //

	// Consistency checking.
	assert(usysds->cached.generation_id == 0);
	assert(left_index < 0 || left != NULL);
	assert(left_index >= 0 || left == NULL);
	assert(right_index < 0 || right != NULL);
	assert(right_index >= 0 || right == NULL);

	// Completely clear the struct.
	usysds->cached = (struct xrt_system_roles)XRT_SYSTEM_ROLES_INIT;
	usysds->cached.generation_id = 1;
	usysds->cached.left = left_index;
	usysds->cached.right = right_index;
}


/*
 *
 * Generic system devices helper.
 *
 */

xrt_result_t
u_system_devices_create_from_prober(struct xrt_instance *xinst,
                                    struct xrt_session_event_sink *broadcast,
                                    struct xrt_system_devices **out_xsysd,
                                    struct xrt_space_overseer **out_xso)
{
	xrt_result_t xret;

	assert(out_xsysd != NULL);
	assert(*out_xsysd == NULL);


	/*
	 * Create the devices.
	 */

	struct xrt_prober *xp = NULL;
	xret = xrt_instance_get_prober(xinst, &xp);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	xret = xrt_prober_probe(xp);
	if (xret < 0) {
		return xret;
	}

	return xrt_prober_create_system(xp, broadcast, out_xsysd, out_xso);
}

struct xrt_device *
u_system_devices_get_ht_device(struct xrt_system_devices *xsysd, enum xrt_input_name name)
{
	for (uint32_t i = 0; i < xsysd->xdev_count; i++) {
		struct xrt_device *xdev = xsysd->xdevs[i];

		if (xdev == NULL || !xdev->supported.hand_tracking) {
			continue;
		}

		for (uint32_t j = 0; j < xdev->input_count; j++) {
			struct xrt_input *input = &xdev->inputs[j];

			if (input->name == name) {
				return xdev;
			}
		}
	}

	return NULL;
}
