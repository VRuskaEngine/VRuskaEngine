// Copyright 2020-2024, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Handling functions called from generated dispatch function.
 * @author Pete Black <pblack@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup ipc_server
 */

#include "util/u_misc.h"
#include "util/u_handles.h"
#include "util/u_pretty_print.h"
#include "util/u_visibility_mask.h"
#include "util/u_trace_marker.h"

#include "server/ipc_server.h"
#include "ipc_server_generated.h"

#ifdef XRT_GRAPHICS_SYNC_HANDLE_IS_FD
#include <unistd.h>
#endif


/*
 *
 * Helper functions.
 *
 */

static xrt_result_t
validate_device_id(volatile struct ipc_client_state *ics, int64_t device_id, struct xrt_device **out_device)
{
	if (device_id >= XRT_SYSTEM_MAX_DEVICES) {
		IPC_ERROR(ics->server, "Invalid device ID (device_id >= XRT_SYSTEM_MAX_DEVICES)!");
		return XRT_ERROR_IPC_FAILURE;
	}

	struct xrt_device *xdev = ics->server->idevs[device_id].xdev;
	if (xdev == NULL) {
		IPC_ERROR(ics->server, "Invalid device ID (xdev is NULL)!");
		return XRT_ERROR_IPC_FAILURE;
	}

	*out_device = xdev;

	return XRT_SUCCESS;
}

static xrt_result_t
validate_origin_id(volatile struct ipc_client_state *ics, int64_t origin_id, struct xrt_tracking_origin **out_xtrack)
{
	if (origin_id >= XRT_SYSTEM_MAX_DEVICES) {
		IPC_ERROR(ics->server, "Invalid origin ID (origin_id >= XRT_SYSTEM_MAX_DEVICES)!");
		return XRT_ERROR_IPC_FAILURE;
	}

	struct xrt_tracking_origin *xtrack = ics->server->xtracks[origin_id];
	if (xtrack == NULL) {
		IPC_ERROR(ics->server, "Invalid origin ID (xtrack is NULL)!");
		return XRT_ERROR_IPC_FAILURE;
	}

	*out_xtrack = xtrack;

	return XRT_SUCCESS;
}

static xrt_result_t
validate_swapchain_state(volatile struct ipc_client_state *ics, uint32_t *out_index)
{
	// Our handle is just the index for now.
	uint32_t index = 0;
	for (; index < IPC_MAX_CLIENT_SWAPCHAINS; index++) {
		if (!ics->swapchain_data[index].active) {
			break;
		}
	}

	if (index >= IPC_MAX_CLIENT_SWAPCHAINS) {
		IPC_ERROR(ics->server, "Too many swapchains!");
		return XRT_ERROR_IPC_FAILURE;
	}

	*out_index = index;

	return XRT_SUCCESS;
}

static void
set_swapchain_info(volatile struct ipc_client_state *ics,
                   uint32_t index,
                   const struct xrt_swapchain_create_info *info,
                   struct xrt_swapchain *xsc)
{
	ics->xscs[index] = xsc;
	ics->swapchain_data[index].active = true;
	ics->swapchain_data[index].width = info->width;
	ics->swapchain_data[index].height = info->height;
	ics->swapchain_data[index].format = info->format;
	ics->swapchain_data[index].image_count = xsc->image_count;
}

static xrt_result_t
validate_reference_space_type(volatile struct ipc_client_state *ics, enum xrt_reference_space_type type)
{
	if ((uint32_t)type >= XRT_SPACE_REFERENCE_TYPE_COUNT) {
		IPC_ERROR(ics->server, "Invalid reference space type %u", type);
		return XRT_ERROR_IPC_FAILURE;
	}

	return XRT_SUCCESS;
}

static xrt_result_t
validate_device_feature_type(volatile struct ipc_client_state *ics, enum xrt_device_feature_type type)
{
	if ((uint32_t)type >= XRT_DEVICE_FEATURE_MAX_ENUM) {
		IPC_ERROR(ics->server, "Invalid device feature type %u", type);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}

	return XRT_SUCCESS;
}


static xrt_result_t
validate_space_id(volatile struct ipc_client_state *ics, int64_t space_id, struct xrt_space **out_xspc)
{
	if (space_id < 0) {
		return XRT_ERROR_IPC_FAILURE;
	}

	if (space_id >= IPC_MAX_CLIENT_SPACES) {
		return XRT_ERROR_IPC_FAILURE;
	}

	if (ics->xspcs[space_id] == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}

	*out_xspc = (struct xrt_space *)ics->xspcs[space_id];

	return XRT_SUCCESS;
}

static xrt_result_t
get_new_space_id(volatile struct ipc_client_state *ics, uint32_t *out_id)
{
	// Our handle is just the index for now.
	uint32_t index = 0;
	for (; index < IPC_MAX_CLIENT_SPACES; index++) {
		if (ics->xspcs[index] == NULL) {
			break;
		}
	}

	if (index >= IPC_MAX_CLIENT_SPACES) {
		IPC_ERROR(ics->server, "Too many spaces!");
		return XRT_ERROR_IPC_FAILURE;
	}

	*out_id = index;

	return XRT_SUCCESS;
}

static xrt_result_t
track_space(volatile struct ipc_client_state *ics, struct xrt_space *xs, uint32_t *out_id)
{
	uint32_t id = UINT32_MAX;
	xrt_result_t xret = get_new_space_id(ics, &id);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	// Remove volatile
	struct xrt_space **xs_ptr = (struct xrt_space **)&ics->xspcs[id];
	xrt_space_reference(xs_ptr, xs);

	*out_id = id;

	return XRT_SUCCESS;
}


static xrt_result_t
get_new_localspace_id(volatile struct ipc_client_state *ics, uint32_t *out_local_id, uint32_t *out_local_floor_id)
{
	// Our handle is just the index for now.
	uint32_t index = 0;
	for (; index < IPC_MAX_CLIENT_SPACES; index++) {
		if (ics->server->xso->localspace[index] == NULL) {
			break;
		}
	}

	if (index >= IPC_MAX_CLIENT_SPACES) {
		IPC_ERROR(ics->server, "Too many localspaces!");
		return XRT_ERROR_IPC_FAILURE;
	}

	ics->local_space_overseer_index = index;
	index = 0;
	for (; index < IPC_MAX_CLIENT_SPACES; index++) {
		if (ics->xspcs[index] == NULL) {
			break;
		}
	}

	if (index >= IPC_MAX_CLIENT_SPACES) {
		IPC_ERROR(ics->server, "Too many spaces!");
		return XRT_ERROR_IPC_FAILURE;
	}

	ics->local_space_index = index;
	*out_local_id = index;

	for (index = 0; index < IPC_MAX_CLIENT_SPACES; index++) {
		if (ics->server->xso->localfloorspace[index] == NULL) {
			break;
		}
	}

	if (index >= IPC_MAX_CLIENT_SPACES) {
		IPC_ERROR(ics->server, "Too many localfloorspaces!");
		return XRT_ERROR_IPC_FAILURE;
	}

	ics->local_floor_space_overseer_index = index;

	for (index = 0; index < IPC_MAX_CLIENT_SPACES; index++) {
		if (ics->xspcs[index] == NULL && index != ics->local_space_index) {
			break;
		}
	}

	if (index >= IPC_MAX_CLIENT_SPACES) {
		IPC_ERROR(ics->server, "Too many spaces!");
		return XRT_ERROR_IPC_FAILURE;
	}

	ics->local_floor_space_index = index;
	*out_local_floor_id = index;

	return XRT_SUCCESS;
}

static xrt_result_t
create_localspace(volatile struct ipc_client_state *ics, uint32_t *out_local_id, uint32_t *out_local_floor_id)
{
	uint32_t local_id = UINT32_MAX;
	uint32_t local_floor_id = UINT32_MAX;
	xrt_result_t xret = get_new_localspace_id(ics, &local_id, &local_floor_id);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	struct xrt_space_overseer *xso = ics->server->xso;
	struct xrt_space **xslocal_ptr = (struct xrt_space **)&ics->xspcs[local_id];
	struct xrt_space **xslocalfloor_ptr = (struct xrt_space **)&ics->xspcs[local_floor_id];

	xret = xrt_space_overseer_create_local_space(xso, &xso->localspace[ics->local_space_overseer_index],
	                                             &xso->localfloorspace[ics->local_floor_space_overseer_index]);
	if (xret != XRT_SUCCESS) {
		return xret;
	}
	xrt_space_reference(xslocal_ptr, xso->localspace[ics->local_space_overseer_index]);
	xrt_space_reference(xslocalfloor_ptr, xso->localfloorspace[ics->local_floor_space_overseer_index]);
	*out_local_id = local_id;
	*out_local_floor_id = local_floor_id;

	return XRT_SUCCESS;
}

/*
 *
 * Handle functions.
 *
 */

xrt_result_t
ipc_handle_instance_get_shm_fd(volatile struct ipc_client_state *ics,
                               uint32_t max_handle_capacity,
                               xrt_shmem_handle_t *out_handles,
                               uint32_t *out_handle_count)
{
	IPC_TRACE_MARKER();

	assert(max_handle_capacity >= 1);

	out_handles[0] = ics->server->ism_handle;
	*out_handle_count = 1;

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_instance_describe_client(volatile struct ipc_client_state *ics,
                                    const struct ipc_client_description *client_desc)
{
	ics->client_state.info = client_desc->info;
	ics->client_state.pid = client_desc->pid;

	struct u_pp_sink_stack_only sink;
	u_pp_delegate_t dg = u_pp_sink_stack_only_init(&sink);

#define P(...) u_pp(dg, __VA_ARGS__)
#define PNT(...) u_pp(dg, "\n\t" __VA_ARGS__)
#define PNTT(...) u_pp(dg, "\n\t\t" __VA_ARGS__)
#define EXT(NAME) PNTT(#NAME ": %s", client_desc->info.NAME ? "true" : "false")

	P("Client info:");
	PNT("id: %u", ics->client_state.id);
	PNT("application_name: '%s'", client_desc->info.application_name);
	PNT("pid: %i", client_desc->pid);
	PNT("extensions:");

	EXT(ext_hand_tracking_enabled);
	EXT(ext_eye_gaze_interaction_enabled);
	EXT(ext_hand_interaction_enabled);
#ifdef OXR_HAVE_HTC_facial_tracking
	EXT(htc_facial_tracking_enabled);
#endif
#ifdef OXR_HAVE_FB_body_tracking
	EXT(fb_body_tracking_enabled);
#endif
#ifdef OXR_HAVE_META_body_tracking_full_body
	EXT(meta_body_tracking_full_body_enabled);
#endif
#ifdef OXR_HAVE_FB_face_tracking2
	EXT(fb_face_tracking2_enabled);
#endif

#undef EXT
#undef PTT
#undef PT
#undef P

	// Log the pretty message.
	IPC_INFO(ics->server, "%s", sink.buffer);

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_system_compositor_get_info(volatile struct ipc_client_state *ics,
                                      struct xrt_system_compositor_info *out_info)
{
	IPC_TRACE_MARKER();

	*out_info = ics->server->xsysc->info;

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_session_create(volatile struct ipc_client_state *ics,
                          const struct xrt_session_info *xsi,
                          bool create_native_compositor)
{
	IPC_TRACE_MARKER();

	struct xrt_session *xs = NULL;
	struct xrt_compositor_native *xcn = NULL;

	if (ics->xs != NULL) {
		return XRT_ERROR_IPC_SESSION_ALREADY_CREATED;
	}

	if (!create_native_compositor) {
		IPC_INFO(ics->server, "App asked for headless session, creating native compositor anyways");
	}

	xrt_result_t xret = xrt_system_create_session(ics->server->xsys, xsi, &xs, &xcn);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	ics->client_state.session_overlay = xsi->is_overlay;
	ics->client_state.z_order = xsi->z_order;

	ics->xs = xs;
	ics->xc = &xcn->base;

	xrt_syscomp_set_state(ics->server->xsysc, ics->xc, ics->client_state.session_visible,
	                      ics->client_state.session_focused);
	xrt_syscomp_set_z_order(ics->server->xsysc, ics->xc, ics->client_state.z_order);

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_session_poll_events(volatile struct ipc_client_state *ics, union xrt_session_event *out_xse)
{
	// Have we created the session?
	if (ics->xs == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}

	return xrt_session_poll_events(ics->xs, out_xse);
}

xrt_result_t
ipc_handle_session_begin(volatile struct ipc_client_state *ics)
{
	IPC_TRACE_MARKER();

	// Have we created the session?
	if (ics->xs == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}

	// Need to check both because begin session is handled by compositor.
	if (ics->xc == NULL) {
		return XRT_ERROR_IPC_COMPOSITOR_NOT_CREATED;
	}

	//! @todo Pass the view type down.
	const struct xrt_begin_session_info begin_session_info = {
	    .view_type = XRT_VIEW_TYPE_STEREO,
	    .ext_hand_tracking_enabled = ics->client_state.info.ext_hand_tracking_enabled,
	    .ext_eye_gaze_interaction_enabled = ics->client_state.info.ext_eye_gaze_interaction_enabled,
	    .ext_hand_interaction_enabled = ics->client_state.info.ext_hand_interaction_enabled,
	    .htc_facial_tracking_enabled = ics->client_state.info.htc_facial_tracking_enabled,
	    .fb_body_tracking_enabled = ics->client_state.info.fb_body_tracking_enabled,
	    .fb_face_tracking2_enabled = ics->client_state.info.fb_face_tracking2_enabled,
	    .meta_body_tracking_full_body_enabled = ics->client_state.info.meta_body_tracking_full_body_enabled,
	};

	return xrt_comp_begin_session(ics->xc, &begin_session_info);
}

xrt_result_t
ipc_handle_session_end(volatile struct ipc_client_state *ics)
{
	IPC_TRACE_MARKER();

	// Have we created the session?
	if (ics->xs == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}

	// Need to check both because end session is handled by compositor.
	if (ics->xc == NULL) {
		return XRT_ERROR_IPC_COMPOSITOR_NOT_CREATED;
	}

	return xrt_comp_end_session(ics->xc);
}

xrt_result_t
ipc_handle_session_destroy(volatile struct ipc_client_state *ics)
{
	IPC_TRACE_MARKER();

	// Have we created the session?
	if (ics->xs == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}

	ipc_server_client_destroy_session_and_compositor(ics);

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_space_create_semantic_ids(volatile struct ipc_client_state *ics,
                                     uint32_t *out_root_id,
                                     uint32_t *out_view_id,
                                     uint32_t *out_local_id,
                                     uint32_t *out_local_floor_id,
                                     uint32_t *out_stage_id,
                                     uint32_t *out_unbounded_id)
{
	IPC_TRACE_MARKER();

	struct xrt_space_overseer *xso = ics->server->xso;

#define CREATE(NAME)                                                                                                   \
	do {                                                                                                           \
		*out_##NAME##_id = UINT32_MAX;                                                                         \
		if (xso->semantic.NAME == NULL) {                                                                      \
			break;                                                                                         \
		}                                                                                                      \
		uint32_t id = 0;                                                                                       \
		xrt_result_t xret = track_space(ics, xso->semantic.NAME, &id);                                         \
		if (xret != XRT_SUCCESS) {                                                                             \
			break;                                                                                         \
		}                                                                                                      \
		*out_##NAME##_id = id;                                                                                 \
	} while (false)

	CREATE(root);
	CREATE(view);
	CREATE(stage);
	CREATE(unbounded);

#undef CREATE
	return create_localspace(ics, out_local_id, out_local_floor_id);
}

xrt_result_t
ipc_handle_space_create_offset(volatile struct ipc_client_state *ics,
                               uint32_t parent_id,
                               const struct xrt_pose *offset,
                               uint32_t *out_space_id)
{
	IPC_TRACE_MARKER();

	struct xrt_space_overseer *xso = ics->server->xso;

	struct xrt_space *parent = NULL;
	xrt_result_t xret = validate_space_id(ics, parent_id, &parent);
	if (xret != XRT_SUCCESS) {
		return xret;
	}


	struct xrt_space *xs = NULL;
	xret = xrt_space_overseer_create_offset_space(xso, parent, offset, &xs);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	uint32_t space_id = UINT32_MAX;
	xret = track_space(ics, xs, &space_id);

	// Track space grabs a reference, or it errors and we don't want to keep it around.
	xrt_space_reference(&xs, NULL);

	if (xret != XRT_SUCCESS) {
		return xret;
	}

	*out_space_id = space_id;

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_space_create_pose(volatile struct ipc_client_state *ics,
                             uint32_t xdev_id,
                             enum xrt_input_name name,
                             uint32_t *out_space_id)
{
	IPC_TRACE_MARKER();

	struct xrt_space_overseer *xso = ics->server->xso;

	struct xrt_device *xdev = NULL;
	xrt_result_t xret = validate_device_id(ics, xdev_id, &xdev);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Invalid device_id!");
		return xret;
	}

	struct xrt_space *xs = NULL;
	xret = xrt_space_overseer_create_pose_space(xso, xdev, name, &xs);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	uint32_t space_id = UINT32_MAX;
	xret = track_space(ics, xs, &space_id);

	// Track space grabs a reference, or it errors and we don't want to keep it around.
	xrt_space_reference(&xs, NULL);

	if (xret != XRT_SUCCESS) {
		return xret;
	}

	*out_space_id = space_id;

	return xret;
}

xrt_result_t
ipc_handle_space_locate_space(volatile struct ipc_client_state *ics,
                              uint32_t base_space_id,
                              const struct xrt_pose *base_offset,
                              int64_t at_timestamp,
                              uint32_t space_id,
                              const struct xrt_pose *offset,
                              struct xrt_space_relation *out_relation)
{
	IPC_TRACE_MARKER();

	struct xrt_space_overseer *xso = ics->server->xso;
	struct xrt_space *base_space = NULL;
	struct xrt_space *space = NULL;
	xrt_result_t xret;

	xret = validate_space_id(ics, base_space_id, &base_space);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Invalid base_space_id!");
		return xret;
	}

	xret = validate_space_id(ics, space_id, &space);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Invalid space_id!");
		return xret;
	}

	return xrt_space_overseer_locate_space( //
	    xso,                                //
	    base_space,                         //
	    base_offset,                        //
	    at_timestamp,                       //
	    space,                              //
	    offset,                             //
	    out_relation);                      //
}

xrt_result_t
ipc_handle_space_locate_spaces(volatile struct ipc_client_state *ics,
                               uint32_t base_space_id,
                               const struct xrt_pose *base_offset,
                               uint32_t space_count,
                               int64_t at_timestamp)
{
	IPC_TRACE_MARKER();
	struct ipc_message_channel *imc = (struct ipc_message_channel *)&ics->imc;
	struct ipc_server *s = ics->server;

	struct xrt_space_overseer *xso = ics->server->xso;
	struct xrt_space *base_space = NULL;

	struct xrt_space **xspaces = U_TYPED_ARRAY_CALLOC(struct xrt_space *, space_count);
	struct xrt_pose *offsets = U_TYPED_ARRAY_CALLOC(struct xrt_pose, space_count);
	struct xrt_space_relation *out_relations = U_TYPED_ARRAY_CALLOC(struct xrt_space_relation, space_count);

	xrt_result_t xret;

	os_mutex_lock(&ics->server->global_state.lock);

	uint32_t *space_ids = U_TYPED_ARRAY_CALLOC(uint32_t, space_count);

	// we need to send back whether allocation succeeded so the client knows whether to send more data
	if (space_ids == NULL) {
		xret = XRT_ERROR_ALLOCATION;
	} else {
		xret = XRT_SUCCESS;
	}

	xret = ipc_send(imc, &xret, sizeof(enum xrt_result));
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(ics->server, "Failed to send spaces allocate result");
		// Nothing else we can do
		goto out_locate_spaces;
	}

	// only after sending the allocation result can we skip to the end in the allocation error case
	if (space_ids == NULL) {
		IPC_ERROR(s, "Failed to allocate space for receiving spaces ids");
		xret = XRT_ERROR_ALLOCATION;
		goto out_locate_spaces;
	}

	xret = ipc_receive(imc, space_ids, space_count * sizeof(uint32_t));
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(ics->server, "Failed to receive spaces ids");
		// assume early abort is possible, i.e. client will not send more data for this request
		goto out_locate_spaces;
	}

	xret = ipc_receive(imc, offsets, space_count * sizeof(struct xrt_pose));
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(ics->server, "Failed to receive spaces offsets");
		// assume early abort is possible, i.e. client will not send more data for this request
		goto out_locate_spaces;
	}

	xret = validate_space_id(ics, base_space_id, &base_space);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Invalid base_space_id %d!", base_space_id);
		// Client is receiving out_relations now, it will get xret on this receive.
		goto out_locate_spaces;
	}

	for (uint32_t i = 0; i < space_count; i++) {
		if (space_ids[i] == UINT32_MAX) {
			xspaces[i] = NULL;
		} else {
			xret = validate_space_id(ics, space_ids[i], &xspaces[i]);
			if (xret != XRT_SUCCESS) {
				U_LOG_E("Invalid space_id space_ids[%d] = %d!", i, space_ids[i]);
				// Client is receiving out_relations now, it will get xret on this receive.
				goto out_locate_spaces;
			}
		}
	}
	xret = xrt_space_overseer_locate_spaces( //
	    xso,                                 //
	    base_space,                          //
	    base_offset,                         //
	    at_timestamp,                        //
	    xspaces,                             //
	    space_count,                         //
	    offsets,                             //
	    out_relations);                      //

	xret = ipc_send(imc, out_relations, sizeof(struct xrt_space_relation) * space_count);
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(ics->server, "Failed to send spaces relations");
		// Nothing else we can do
		goto out_locate_spaces;
	}

out_locate_spaces:
	free(xspaces);
	free(offsets);
	free(out_relations);
	os_mutex_unlock(&ics->server->global_state.lock);
	return xret;
}

xrt_result_t
ipc_handle_space_locate_device(volatile struct ipc_client_state *ics,
                               uint32_t base_space_id,
                               const struct xrt_pose *base_offset,
                               int64_t at_timestamp,
                               uint32_t xdev_id,
                               struct xrt_space_relation *out_relation)
{
	IPC_TRACE_MARKER();

	struct xrt_space_overseer *xso = ics->server->xso;
	struct xrt_space *base_space = NULL;
	struct xrt_device *xdev = NULL;
	xrt_result_t xret;

	xret = validate_space_id(ics, base_space_id, &base_space);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Invalid base_space_id!");
		return xret;
	}

	xret = validate_device_id(ics, xdev_id, &xdev);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Invalid device_id!");
		return xret;
	}

	return xrt_space_overseer_locate_device( //
	    xso,                                 //
	    base_space,                          //
	    base_offset,                         //
	    at_timestamp,                        //
	    xdev,                                //
	    out_relation);                       //
}

xrt_result_t
ipc_handle_space_destroy(volatile struct ipc_client_state *ics, uint32_t space_id)
{
	struct xrt_space *xs = NULL;
	xrt_result_t xret;

	xret = validate_space_id(ics, space_id, &xs);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Invalid space_id!");
		return xret;
	}

	assert(xs != NULL);
	xs = NULL;

	// Remove volatile
	struct xrt_space **xs_ptr = (struct xrt_space **)&ics->xspcs[space_id];
	xrt_space_reference(xs_ptr, NULL);

	if (space_id == ics->local_space_index) {
		struct xrt_space **xslocal_ptr =
		    (struct xrt_space **)&ics->server->xso->localspace[ics->local_space_overseer_index];
		xrt_space_reference(xslocal_ptr, NULL);
	}

	if (space_id == ics->local_floor_space_index) {
		struct xrt_space **xslocalfloor_ptr =
		    (struct xrt_space **)&ics->server->xso->localfloorspace[ics->local_floor_space_overseer_index];
		xrt_space_reference(xslocalfloor_ptr, NULL);
	}

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_space_mark_ref_space_in_use(volatile struct ipc_client_state *ics, enum xrt_reference_space_type type)
{
	struct xrt_space_overseer *xso = ics->server->xso;
	xrt_result_t xret;

	xret = validate_reference_space_type(ics, type);
	if (xret != XRT_SUCCESS) {
		return XRT_ERROR_IPC_FAILURE;
	}

	// Is this space already used?
	if (ics->ref_space_used[type]) {
		IPC_ERROR(ics->server, "Space '%u' already used!", type);
		return XRT_ERROR_IPC_FAILURE;
	}

	xret = xrt_space_overseer_ref_space_inc(xso, type);
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(ics->server, "xrt_space_overseer_ref_space_inc failed");
		return xret;
	}

	// Can now mark it as used.
	ics->ref_space_used[type] = true;

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_space_unmark_ref_space_in_use(volatile struct ipc_client_state *ics, enum xrt_reference_space_type type)
{
	struct xrt_space_overseer *xso = ics->server->xso;
	xrt_result_t xret;

	xret = validate_reference_space_type(ics, type);
	if (xret != XRT_SUCCESS) {
		return XRT_ERROR_IPC_FAILURE;
	}

	if (!ics->ref_space_used[type]) {
		IPC_ERROR(ics->server, "Space '%u' not used!", type);
		return XRT_ERROR_IPC_FAILURE;
	}

	xret = xrt_space_overseer_ref_space_dec(xso, type);
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(ics->server, "xrt_space_overseer_ref_space_dec failed");
		return xret;
	}

	// Now we can mark it as not used.
	ics->ref_space_used[type] = false;

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_space_recenter_local_spaces(volatile struct ipc_client_state *ics)
{
	struct xrt_space_overseer *xso = ics->server->xso;

	return xrt_space_overseer_recenter_local_spaces(xso);
}

xrt_result_t
ipc_handle_space_get_tracking_origin_offset(volatile struct ipc_client_state *ics,
                                            uint32_t origin_id,
                                            struct xrt_pose *out_offset)
{
	struct xrt_space_overseer *xso = ics->server->xso;
	struct xrt_tracking_origin *xto;
	xrt_result_t xret = validate_origin_id(ics, origin_id, &xto);
	if (xret != XRT_SUCCESS) {
		return xret;
	}
	return xrt_space_overseer_get_tracking_origin_offset(xso, xto, out_offset);
}

xrt_result_t
ipc_handle_space_set_tracking_origin_offset(volatile struct ipc_client_state *ics,
                                            uint32_t origin_id,
                                            const struct xrt_pose *offset)
{
	struct xrt_space_overseer *xso = ics->server->xso;
	struct xrt_tracking_origin *xto;
	xrt_result_t xret = validate_origin_id(ics, origin_id, &xto);
	if (xret != XRT_SUCCESS) {
		return xret;
	}
	return xrt_space_overseer_set_tracking_origin_offset(xso, xto, offset);
}

xrt_result_t
ipc_handle_space_get_reference_space_offset(volatile struct ipc_client_state *ics,
                                            enum xrt_reference_space_type type,
                                            struct xrt_pose *out_offset)
{
	struct xrt_space_overseer *xso = ics->server->xso;
	return xrt_space_overseer_get_reference_space_offset(xso, type, out_offset);
}

xrt_result_t
ipc_handle_space_set_reference_space_offset(volatile struct ipc_client_state *ics,
                                            enum xrt_reference_space_type type,
                                            const struct xrt_pose *offset)
{
	struct xrt_space_overseer *xso = ics->server->xso;
	return xrt_space_overseer_set_reference_space_offset(xso, type, offset);
}

xrt_result_t
ipc_handle_compositor_get_info(volatile struct ipc_client_state *ics, struct xrt_compositor_info *out_info)
{
	IPC_TRACE_MARKER();

	if (ics->xc == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}

	*out_info = ics->xc->info;

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_compositor_predict_frame(volatile struct ipc_client_state *ics,
                                    int64_t *out_frame_id,
                                    int64_t *out_wake_up_time_ns,
                                    int64_t *out_predicted_display_time_ns,
                                    int64_t *out_predicted_display_period_ns)
{
	IPC_TRACE_MARKER();

	if (ics->xc == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}

	/*
	 * We use this to signal that the session has started, this is needed
	 * to make this client/session active/visible/focused.
	 */
	ipc_server_activate_session(ics);

	int64_t gpu_time_ns = 0;
	return xrt_comp_predict_frame(        //
	    ics->xc,                          //
	    out_frame_id,                     //
	    out_wake_up_time_ns,              //
	    &gpu_time_ns,                     //
	    out_predicted_display_time_ns,    //
	    out_predicted_display_period_ns); //
}

xrt_result_t
ipc_handle_compositor_wait_woke(volatile struct ipc_client_state *ics, int64_t frame_id)
{
	IPC_TRACE_MARKER();

	if (ics->xc == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}

	return xrt_comp_mark_frame(ics->xc, frame_id, XRT_COMPOSITOR_FRAME_POINT_WOKE, os_monotonic_get_ns());
}

xrt_result_t
ipc_handle_compositor_begin_frame(volatile struct ipc_client_state *ics, int64_t frame_id)
{
	IPC_TRACE_MARKER();

	if (ics->xc == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}

	return xrt_comp_begin_frame(ics->xc, frame_id);
}

xrt_result_t
ipc_handle_compositor_discard_frame(volatile struct ipc_client_state *ics, int64_t frame_id)
{
	IPC_TRACE_MARKER();

	if (ics->xc == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}

	return xrt_comp_discard_frame(ics->xc, frame_id);
}

xrt_result_t
ipc_handle_compositor_get_display_refresh_rate(volatile struct ipc_client_state *ics,
                                               float *out_display_refresh_rate_hz)
{
	IPC_TRACE_MARKER();

	if (ics->xc == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}

	return xrt_comp_get_display_refresh_rate(ics->xc, out_display_refresh_rate_hz);
}

xrt_result_t
ipc_handle_compositor_request_display_refresh_rate(volatile struct ipc_client_state *ics, float display_refresh_rate_hz)
{
	IPC_TRACE_MARKER();

	if (ics->xc == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}

	return xrt_comp_request_display_refresh_rate(ics->xc, display_refresh_rate_hz);
}

xrt_result_t
ipc_handle_compositor_set_performance_level(volatile struct ipc_client_state *ics,
                                            enum xrt_perf_domain domain,
                                            enum xrt_perf_set_level level)
{
	IPC_TRACE_MARKER();

	if (ics->xc == NULL) {
		return XRT_ERROR_IPC_COMPOSITOR_NOT_CREATED;
	}

	if (ics->xc->set_performance_level == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}

	return xrt_comp_set_performance_level(ics->xc, domain, level);
}

static bool
_update_projection_layer(struct xrt_compositor *xc,
                         volatile struct ipc_client_state *ics,
                         volatile struct ipc_layer_entry *layer,
                         uint32_t i)
{
	// xdev
	uint32_t device_id = layer->xdev_id;
	struct xrt_device *xdev = get_xdev(ics, device_id);

	if (xdev == NULL) {
		U_LOG_E("Invalid xdev for projection layer!");
		return false;
	}

	uint32_t view_count = xdev->hmd->view_count;

	struct xrt_swapchain *xcs[XRT_MAX_VIEWS];
	for (uint32_t k = 0; k < view_count; k++) {
		const uint32_t xsci = layer->swapchain_ids[k];
		xcs[k] = ics->xscs[xsci];
		if (xcs[k] == NULL) {
			U_LOG_E("Invalid swap chain for projection layer!");
			return false;
		}
	}


	// Cast away volatile.
	struct xrt_layer_data *data = (struct xrt_layer_data *)&layer->data;

	xrt_comp_layer_projection(xc, xdev, xcs, data);

	return true;
}

static bool
_update_projection_layer_depth(struct xrt_compositor *xc,
                               volatile struct ipc_client_state *ics,
                               volatile struct ipc_layer_entry *layer,
                               uint32_t i)
{
	// xdev
	uint32_t xdevi = layer->xdev_id;

	// Cast away volatile.
	struct xrt_layer_data *data = (struct xrt_layer_data *)&layer->data;

	struct xrt_device *xdev = get_xdev(ics, xdevi);
	if (xdev == NULL) {
		U_LOG_E("Invalid xdev for projection layer #%u!", i);
		return false;
	}

	struct xrt_swapchain *xcs[XRT_MAX_VIEWS];
	struct xrt_swapchain *d_xcs[XRT_MAX_VIEWS];

	for (uint32_t j = 0; j < data->view_count; j++) {
		int xsci = layer->swapchain_ids[j];
		int d_xsci = layer->swapchain_ids[j + data->view_count];

		xcs[j] = ics->xscs[xsci];
		d_xcs[j] = ics->xscs[d_xsci];
		if (xcs[j] == NULL || d_xcs[j] == NULL) {
			U_LOG_E("Invalid swap chain for projection layer #%u!", i);
			return false;
		}
	}

	xrt_comp_layer_projection_depth(xc, xdev, xcs, d_xcs, data);

	return true;
}

static bool
do_single(struct xrt_compositor *xc,
          volatile struct ipc_client_state *ics,
          volatile struct ipc_layer_entry *layer,
          uint32_t i,
          const char *name,
          struct xrt_device **out_xdev,
          struct xrt_swapchain **out_xcs,
          struct xrt_layer_data **out_data)
{
	uint32_t device_id = layer->xdev_id;
	uint32_t sci = layer->swapchain_ids[0];

	struct xrt_device *xdev = get_xdev(ics, device_id);
	struct xrt_swapchain *xcs = ics->xscs[sci];

	if (xcs == NULL) {
		U_LOG_E("Invalid swapchain for layer #%u, '%s'!", i, name);
		return false;
	}

	if (xdev == NULL) {
		U_LOG_E("Invalid xdev for layer #%u, '%s'!", i, name);
		return false;
	}

	// Cast away volatile.
	struct xrt_layer_data *data = (struct xrt_layer_data *)&layer->data;

	*out_xdev = xdev;
	*out_xcs = xcs;
	*out_data = data;

	return true;
}

static bool
_update_quad_layer(struct xrt_compositor *xc,
                   volatile struct ipc_client_state *ics,
                   volatile struct ipc_layer_entry *layer,
                   uint32_t i)
{
	struct xrt_device *xdev;
	struct xrt_swapchain *xcs;
	struct xrt_layer_data *data;

	if (!do_single(xc, ics, layer, i, "quad", &xdev, &xcs, &data)) {
		return false;
	}

	xrt_comp_layer_quad(xc, xdev, xcs, data);

	return true;
}

static bool
_update_cube_layer(struct xrt_compositor *xc,
                   volatile struct ipc_client_state *ics,
                   volatile struct ipc_layer_entry *layer,
                   uint32_t i)
{
	struct xrt_device *xdev;
	struct xrt_swapchain *xcs;
	struct xrt_layer_data *data;

	if (!do_single(xc, ics, layer, i, "cube", &xdev, &xcs, &data)) {
		return false;
	}

	xrt_comp_layer_cube(xc, xdev, xcs, data);

	return true;
}

static bool
_update_cylinder_layer(struct xrt_compositor *xc,
                       volatile struct ipc_client_state *ics,
                       volatile struct ipc_layer_entry *layer,
                       uint32_t i)
{
	struct xrt_device *xdev;
	struct xrt_swapchain *xcs;
	struct xrt_layer_data *data;

	if (!do_single(xc, ics, layer, i, "cylinder", &xdev, &xcs, &data)) {
		return false;
	}

	xrt_comp_layer_cylinder(xc, xdev, xcs, data);

	return true;
}

static bool
_update_equirect1_layer(struct xrt_compositor *xc,
                        volatile struct ipc_client_state *ics,
                        volatile struct ipc_layer_entry *layer,
                        uint32_t i)
{
	struct xrt_device *xdev;
	struct xrt_swapchain *xcs;
	struct xrt_layer_data *data;

	if (!do_single(xc, ics, layer, i, "equirect1", &xdev, &xcs, &data)) {
		return false;
	}

	xrt_comp_layer_equirect1(xc, xdev, xcs, data);

	return true;
}

static bool
_update_equirect2_layer(struct xrt_compositor *xc,
                        volatile struct ipc_client_state *ics,
                        volatile struct ipc_layer_entry *layer,
                        uint32_t i)
{
	struct xrt_device *xdev;
	struct xrt_swapchain *xcs;
	struct xrt_layer_data *data;

	if (!do_single(xc, ics, layer, i, "equirect2", &xdev, &xcs, &data)) {
		return false;
	}

	xrt_comp_layer_equirect2(xc, xdev, xcs, data);

	return true;
}

static bool
_update_passthrough_layer(struct xrt_compositor *xc,
                          volatile struct ipc_client_state *ics,
                          volatile struct ipc_layer_entry *layer,
                          uint32_t i)
{
	// xdev
	uint32_t xdevi = layer->xdev_id;

	struct xrt_device *xdev = get_xdev(ics, xdevi);

	if (xdev == NULL) {
		U_LOG_E("Invalid xdev for passthrough layer #%u!", i);
		return false;
	}

	// Cast away volatile.
	struct xrt_layer_data *data = (struct xrt_layer_data *)&layer->data;

	xrt_comp_layer_passthrough(xc, xdev, data);

	return true;
}

static bool
_update_layers(volatile struct ipc_client_state *ics, struct xrt_compositor *xc, struct ipc_layer_slot *slot)
{
	IPC_TRACE_MARKER();

	for (uint32_t i = 0; i < slot->layer_count; i++) {
		volatile struct ipc_layer_entry *layer = &slot->layers[i];

		switch (layer->data.type) {
		case XRT_LAYER_PROJECTION:
			if (!_update_projection_layer(xc, ics, layer, i)) {
				return false;
			}
			break;
		case XRT_LAYER_PROJECTION_DEPTH:
			if (!_update_projection_layer_depth(xc, ics, layer, i)) {
				return false;
			}
			break;
		case XRT_LAYER_QUAD:
			if (!_update_quad_layer(xc, ics, layer, i)) {
				return false;
			}
			break;
		case XRT_LAYER_CUBE:
			if (!_update_cube_layer(xc, ics, layer, i)) {
				return false;
			}
			break;
		case XRT_LAYER_CYLINDER:
			if (!_update_cylinder_layer(xc, ics, layer, i)) {
				return false;
			}
			break;
		case XRT_LAYER_EQUIRECT1:
			if (!_update_equirect1_layer(xc, ics, layer, i)) {
				return false;
			}
			break;
		case XRT_LAYER_EQUIRECT2:
			if (!_update_equirect2_layer(xc, ics, layer, i)) {
				return false;
			}
			break;
		case XRT_LAYER_PASSTHROUGH:
			if (!_update_passthrough_layer(xc, ics, layer, i)) {
				return false;
			}
			break;
		default: U_LOG_E("Unhandled layer type '%i'!", layer->data.type); break;
		}
	}

	return true;
}

xrt_result_t
ipc_handle_compositor_layer_sync(volatile struct ipc_client_state *ics,
                                 uint32_t slot_id,
                                 uint32_t *out_free_slot_id,
                                 const xrt_graphics_sync_handle_t *handles,
                                 const uint32_t handle_count)
{
	IPC_TRACE_MARKER();

	if (ics->xc == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}

	struct ipc_shared_memory *ism = ics->server->ism;
	struct ipc_layer_slot *slot = &ism->slots[slot_id];
	xrt_graphics_sync_handle_t sync_handle = XRT_GRAPHICS_SYNC_HANDLE_INVALID;

	// If we have one or more save the first handle.
	if (handle_count >= 1) {
		sync_handle = handles[0];
	}

	// Free all sync handles after the first one.
	for (uint32_t i = 1; i < handle_count; i++) {
		// Checks for valid handle.
		xrt_graphics_sync_handle_t tmp = handles[i];
		u_graphics_sync_unref(&tmp);
	}

	// Copy current slot data.
	struct ipc_layer_slot copy = *slot;


	/*
	 * Transfer data to underlying compositor.
	 */

	xrt_comp_layer_begin(ics->xc, &copy.data);

	_update_layers(ics, ics->xc, &copy);

	xrt_comp_layer_commit(ics->xc, sync_handle);


	/*
	 * Manage shared state.
	 */

	os_mutex_lock(&ics->server->global_state.lock);

	*out_free_slot_id = (ics->server->current_slot_index + 1) % IPC_MAX_SLOTS;
	ics->server->current_slot_index = *out_free_slot_id;

	os_mutex_unlock(&ics->server->global_state.lock);

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_compositor_layer_sync_with_semaphore(volatile struct ipc_client_state *ics,
                                                uint32_t slot_id,
                                                uint32_t semaphore_id,
                                                uint64_t semaphore_value,
                                                uint32_t *out_free_slot_id)
{
	IPC_TRACE_MARKER();

	if (ics->xc == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}
	if (semaphore_id >= IPC_MAX_CLIENT_SEMAPHORES) {
		IPC_ERROR(ics->server, "Invalid semaphore_id");
		return XRT_ERROR_IPC_FAILURE;
	}
	if (ics->xcsems[semaphore_id] == NULL) {
		IPC_ERROR(ics->server, "Semaphore of id %u not created!", semaphore_id);
		return XRT_ERROR_IPC_FAILURE;
	}

	struct xrt_compositor_semaphore *xcsem = ics->xcsems[semaphore_id];

	struct ipc_shared_memory *ism = ics->server->ism;
	struct ipc_layer_slot *slot = &ism->slots[slot_id];

	// Copy current slot data.
	struct ipc_layer_slot copy = *slot;



	/*
	 * Transfer data to underlying compositor.
	 */

	xrt_comp_layer_begin(ics->xc, &copy.data);

	_update_layers(ics, ics->xc, &copy);

	xrt_comp_layer_commit_with_semaphore(ics->xc, xcsem, semaphore_value);


	/*
	 * Manage shared state.
	 */

	os_mutex_lock(&ics->server->global_state.lock);

	*out_free_slot_id = (ics->server->current_slot_index + 1) % IPC_MAX_SLOTS;
	ics->server->current_slot_index = *out_free_slot_id;

	os_mutex_unlock(&ics->server->global_state.lock);

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_compositor_create_passthrough(volatile struct ipc_client_state *ics,
                                         const struct xrt_passthrough_create_info *info)
{
	IPC_TRACE_MARKER();

	if (ics->xc == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}

	return xrt_comp_create_passthrough(ics->xc, info);
}

xrt_result_t
ipc_handle_compositor_create_passthrough_layer(volatile struct ipc_client_state *ics,
                                               const struct xrt_passthrough_layer_create_info *info)
{
	IPC_TRACE_MARKER();

	if (ics->xc == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}

	return xrt_comp_create_passthrough_layer(ics->xc, info);
}

xrt_result_t
ipc_handle_compositor_destroy_passthrough(volatile struct ipc_client_state *ics)
{
	IPC_TRACE_MARKER();

	if (ics->xc == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}

	xrt_comp_destroy_passthrough(ics->xc);

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_compositor_set_thread_hint(volatile struct ipc_client_state *ics,
                                      enum xrt_thread_hint hint,
                                      uint32_t thread_id)

{
	IPC_TRACE_MARKER();

	if (ics->xc == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}

	return xrt_comp_set_thread_hint(ics->xc, hint, thread_id);
}

xrt_result_t
ipc_handle_compositor_get_reference_bounds_rect(volatile struct ipc_client_state *ics,
                                                enum xrt_reference_space_type reference_space_type,
                                                struct xrt_vec2 *bounds)
{
	IPC_TRACE_MARKER();

	if (ics->xc == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}

	return xrt_comp_get_reference_bounds_rect(ics->xc, reference_space_type, bounds);
}

xrt_result_t
ipc_handle_system_get_clients(volatile struct ipc_client_state *_ics, struct ipc_client_list *list)
{
	struct ipc_server *s = _ics->server;

	// Look client list.
	os_mutex_lock(&s->global_state.lock);

	uint32_t count = 0;
	for (uint32_t i = 0; i < IPC_MAX_CLIENTS; i++) {

		volatile struct ipc_client_state *ics = &s->threads[i].ics;

		// Is this thread running?
		if (ics->server_thread_index < 0) {
			continue;
		}

		list->ids[count++] = ics->client_state.id;
	}

	list->id_count = count;

	// Unlock now.
	os_mutex_unlock(&s->global_state.lock);

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_system_get_properties(volatile struct ipc_client_state *_ics, struct xrt_system_properties *out_properties)
{
	struct ipc_server *s = _ics->server;

	return ipc_server_get_system_properties(s, out_properties);
}

xrt_result_t
ipc_handle_system_get_client_info(volatile struct ipc_client_state *_ics,
                                  uint32_t client_id,
                                  struct ipc_app_state *out_ias)
{
	struct ipc_server *s = _ics->server;

	return ipc_server_get_client_app_state(s, client_id, out_ias);
}

xrt_result_t
ipc_handle_system_set_primary_client(volatile struct ipc_client_state *_ics, uint32_t client_id)
{
	struct ipc_server *s = _ics->server;

	IPC_INFO(s, "System setting active client to %d.", client_id);

	return ipc_server_set_active_client(s, client_id);
}

xrt_result_t
ipc_handle_system_set_focused_client(volatile struct ipc_client_state *ics, uint32_t client_id)
{
	IPC_INFO(ics->server, "UNIMPLEMENTED: system setting focused client to %d.", client_id);

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_system_toggle_io_client(volatile struct ipc_client_state *_ics, uint32_t client_id)
{
	struct ipc_server *s = _ics->server;

	IPC_INFO(s, "System toggling io for client %u.", client_id);

	return ipc_server_toggle_io_client(s, client_id);
}

xrt_result_t
ipc_handle_system_toggle_io_device(volatile struct ipc_client_state *ics, uint32_t device_id)
{
	if (device_id >= IPC_MAX_DEVICES) {
		return XRT_ERROR_IPC_FAILURE;
	}

	struct ipc_device *idev = &ics->server->idevs[device_id];

	idev->io_active = !idev->io_active;

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_swapchain_get_properties(volatile struct ipc_client_state *ics,
                                    const struct xrt_swapchain_create_info *info,
                                    struct xrt_swapchain_create_properties *xsccp)
{
	IPC_TRACE_MARKER();

	if (ics->xc == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}

	return xrt_comp_get_swapchain_create_properties(ics->xc, info, xsccp);
}

xrt_result_t
ipc_handle_swapchain_create(volatile struct ipc_client_state *ics,
                            const struct xrt_swapchain_create_info *info,
                            uint32_t *out_id,
                            uint32_t *out_image_count,
                            uint64_t *out_size,
                            bool *out_use_dedicated_allocation,
                            uint32_t max_handle_capacity,
                            xrt_graphics_buffer_handle_t *out_handles,
                            uint32_t *out_handle_count)
{
	IPC_TRACE_MARKER();

	xrt_result_t xret = XRT_SUCCESS;
	uint32_t index = 0;

	xret = validate_swapchain_state(ics, &index);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	// Create the swapchain
	struct xrt_swapchain *xsc = NULL; // Has to be NULL.
	xret = xrt_comp_create_swapchain(ics->xc, info, &xsc);
	if (xret != XRT_SUCCESS) {
		if (xret == XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED) {
			IPC_WARN(ics->server,
			         "xrt_comp_create_swapchain: Attempted to create valid, but unsupported swapchain");
		} else {
			IPC_ERROR(ics->server, "Error xrt_comp_create_swapchain failed!");
		}
		return xret;
	}

	// It's now safe to increment the number of swapchains.
	ics->swapchain_count++;

	IPC_TRACE(ics->server, "Created swapchain %d.", index);

	set_swapchain_info(ics, index, info, xsc);

	// return our result to the caller.
	struct xrt_swapchain_native *xscn = (struct xrt_swapchain_native *)xsc;

	// Limit checking
	assert(xsc->image_count <= XRT_MAX_SWAPCHAIN_IMAGES);
	assert(xsc->image_count <= max_handle_capacity);

	for (size_t i = 1; i < xsc->image_count; i++) {
		assert(xscn->images[0].size == xscn->images[i].size);
		assert(xscn->images[0].use_dedicated_allocation == xscn->images[i].use_dedicated_allocation);
	}

	// Assuming all images allocated in the same swapchain have the same allocation requirements.
	*out_size = xscn->images[0].size;
	*out_use_dedicated_allocation = xscn->images[0].use_dedicated_allocation;
	*out_id = index;
	*out_image_count = xsc->image_count;

	// Setup the fds.
	*out_handle_count = xsc->image_count;
	for (size_t i = 0; i < xsc->image_count; i++) {
		out_handles[i] = xscn->images[i].handle;
	}

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_swapchain_import(volatile struct ipc_client_state *ics,
                            const struct xrt_swapchain_create_info *info,
                            const struct ipc_arg_swapchain_from_native *args,
                            uint32_t *out_id,
                            const xrt_graphics_buffer_handle_t *handles,
                            uint32_t handle_count)
{
	IPC_TRACE_MARKER();

	xrt_result_t xret = XRT_SUCCESS;
	uint32_t index = 0;

	xret = validate_swapchain_state(ics, &index);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	struct xrt_image_native xins[XRT_MAX_SWAPCHAIN_IMAGES] = XRT_STRUCT_INIT;
	for (uint32_t i = 0; i < handle_count; i++) {
		xins[i].handle = handles[i];
		xins[i].size = args->sizes[i];
#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_WIN32_HANDLE)
		// DXGI handles need to be dealt with differently, they are identified
		// by having their lower bit set to 1 during transfer
		if ((size_t)xins[i].handle & 1) {
			xins[i].handle = (HANDLE)((size_t)xins[i].handle - 1);
			xins[i].is_dxgi_handle = true;
		}
#endif
	}

	// create the swapchain
	struct xrt_swapchain *xsc = NULL;
	xret = xrt_comp_import_swapchain(ics->xc, info, xins, handle_count, &xsc);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	// It's now safe to increment the number of swapchains.
	ics->swapchain_count++;

	IPC_TRACE(ics->server, "Created swapchain %d.", index);

	set_swapchain_info(ics, index, info, xsc);
	*out_id = index;

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_swapchain_wait_image(volatile struct ipc_client_state *ics, uint32_t id, int64_t timeout_ns, uint32_t index)
{
	if (ics->xc == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}

	//! @todo Look up the index.
	uint32_t sc_index = id;
	struct xrt_swapchain *xsc = ics->xscs[sc_index];

	return xrt_swapchain_wait_image(xsc, timeout_ns, index);
}

xrt_result_t
ipc_handle_swapchain_acquire_image(volatile struct ipc_client_state *ics, uint32_t id, uint32_t *out_index)
{
	if (ics->xc == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}

	//! @todo Look up the index.
	uint32_t sc_index = id;
	struct xrt_swapchain *xsc = ics->xscs[sc_index];

	xrt_swapchain_acquire_image(xsc, out_index);

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_swapchain_release_image(volatile struct ipc_client_state *ics, uint32_t id, uint32_t index)
{
	if (ics->xc == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}

	//! @todo Look up the index.
	uint32_t sc_index = id;
	struct xrt_swapchain *xsc = ics->xscs[sc_index];

	xrt_swapchain_release_image(xsc, index);

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_swapchain_destroy(volatile struct ipc_client_state *ics, uint32_t id)
{
	if (ics->xc == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}

	ics->swapchain_count--;

	// Drop our reference, does NULL checking. Cast away volatile.
	xrt_swapchain_reference((struct xrt_swapchain **)&ics->xscs[id], NULL);
	ics->swapchain_data[id].active = false;

	return XRT_SUCCESS;
}


/*
 *
 * Compositor semaphore function..
 *
 */

xrt_result_t
ipc_handle_compositor_semaphore_create(volatile struct ipc_client_state *ics,
                                       uint32_t *out_id,
                                       uint32_t max_handle_count,
                                       xrt_graphics_sync_handle_t *out_handles,
                                       uint32_t *out_handle_count)
{
	xrt_result_t xret;

	if (ics->xc == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}

	int id = 0;
	for (; id < IPC_MAX_CLIENT_SEMAPHORES; id++) {
		if (ics->xcsems[id] == NULL) {
			break;
		}
	}

	if (id == IPC_MAX_CLIENT_SEMAPHORES) {
		IPC_ERROR(ics->server, "Too many compositor semaphores alive!");
		return XRT_ERROR_IPC_FAILURE;
	}

	struct xrt_compositor_semaphore *xcsem = NULL;
	xrt_graphics_sync_handle_t handle = XRT_GRAPHICS_SYNC_HANDLE_INVALID;

	xret = xrt_comp_create_semaphore(ics->xc, &handle, &xcsem);
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(ics->server, "Failed to create compositor semaphore!");
		return xret;
	}

	// Set it directly, no need to use reference here.
	ics->xcsems[id] = xcsem;

	// Set out parameters.
	*out_id = id;
	out_handles[0] = handle;
	*out_handle_count = 1;

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_compositor_semaphore_destroy(volatile struct ipc_client_state *ics, uint32_t id)
{
	if (ics->xc == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}

	if (ics->xcsems[id] == NULL) {
		IPC_ERROR(ics->server, "Client tried to delete non-existent compositor semaphore!");
		return XRT_ERROR_IPC_FAILURE;
	}

	ics->compositor_semaphore_count--;

	// Drop our reference, does NULL checking. Cast away volatile.
	xrt_compositor_semaphore_reference((struct xrt_compositor_semaphore **)&ics->xcsems[id], NULL);

	return XRT_SUCCESS;
}


/*
 *
 * Device functions.
 *
 */

xrt_result_t
ipc_handle_device_update_input(volatile struct ipc_client_state *ics, uint32_t id)
{
	// To make the code a bit more readable.
	uint32_t device_id = id;
	struct ipc_shared_memory *ism = ics->server->ism;
	struct ipc_device *idev = get_idev(ics, device_id);
	struct xrt_device *xdev = idev->xdev;
	struct ipc_shared_device *isdev = &ism->isdevs[device_id];

	// Update inputs.
	xrt_result_t xret = xrt_device_update_inputs(xdev);
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(ics->server, "Failed to update input");
		return xret;
	}

	// Copy data into the shared memory.
	struct xrt_input *src = xdev->inputs;
	struct xrt_input *dst = &ism->inputs[isdev->first_input_index];
	size_t size = sizeof(struct xrt_input) * isdev->input_count;

	bool io_active = ics->io_active && idev->io_active;
	if (io_active) {
		memcpy(dst, src, size);
	} else {
		memset(dst, 0, size);

		for (uint32_t i = 0; i < isdev->input_count; i++) {
			dst[i].name = src[i].name;

			// Special case the rotation of the head.
			if (dst[i].name == XRT_INPUT_GENERIC_HEAD_POSE) {
				dst[i].active = src[i].active;
			}
		}
	}

	// Reply.
	return XRT_SUCCESS;
}

static struct xrt_input *
find_input(volatile struct ipc_client_state *ics, uint32_t device_id, enum xrt_input_name name)
{
	struct ipc_shared_memory *ism = ics->server->ism;
	struct ipc_shared_device *isdev = &ism->isdevs[device_id];
	struct xrt_input *io = &ism->inputs[isdev->first_input_index];

	for (uint32_t i = 0; i < isdev->input_count; i++) {
		if (io[i].name == name) {
			return &io[i];
		}
	}

	return NULL;
}

xrt_result_t
ipc_handle_device_get_tracked_pose(volatile struct ipc_client_state *ics,
                                   uint32_t id,
                                   enum xrt_input_name name,
                                   int64_t at_timestamp,
                                   struct xrt_space_relation *out_relation)
{
	// To make the code a bit more readable.
	uint32_t device_id = id;
	struct ipc_device *isdev = &ics->server->idevs[device_id];
	struct xrt_device *xdev = isdev->xdev;

	// Find the input
	struct xrt_input *input = find_input(ics, device_id, name);
	if (input == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}

	// Special case the headpose.
	bool disabled = (!isdev->io_active || !ics->io_active) && name != XRT_INPUT_GENERIC_HEAD_POSE;
	bool active_on_client = input->active;

	// We have been disabled but the client hasn't called update.
	if (disabled && active_on_client) {
		U_ZERO(out_relation);
		return XRT_SUCCESS;
	}

	if (disabled || !active_on_client) {
		return XRT_ERROR_POSE_NOT_ACTIVE;
	}

	// Get the pose.
	return xrt_device_get_tracked_pose(xdev, name, at_timestamp, out_relation);
}

xrt_result_t
ipc_handle_device_get_hand_tracking(volatile struct ipc_client_state *ics,
                                    uint32_t id,
                                    enum xrt_input_name name,
                                    int64_t at_timestamp,
                                    struct xrt_hand_joint_set *out_value,
                                    int64_t *out_timestamp)
{

	// To make the code a bit more readable.
	uint32_t device_id = id;
	struct xrt_device *xdev = get_xdev(ics, device_id);

	// Get the pose.
	return xrt_device_get_hand_tracking(xdev, name, at_timestamp, out_value, out_timestamp);
}

xrt_result_t
ipc_handle_device_get_view_poses(volatile struct ipc_client_state *ics,
                                 uint32_t id,
                                 const struct xrt_vec3 *fallback_eye_relation,
                                 int64_t at_timestamp_ns,
                                 uint32_t view_count)
{
	struct ipc_message_channel *imc = (struct ipc_message_channel *)&ics->imc;
	struct ipc_device_get_view_poses_reply reply = XRT_STRUCT_INIT;
	struct ipc_server *s = ics->server;
	xrt_result_t xret;

	// To make the code a bit more readable.
	uint32_t device_id = id;
	struct xrt_device *xdev = get_xdev(ics, device_id);


	if (view_count == 0 || view_count > IPC_MAX_RAW_VIEWS) {
		IPC_ERROR(s, "Client asked for zero or too many views! (%u)", view_count);

		reply.result = XRT_ERROR_IPC_FAILURE;
		// Send the full reply, the client expects it.
		return ipc_send(imc, &reply, sizeof(reply));
	}

	// Data to get.
	struct xrt_fov fovs[IPC_MAX_RAW_VIEWS];
	struct xrt_pose poses[IPC_MAX_RAW_VIEWS];

	xrt_device_get_view_poses( //
	    xdev,                  //
	    fallback_eye_relation, //
	    at_timestamp_ns,       //
	    view_count,            //
	    &reply.head_relation,  //
	    fovs,                  //
	    poses);                //

	/*
	 * Operation ok, head_relation has already been put in the reply
	 * struct, so we don't need to send that manually.
	 */
	reply.result = XRT_SUCCESS;

	/*
	 * This isn't really needed, but demonstrates the server sending the
	 * length back in the reply, a common pattern for other functions.
	 */
	reply.view_count = view_count;

	/*
	 * Send the reply first isn't required for functions in general, but it
	 * will need to match what the client expects. This demonstrates the
	 * server sending the length back in the reply, a common pattern for
	 * other functions.
	 */
	xret = ipc_send(imc, &reply, sizeof(reply));
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(s, "Failed to send reply!");
		return xret;
	}

	// Send the fovs that we got.
	xret = ipc_send(imc, fovs, sizeof(struct xrt_fov) * view_count);
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(s, "Failed to send fovs!");
		return xret;
	}

	// And finally the poses.
	xret = ipc_send(imc, poses, sizeof(struct xrt_pose) * view_count);
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(s, "Failed to send poses!");
		return xret;
	}

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_device_get_view_poses_2(volatile struct ipc_client_state *ics,
                                   uint32_t id,
                                   const struct xrt_vec3 *default_eye_relation,
                                   int64_t at_timestamp_ns,
                                   uint32_t view_count,
                                   struct ipc_info_get_view_poses_2 *out_info)
{
	// To make the code a bit more readable.
	uint32_t device_id = id;
	struct xrt_device *xdev = get_xdev(ics, device_id);
	xrt_device_get_view_poses(    //
	    xdev,                     //
	    default_eye_relation,     //
	    at_timestamp_ns,          //
	    view_count,               //
	    &out_info->head_relation, //
	    out_info->fovs,           //
	    out_info->poses);         //

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_device_compute_distortion(volatile struct ipc_client_state *ics,
                                     uint32_t id,
                                     uint32_t view,
                                     float u,
                                     float v,
                                     bool *out_ret,
                                     struct xrt_uv_triplet *out_triplet)
{
	// To make the code a bit more readable.
	uint32_t device_id = id;
	struct xrt_device *xdev = get_xdev(ics, device_id);

	bool ret = xrt_device_compute_distortion(xdev, view, u, v, out_triplet);
	*out_ret = ret;

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_device_begin_plane_detection_ext(volatile struct ipc_client_state *ics,
                                            uint32_t id,
                                            uint64_t plane_detection_id,
                                            uint64_t *out_plane_detection_id)
{
	// To make the code a bit more readable.
	uint32_t device_id = id;
	struct xrt_device *xdev = get_xdev(ics, device_id);

	uint64_t new_count = ics->plane_detection_count + 1;

	if (new_count > ics->plane_detection_size) {
		IPC_TRACE(ics->server, "Plane detections tracking size: %u -> %u", (uint32_t)ics->plane_detection_count,
		          (uint32_t)new_count);

		U_ARRAY_REALLOC_OR_FREE(ics->plane_detection_ids, uint64_t, new_count);
		U_ARRAY_REALLOC_OR_FREE(ics->plane_detection_xdev, struct xrt_device *, new_count);
		ics->plane_detection_size = new_count;
	}

	struct xrt_plane_detector_begin_info_ext *begin_info = &ics->server->ism->plane_begin_info_ext;

	enum xrt_result xret =
	    xrt_device_begin_plane_detection_ext(xdev, begin_info, plane_detection_id, out_plane_detection_id);
	if (xret != XRT_SUCCESS) {
		IPC_TRACE(ics->server, "xrt_device_begin_plane_detection_ext error: %d", xret);
		return xret;
	}

	if (*out_plane_detection_id != 0) {
		uint64_t index = ics->plane_detection_count;
		ics->plane_detection_ids[index] = *out_plane_detection_id;
		ics->plane_detection_xdev[index] = xdev;
		ics->plane_detection_count = new_count;
	}

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_device_destroy_plane_detection_ext(volatile struct ipc_client_state *ics,
                                              uint32_t id,
                                              uint64_t plane_detection_id)
{
	// To make the code a bit more readable.
	uint32_t device_id = id;
	struct xrt_device *xdev = get_xdev(ics, device_id);

	enum xrt_result xret = xrt_device_destroy_plane_detection_ext(xdev, plane_detection_id);

	// Iterate through plane detection ids. Once found, move every item one slot to the left.
	bool compact_right = false;
	for (uint32_t i = 0; i < ics->plane_detection_count; i++) {
		if (ics->plane_detection_ids[i] == plane_detection_id) {
			compact_right = true;
		}
		if (compact_right && (i + 1) < ics->plane_detection_count) {
			ics->plane_detection_ids[i] = ics->plane_detection_ids[i + 1];
			ics->plane_detection_xdev[i] = ics->plane_detection_xdev[i + 1];
		}
	}
	// if the plane detection was correctly tracked compact_right should always be true
	if (compact_right) {
		ics->plane_detection_count -= 1;
	} else {
		IPC_ERROR(ics->server, "Destroyed plane detection that was not tracked");
	}

	if (xret != XRT_SUCCESS) {
		IPC_ERROR(ics->server, "xrt_device_destroy_plane_detection_ext error: %d", xret);
		return xret;
	}

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_device_get_plane_detection_state_ext(volatile struct ipc_client_state *ics,
                                                uint32_t id,
                                                uint64_t plane_detection_id,
                                                enum xrt_plane_detector_state_ext *out_state)
{
	// To make the code a bit more readable.
	uint32_t device_id = id;
	struct xrt_device *xdev = get_xdev(ics, device_id);

	xrt_result_t xret = xrt_device_get_plane_detection_state_ext(xdev, plane_detection_id, out_state);
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(ics->server, "xrt_device_get_plane_detection_state_ext error: %d", xret);
		return xret;
	}

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_device_get_plane_detections_ext(volatile struct ipc_client_state *ics,
                                           uint32_t id,
                                           uint64_t plane_detection_id)

{
	struct ipc_message_channel *imc = (struct ipc_message_channel *)&ics->imc;
	struct ipc_device_get_plane_detections_ext_reply reply = XRT_STRUCT_INIT;
	struct ipc_server *s = ics->server;

	// To make the code a bit more readable.
	uint32_t device_id = id;
	struct xrt_device *xdev = get_xdev(ics, device_id);

	struct xrt_plane_detections_ext out = {0};

	xrt_result_t xret = xrt_device_get_plane_detections_ext(xdev, plane_detection_id, &out);
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(ics->server, "xrt_device_get_plane_detections_ext error: %d", xret);
		// probably nothing allocated on error, but make sure
		xrt_plane_detections_ext_clear(&out);
		return xret;
	}

	reply.result = XRT_SUCCESS;
	reply.location_size = out.location_count; // because we initialized to 0, now size == count
	reply.polygon_size = out.polygon_info_size;
	reply.vertex_size = out.vertex_size;

	xret = ipc_send(imc, &reply, sizeof(reply));
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(s, "Failed to send reply!");
		goto out;
	}

	// send expected contents

	if (out.location_count > 0) {
		xret =
		    ipc_send(imc, out.locations, sizeof(struct xrt_plane_detector_location_ext) * out.location_count);
		if (xret != XRT_SUCCESS) {
			IPC_ERROR(s, "Failed to send locations!");
			goto out;
		}

		xret = ipc_send(imc, out.polygon_info_start_index, sizeof(uint32_t) * out.location_count);
		if (xret != XRT_SUCCESS) {
			IPC_ERROR(s, "Failed to send locations!");
			goto out;
		}
	}

	if (out.polygon_info_size > 0) {
		xret =
		    ipc_send(imc, out.polygon_infos, sizeof(struct xrt_plane_polygon_info_ext) * out.polygon_info_size);
		if (xret != XRT_SUCCESS) {
			IPC_ERROR(s, "Failed to send polygon_infos!");
			goto out;
		}
	}

	if (out.vertex_size > 0) {
		xret = ipc_send(imc, out.vertices, sizeof(struct xrt_vec2) * out.vertex_size);
		if (xret != XRT_SUCCESS) {
			IPC_ERROR(s, "Failed to send vertices!");
			goto out;
		}
	}

out:
	xrt_plane_detections_ext_clear(&out);
	return xret;
}

xrt_result_t
ipc_handle_device_set_output(volatile struct ipc_client_state *ics,
                             uint32_t id,
                             enum xrt_output_name name,
                             const struct xrt_output_value *value)
{
	// To make the code a bit more readable.
	uint32_t device_id = id;
	struct xrt_device *xdev = get_xdev(ics, device_id);

	// Set the output.
	xrt_device_set_output(xdev, name, value);

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_device_set_haptic_output(volatile struct ipc_client_state *ics,
                                    uint32_t id,
                                    enum xrt_output_name name,
                                    const struct ipc_pcm_haptic_buffer *buffer)
{
	IPC_TRACE_MARKER();
	struct ipc_message_channel *imc = (struct ipc_message_channel *)&ics->imc;
	struct ipc_server *s = ics->server;

	xrt_result_t xret;

	// To make the code a bit more readable.
	uint32_t device_id = id;
	struct xrt_device *xdev = get_xdev(ics, device_id);

	os_mutex_lock(&ics->server->global_state.lock);

	float *samples = U_TYPED_ARRAY_CALLOC(float, buffer->num_samples);

	// send the allocation result
	xret = samples ? XRT_SUCCESS : XRT_ERROR_ALLOCATION;
	xret = ipc_send(imc, &xret, sizeof xret);
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(ics->server, "Failed to send samples allocate result");
		goto set_haptic_output_end;
	}

	if (!samples) {
		IPC_ERROR(s, "Failed to allocate samples for haptic output");
		xret = XRT_ERROR_ALLOCATION;
		goto set_haptic_output_end;
	}

	xret = ipc_receive(imc, samples, sizeof(float) * buffer->num_samples);
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(s, "Failed to receive samples");
		goto set_haptic_output_end;
	}

	uint32_t samples_consumed;
	struct xrt_output_value value = {
	    .type = XRT_OUTPUT_VALUE_TYPE_PCM_VIBRATION,
	    .pcm_vibration =
	        {
	            .append = buffer->append,
	            .buffer_size = buffer->num_samples,
	            .sample_rate = buffer->sample_rate,
	            .samples_consumed = &samples_consumed,
	            .buffer = samples,
	        },
	};

	// Set the output.
	xrt_device_set_output(xdev, name, &value);

	xret = ipc_send(imc, &samples_consumed, sizeof samples_consumed);
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(ics->server, "Failed to send samples consumed");
		goto set_haptic_output_end;
	}

	xret = XRT_SUCCESS;

set_haptic_output_end:
	os_mutex_unlock(&ics->server->global_state.lock);

	free(samples);

	return xret;
}

xrt_result_t
ipc_handle_device_get_output_limits(volatile struct ipc_client_state *ics,
                                    uint32_t id,
                                    struct xrt_output_limits *limits)
{
	// To make the code a bit more readable.
	uint32_t device_id = id;
	struct xrt_device *xdev = get_xdev(ics, device_id);

	// Set the output.
	return xrt_device_get_output_limits(xdev, limits);
}

xrt_result_t
ipc_handle_device_get_visibility_mask(volatile struct ipc_client_state *ics,
                                      uint32_t device_id,
                                      enum xrt_visibility_mask_type type,
                                      uint32_t view_index)
{
	struct ipc_message_channel *imc = (struct ipc_message_channel *)&ics->imc;
	struct ipc_device_get_visibility_mask_reply reply = XRT_STRUCT_INIT;
	struct ipc_server *s = ics->server;
	xrt_result_t xret;

	// @todo verify
	struct xrt_device *xdev = get_xdev(ics, device_id);
	struct xrt_visibility_mask *mask = NULL;
	if (xdev->get_visibility_mask) {
		xret = xrt_device_get_visibility_mask(xdev, type, view_index, &mask);
		if (xret != XRT_SUCCESS) {
			IPC_ERROR(s, "Failed to get visibility mask");
			return xret;
		}
	} else {
		struct xrt_fov fov = xdev->hmd->distortion.fov[view_index];
		u_visibility_mask_get_default(type, &fov, &mask);
	}

	if (mask == NULL) {
		IPC_ERROR(s, "Failed to get visibility mask");
		reply.mask_size = 0;
	} else {
		reply.mask_size = xrt_visibility_mask_get_size(mask);
	}

	xret = ipc_send(imc, &reply, sizeof(reply));
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(s, "Failed to send reply");
		goto out_free;
	}

	xret = ipc_send(imc, mask, reply.mask_size);
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(s, "Failed to send mask");
		goto out_free;
	}

out_free:
	free(mask);
	return xret;
}

xrt_result_t
ipc_handle_device_is_form_factor_available(volatile struct ipc_client_state *ics,
                                           uint32_t id,
                                           enum xrt_form_factor form_factor,
                                           bool *out_available)
{
	// To make the code a bit more readable.
	uint32_t device_id = id;
	struct xrt_device *xdev = get_xdev(ics, device_id);
	*out_available = xrt_device_is_form_factor_available(xdev, form_factor);
	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_system_devices_get_roles(volatile struct ipc_client_state *ics, struct xrt_system_roles *out_roles)
{
	return xrt_system_devices_get_roles(ics->server->xsysd, out_roles);
}

xrt_result_t
ipc_handle_system_devices_begin_feature(volatile struct ipc_client_state *ics, enum xrt_device_feature_type type)
{
	struct xrt_system_devices *xsysd = ics->server->xsysd;
	xrt_result_t xret;

	xret = validate_device_feature_type(ics, type);
	if (xret != XRT_SUCCESS) {
		return XRT_ERROR_IPC_FAILURE;
	}

	// Is this feature already used?
	if (ics->device_feature_used[type]) {
		IPC_ERROR(ics->server, "feature '%u' already used!", type);
		return XRT_ERROR_IPC_FAILURE;
	}

	xret = xrt_system_devices_feature_inc(xsysd, type);
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(ics->server, "xrt_system_devices_feature_inc failed");
		return xret;
	}

	// Can now mark it as used.
	ics->device_feature_used[type] = true;

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_system_devices_end_feature(volatile struct ipc_client_state *ics, enum xrt_device_feature_type type)
{
	struct xrt_system_devices *xsysd = ics->server->xsysd;
	xrt_result_t xret;

	xret = validate_device_feature_type(ics, type);
	if (xret != XRT_SUCCESS) {
		return XRT_ERROR_IPC_FAILURE;
	}

	if (!ics->device_feature_used[type]) {
		IPC_ERROR(ics->server, "feature '%u' not used!", type);
		return XRT_ERROR_IPC_FAILURE;
	}

	xret = xrt_system_devices_feature_dec(xsysd, type);
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(ics->server, "xrt_system_devices_feature_dec failed");
		return xret;
	}

	// Now we can mark it as not used.
	ics->device_feature_used[type] = false;

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_device_get_face_tracking(volatile struct ipc_client_state *ics,
                                    uint32_t id,
                                    enum xrt_input_name facial_expression_type,
                                    int64_t at_timestamp_ns,
                                    struct xrt_facial_expression_set *out_value)
{
	const uint32_t device_id = id;
	struct xrt_device *xdev = get_xdev(ics, device_id);
	// Get facial expression data.
	return xrt_device_get_face_tracking(xdev, facial_expression_type, at_timestamp_ns, out_value);
}

xrt_result_t
ipc_handle_device_get_body_skeleton(volatile struct ipc_client_state *ics,
                                    uint32_t id,
                                    enum xrt_input_name body_tracking_type,
                                    struct xrt_body_skeleton *out_value)
{
	struct xrt_device *xdev = get_xdev(ics, id);
	return xrt_device_get_body_skeleton(xdev, body_tracking_type, out_value);
}

xrt_result_t
ipc_handle_device_get_body_joints(volatile struct ipc_client_state *ics,
                                  uint32_t id,
                                  enum xrt_input_name body_tracking_type,
                                  int64_t desired_timestamp_ns,
                                  struct xrt_body_joint_set *out_value)
{
	struct xrt_device *xdev = get_xdev(ics, id);
	return xrt_device_get_body_joints(xdev, body_tracking_type, desired_timestamp_ns, out_value);
}

xrt_result_t
ipc_handle_device_get_battery_status(
    volatile struct ipc_client_state *ics, uint32_t id, bool *out_present, bool *out_charging, float *out_charge)
{
	struct xrt_device *xdev = get_xdev(ics, id);
	return xrt_device_get_battery_status(xdev, out_present, out_charging, out_charge);
}
