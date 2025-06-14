// Copyright 2020-2024, Collabora, Ltd.
// Copyright 2024-2025, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Server process functions.
 * @author Pete Black <pblack@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup ipc_server
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_system.h"
#include "xrt/xrt_instance.h"
#include "xrt/xrt_compositor.h"
#include "xrt/xrt_config_have.h"
#include "xrt/xrt_config_os.h"

#include "os/os_time.h"
#include "util/u_var.h"
#include "util/u_misc.h"
#include "util/u_debug.h"
#include "util/u_trace_marker.h"
#include "util/u_verify.h"
#include "util/u_process.h"
#include "util/u_debug_gui.h"
#include "util/u_pretty_print.h"

#include "util/u_git_tag.h"

#include "shared/ipc_shmem.h"
#include "server/ipc_server.h"
#include "server/ipc_server_interface.h"

#include <stdlib.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <limits.h>

#if defined(XRT_OS_WINDOWS)
#include <timeapi.h>
#endif


/*
 *
 * Defines and helpers.
 *
 */

DEBUG_GET_ONCE_BOOL_OPTION(exit_on_disconnect, "IPC_EXIT_ON_DISCONNECT", false)
DEBUG_GET_ONCE_BOOL_OPTION(exit_when_idle, "IPC_EXIT_WHEN_IDLE", false)
DEBUG_GET_ONCE_NUM_OPTION(exit_when_idle_delay_ms, "IPC_EXIT_WHEN_IDLE_DELAY_MS", 5000)
DEBUG_GET_ONCE_LOG_OPTION(ipc_log, "IPC_LOG", U_LOGGING_INFO)


/*
 *
 * Idev functions.
 *
 */

static int32_t
find_xdev_index(struct ipc_server *s, struct xrt_device *xdev)
{
	if (xdev == NULL) {
		return -1;
	}

	for (int32_t i = 0; i < XRT_SYSTEM_MAX_DEVICES; i++) {
		if (s->xsysd->xdevs[i] == xdev) {
			return i;
		}
	}

	IPC_WARN(s, "Could not find index for xdev: '%s'", xdev->str);

	return -1;
}

static void
init_idev(struct ipc_device *idev, struct xrt_device *xdev)
{
	if (xdev != NULL) {
		idev->io_active = true;
		idev->xdev = xdev;
	} else {
		idev->io_active = false;
	}
}

static void
teardown_idev(struct ipc_device *idev)
{
	idev->io_active = false;
}

static int
init_idevs(struct ipc_server *s)
{
	// Copy the devices over into the idevs array.
	for (size_t i = 0; i < XRT_SYSTEM_MAX_DEVICES; i++) {
		if (s->xsysd->xdevs[i] == NULL) {
			continue;
		}

		init_idev(&s->idevs[i], s->xsysd->xdevs[i]);
	}

	return 0;
}

static void
teardown_idevs(struct ipc_server *s)
{
	for (size_t i = 0; i < XRT_SYSTEM_MAX_DEVICES; i++) {
		teardown_idev(&s->idevs[i]);
	}
}


/*
 *
 * Static functions.
 *
 */

XRT_MAYBE_UNUSED static void
print_linux_end_user_failed_information(enum u_logging_level log_level)
{
	struct u_pp_sink_stack_only sink;
	u_pp_delegate_t dg = u_pp_sink_stack_only_init(&sink);

	// Print Newline
#define PN() u_pp(dg, "\n")
	// Print Newline, Hash, Space
#define PNH() u_pp(dg, "\n#")
	// Print Newline, Hash, Space
#define PNHS(...) u_pp(dg, "\n# "__VA_ARGS__)
	// Print Newline, 80 Hashes
#define PN80H()                                                                                                        \
	do {                                                                                                           \
		PN();                                                                                                  \
		for (uint32_t i = 0; i < 8; i++) {                                                                     \
			u_pp(dg, "##########");                                                                        \
		}                                                                                                      \
	} while (false)

	PN80H();
	PNHS("                                                                             #");
	PNHS("                 The VRuska Engine service has failed to start.                     #");
	PNHS("                                                                             #");
	PNHS("If you want to report please upload the logs of the service as a text file.  #");
	PNHS("You can also capture the output the VRuska Engine-cli info command to provide more  #");
	PNHS("information about your system, that will help diagnosing your problem. The   #");
	PNHS("below commands is how you best capture the information from the commands.    #");
	PNHS("                                                                             #");
	PNHS("    VRuska Engine-cli info 2>&1 | tee info.txt                                      #");
	PNHS("    VRuska Engine-service 2>&1 | tee logs.txt                                       #");
	PNHS("                                                                             #");
	PN80H();

	U_LOG_IFL_I(log_level, "%s", sink.buffer);
}

XRT_MAYBE_UNUSED static void
print_linux_end_user_started_information(enum u_logging_level log_level)
{
	struct u_pp_sink_stack_only sink;
	u_pp_delegate_t dg = u_pp_sink_stack_only_init(&sink);


	PN80H();
	PNHS("                                                                             #");
	PNHS("                       The VRuska Engine service has started.                       #");
	PNHS("                                                                             #");
	PN80H();

#undef PN
#undef PNH
#undef PNHS
#undef PN80H

	U_LOG_IFL_I(log_level, "%s", sink.buffer);
}

static void
teardown_all(struct ipc_server *s)
{
	u_var_remove_root(s);

	xrt_syscomp_destroy(&s->xsysc);

	teardown_idevs(s);

	xrt_space_overseer_destroy(&s->xso);
	xrt_system_devices_destroy(&s->xsysd);
	xrt_system_destroy(&s->xsys);

	xrt_instance_destroy(&s->xinst);

	ipc_server_mainloop_deinit(&s->ml);

	u_process_destroy(s->process);

	ipc_shmem_destroy(&s->ism_handle, (void **)&s->ism, sizeof(struct ipc_shared_memory));

	// Destroyed last.
	os_mutex_destroy(&s->global_state.lock);
}

static int
init_tracking_origins(struct ipc_server *s)
{
	for (size_t i = 0; i < XRT_SYSTEM_MAX_DEVICES; i++) {
		struct xrt_device *xdev = s->idevs[i].xdev;
		if (xdev == NULL) {
			continue;
		}

		struct xrt_tracking_origin *xtrack = xdev->tracking_origin;
		assert(xtrack != NULL);
		size_t index = 0;

		for (; index < XRT_SYSTEM_MAX_DEVICES; index++) {
			if (s->xtracks[index] == NULL) {
				s->xtracks[index] = xtrack;
				break;
			}
			if (s->xtracks[index] == xtrack) {
				break;
			}
		}
	}

	return 0;
}

static void
handle_binding(struct ipc_shared_memory *ism,
               struct xrt_binding_profile *xbp,
               struct ipc_shared_binding_profile *isbp,
               uint32_t *input_pair_index_ptr,
               uint32_t *output_pair_index_ptr)
{
	uint32_t input_pair_index = *input_pair_index_ptr;
	uint32_t output_pair_index = *output_pair_index_ptr;

	isbp->name = xbp->name;

	// Copy the initial state and also count the number in input_pairs.
	uint32_t input_pair_start = input_pair_index;
	for (size_t k = 0; k < xbp->input_count; k++) {
		ism->input_pairs[input_pair_index++] = xbp->inputs[k];
	}

	// Setup the 'offsets' and number of input_pairs.
	if (input_pair_start != input_pair_index) {
		isbp->input_count = input_pair_index - input_pair_start;
		isbp->first_input_index = input_pair_start;
	}

	// Copy the initial state and also count the number in outputs.
	uint32_t output_pair_start = output_pair_index;
	for (size_t k = 0; k < xbp->output_count; k++) {
		ism->output_pairs[output_pair_index++] = xbp->outputs[k];
	}

	// Setup the 'offsets' and number of output_pairs.
	if (output_pair_start != output_pair_index) {
		isbp->output_count = output_pair_index - output_pair_start;
		isbp->first_output_index = output_pair_start;
	}

	*input_pair_index_ptr = input_pair_index;
	*output_pair_index_ptr = output_pair_index;
}

static int
init_shm(struct ipc_server *s)
{
	const size_t size = sizeof(struct ipc_shared_memory);
	xrt_shmem_handle_t handle;
	xrt_result_t result = ipc_shmem_create(size, &handle, (void **)&s->ism);
	if (result != XRT_SUCCESS) {
		return -1;
	}

	// we have a filehandle, we will pass this to our client
	s->ism_handle = handle;


	/*
	 *
	 * Setup the shared memory state.
	 *
	 */

	uint32_t count = 0;
	struct ipc_shared_memory *ism = s->ism;

	ism->startup_timestamp = os_monotonic_get_ns();

	// Setup the tracking origins.
	count = 0;
	for (size_t i = 0; i < XRT_SYSTEM_MAX_DEVICES; i++) {
		struct xrt_tracking_origin *xtrack = s->xtracks[i];
		if (xtrack == NULL) {
			continue;
		}

		// The position of the tracking origin matches that in the
		// server's memory.
		assert(i < XRT_SYSTEM_MAX_DEVICES);

		struct ipc_shared_tracking_origin *itrack = &ism->itracks[count++];
		memcpy(itrack->name, xtrack->name, sizeof(itrack->name));
		itrack->type = xtrack->type;
		itrack->offset = xtrack->initial_offset;
	}

	ism->itrack_count = count;

	count = 0;
	uint32_t input_index = 0;
	uint32_t output_index = 0;
	uint32_t binding_index = 0;
	uint32_t input_pair_index = 0;
	uint32_t output_pair_index = 0;

	for (size_t i = 0; i < XRT_SYSTEM_MAX_DEVICES; i++) {
		struct xrt_device *xdev = s->idevs[i].xdev;
		if (xdev == NULL) {
			continue;
		}

		struct ipc_shared_device *isdev = &ism->isdevs[count++];

		isdev->name = xdev->name;
		memcpy(isdev->str, xdev->str, sizeof(isdev->str));
		memcpy(isdev->serial, xdev->serial, sizeof(isdev->serial));

		// Copy information.
		isdev->device_type = xdev->device_type;
		isdev->supported = xdev->supported;

		// Setup the tracking origin.
		isdev->tracking_origin_index = (uint32_t)-1;
		for (uint32_t k = 0; k < XRT_SYSTEM_MAX_DEVICES; k++) {
			if (xdev->tracking_origin != s->xtracks[k]) {
				continue;
			}

			isdev->tracking_origin_index = k;
			break;
		}

		assert(isdev->tracking_origin_index != (uint32_t)-1);

		// Initial update.
		xrt_device_update_inputs(xdev);

		// Bindings
		uint32_t binding_start = binding_index;
		for (size_t k = 0; k < xdev->binding_profile_count; k++) {
			handle_binding(ism, &xdev->binding_profiles[k], &ism->binding_profiles[binding_index++],
			               &input_pair_index, &output_pair_index);
		}

		// Setup the 'offsets' and number of bindings.
		if (binding_start != binding_index) {
			isdev->binding_profile_count = binding_index - binding_start;
			isdev->first_binding_profile_index = binding_start;
		}

		// Copy the initial state and also count the number in inputs.
		uint32_t input_start = input_index;
		for (size_t k = 0; k < xdev->input_count; k++) {
			ism->inputs[input_index++] = xdev->inputs[k];
		}

		// Setup the 'offsets' and number of inputs.
		if (input_start != input_index) {
			isdev->input_count = input_index - input_start;
			isdev->first_input_index = input_start;
		}

		// Copy the initial state and also count the number in outputs.
		uint32_t output_start = output_index;
		for (size_t k = 0; k < xdev->output_count; k++) {
			ism->outputs[output_index++] = xdev->outputs[k];
		}

		// Setup the 'offsets' and number of outputs.
		if (output_start != output_index) {
			isdev->output_count = output_index - output_start;
			isdev->first_output_index = output_start;
		}
	}

	// Setup the HMD
	// set view count
	assert(s->xsysd->static_roles.head->hmd);
	ism->hmd.view_count = s->xsysd->static_roles.head->hmd->view_count;
	for (uint32_t view = 0; view < s->xsysd->static_roles.head->hmd->view_count; ++view) {
		ism->hmd.views[view].display.w_pixels = s->xsysd->static_roles.head->hmd->views[view].display.w_pixels;
		ism->hmd.views[view].display.h_pixels = s->xsysd->static_roles.head->hmd->views[view].display.h_pixels;
	}

	for (size_t i = 0; i < s->xsysd->static_roles.head->hmd->blend_mode_count; i++) {
		// Not super necessary, we also do this assert in oxr_system.c
		assert(u_verify_blend_mode_valid(s->xsysd->static_roles.head->hmd->blend_modes[i]));
		ism->hmd.blend_modes[i] = s->xsysd->static_roles.head->hmd->blend_modes[i];
	}
	ism->hmd.blend_mode_count = s->xsysd->static_roles.head->hmd->blend_mode_count;

	// Finally tell the client how many devices we have.
	s->ism->isdev_count = count;

	// Assign all of the roles.
	ism->roles.head = find_xdev_index(s, s->xsysd->static_roles.head);
	ism->roles.eyes = find_xdev_index(s, s->xsysd->static_roles.eyes);
	ism->roles.face = find_xdev_index(s, s->xsysd->static_roles.face);
	ism->roles.body = find_xdev_index(s, s->xsysd->static_roles.body);
	ism->roles.hand_tracking.left = find_xdev_index(s, s->xsysd->static_roles.hand_tracking.left);
	ism->roles.hand_tracking.right = find_xdev_index(s, s->xsysd->static_roles.hand_tracking.right);

	// Fill out git version info.
	snprintf(s->ism->u_git_tag, IPC_VERSION_NAME_LEN, "%s", u_git_tag);

	return 0;
}

static void
init_server_state(struct ipc_server *s)
{
	// set up initial state for global vars, and each client state

	s->global_state.active_client_index = -1; // we start off with no active client.
	s->global_state.last_active_client_index = -1;
	s->global_state.connected_client_count = 0; // No clients connected initially
	s->current_slot_index = 0;

	for (uint32_t i = 0; i < IPC_MAX_CLIENTS; i++) {
		volatile struct ipc_client_state *ics = &s->threads[i].ics;
		ics->server = s;
		ics->server_thread_index = -1;
	}
}

static int
init_all(struct ipc_server *s, enum u_logging_level log_level)
{
	xrt_result_t xret;
	int ret;

	// First order of business set the log level.
	s->log_level = log_level;

	// This should never fail.
	ret = os_mutex_init(&s->global_state.lock);
	if (ret < 0) {
		IPC_ERROR(s, "Global state lock mutex failed to init!");
		// Do not call teardown_all here, os_mutex_destroy will assert.
		return ret;
	}

	s->process = u_process_create_if_not_running();

	if (!s->process) {
		IPC_ERROR(s, "VRuska Engine-service is already running! Use XRT_LOG=trace for more information.");
		teardown_all(s);
		return 1;
	}

	// Yes we should be running.
	s->running = true;
	s->exit_on_disconnect = debug_get_bool_option_exit_on_disconnect();
	s->exit_when_idle = debug_get_bool_option_exit_when_idle();
	s->last_client_disconnect_ns = 0;
	uint64_t delay_ms = debug_get_num_option_exit_when_idle_delay_ms();
	s->exit_when_idle_delay_ns = delay_ms * U_TIME_1MS_IN_NS;

	xret = xrt_instance_create(NULL, &s->xinst);
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(s, "Failed to create instance!");
		teardown_all(s);
		return -1;
	}

	xret = xrt_instance_create_system(s->xinst, &s->xsys, &s->xsysd, &s->xso, &s->xsysc);
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(s, "Could not create system!");
		teardown_all(s);
		return -1;
	}

	ret = init_idevs(s);
	if (ret < 0) {
		IPC_ERROR(s, "Failed to init idevs!");
		teardown_all(s);
		return ret;
	}

	ret = init_tracking_origins(s);
	if (ret < 0) {
		IPC_ERROR(s, "Failed to init tracking origins!");
		teardown_all(s);
		return ret;
	}

	ret = init_shm(s);
	if (ret < 0) {
		IPC_ERROR(s, "Could not init shared memory!");
		teardown_all(s);
		return ret;
	}

	ret = ipc_server_mainloop_init(&s->ml);
	if (ret < 0) {
		IPC_ERROR(s, "Failed to init ipc main loop!");
		teardown_all(s);
		return ret;
	}

	// Never fails, do this second last.
	init_server_state(s);

	u_var_add_root(s, "IPC Server", false);
	u_var_add_log_level(s, &s->log_level, "Log level");
	u_var_add_bool(s, &s->exit_on_disconnect, "exit_on_disconnect");
	u_var_add_bool(s, &s->exit_when_idle, "exit_when_idle");
	u_var_add_u64(s, &s->exit_when_idle_delay_ns, "exit_when_idle_delay_ns");
	u_var_add_bool(s, (bool *)&s->running, "running");

	return 0;
}

static int
main_loop(struct ipc_server *s)
{
	while (s->running) {
		os_nanosleep(U_TIME_1S_IN_NS / 20);

		// Check polling.
		ipc_server_mainloop_poll(s, &s->ml);
	}

	return 0;
}


/*
 *
 * Client management functions.
 *
 */

static void
handle_overlay_client_events(volatile struct ipc_client_state *ics, int active_id, int prev_active_id)
{
	// Is an overlay session?
	if (!ics->client_state.session_overlay) {
		return;
	}

	// Does this client have a compositor yet, if not return?
	if (ics->xc == NULL) {
		return;
	}

	// Switch between main applications
	if (active_id >= 0 && prev_active_id >= 0) {
		xrt_syscomp_set_main_app_visibility(ics->server->xsysc, ics->xc, false);
		xrt_syscomp_set_main_app_visibility(ics->server->xsysc, ics->xc, true);
	}

	// Switch from idle to active application
	if (active_id >= 0 && prev_active_id < 0) {
		xrt_syscomp_set_main_app_visibility(ics->server->xsysc, ics->xc, true);
	}

	// Switch from active application to idle
	if (active_id < 0 && prev_active_id >= 0) {
		xrt_syscomp_set_main_app_visibility(ics->server->xsysc, ics->xc, false);
	}
}

static void
handle_focused_client_events(volatile struct ipc_client_state *ics, int active_id, int prev_active_id)
{
	// Set start z_order at the bottom.
	int64_t z_order = INT64_MIN;

	// Set visibility/focus to false on all applications.
	bool focused = false;
	bool visible = false;

	// Set visible + focused if we are the primary application
	if (ics->server_thread_index == active_id) {
		visible = true;
		focused = true;
		z_order = INT64_MIN;
	}

	// Set all overlays to always active and focused.
	if (ics->client_state.session_overlay) {
		visible = true;
		focused = true;
		z_order = ics->client_state.z_order;
	}

	ics->client_state.session_visible = visible;
	ics->client_state.session_focused = focused;
	ics->client_state.z_order = z_order;

	if (ics->xc != NULL) {
		xrt_syscomp_set_state(ics->server->xsysc, ics->xc, visible, focused);
		xrt_syscomp_set_z_order(ics->server->xsysc, ics->xc, z_order);
	}
}

static void
flush_state_to_all_clients_locked(struct ipc_server *s)
{
	for (uint32_t i = 0; i < IPC_MAX_CLIENTS; i++) {
		volatile struct ipc_client_state *ics = &s->threads[i].ics;

		// Not running?
		if (ics->server_thread_index < 0) {
			continue;
		}

		handle_focused_client_events(ics, s->global_state.active_client_index,
		                             s->global_state.last_active_client_index);
		handle_overlay_client_events(ics, s->global_state.active_client_index,
		                             s->global_state.last_active_client_index);
	}
}

static void
update_server_state_locked(struct ipc_server *s)
{
	// if our client that is set to active is still active,
	// and it is the same as our last active client, we can
	// early-out, as no events need to be sent

	if (s->global_state.active_client_index >= 0) {

		volatile struct ipc_client_state *ics = &s->threads[s->global_state.active_client_index].ics;

		if (ics->client_state.session_active &&
		    s->global_state.active_client_index == s->global_state.last_active_client_index) {
			return;
		}
	}


	// our active application has changed - this would typically be
	// switched by the VRuska Engine-ctl application or other app making a
	// 'set active application' ipc call, or it could be a
	// connection loss resulting in us needing to 'fall through' to
	// the first active application
	//, or finally to the idle 'wallpaper' images.


	bool set_idle = true;
	int fallback_active_application = -1;

	// do we have a fallback application?
	for (uint32_t i = 0; i < IPC_MAX_CLIENTS; i++) {
		volatile struct ipc_client_state *ics = &s->threads[i].ics;
		if (ics->client_state.session_overlay == false && ics->server_thread_index >= 0 &&
		    ics->client_state.session_active) {
			fallback_active_application = i;
			set_idle = false;
		}
	}

	// if there is a currently-set active primary application and it is not
	// actually active/displayable, use the fallback application
	// instead.
	if (s->global_state.active_client_index >= 0) {
		volatile struct ipc_client_state *ics = &s->threads[s->global_state.active_client_index].ics;
		if (!(ics->client_state.session_overlay == false && ics->client_state.session_active)) {
			s->global_state.active_client_index = fallback_active_application;
		}
	}


	// if we have no applications to fallback to, enable the idle
	// wallpaper.
	if (set_idle) {
		s->global_state.active_client_index = -1;
	}

	flush_state_to_all_clients_locked(s);

	s->global_state.last_active_client_index = s->global_state.active_client_index;
}

static volatile struct ipc_client_state *
find_client_locked(struct ipc_server *s, uint32_t client_id)
{
	// Check for invalid IDs.
	if (client_id == 0 || client_id > INT_MAX) {
		IPC_WARN(s, "Invalid ID '%u', failing operation.", client_id);
		return NULL;
	}

	for (uint32_t i = 0; i < IPC_MAX_CLIENTS; i++) {
		volatile struct ipc_client_state *ics = &s->threads[i].ics;

		// Is this the client we are looking for?
		if (ics->client_state.id != client_id) {
			continue;
		}

		// Just in case of state data.
		if (!xrt_ipc_handle_is_valid(ics->imc.ipc_handle)) {
			IPC_WARN(s, "Encountered invalid state while searching for client with ID '%d'", client_id);
			return NULL;
		}

		return ics;
	}

	IPC_WARN(s, "No client with ID '%u', failing operation.", client_id);

	return NULL;
}

static xrt_result_t
get_client_app_state_locked(struct ipc_server *s, uint32_t client_id, struct ipc_app_state *out_ias)
{
	volatile struct ipc_client_state *ics = find_client_locked(s, client_id);
	if (ics == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}

	struct ipc_app_state ias = ics->client_state;
	ias.io_active = ics->io_active;

	// @todo: track this data in the ipc_client_state struct
	ias.primary_application = false;

	// The active client is decided by index, so get that from the ics.
	int index = ics->server_thread_index;

	if (s->global_state.active_client_index == index) {
		ias.primary_application = true;
	}

	*out_ias = ias;

	return XRT_SUCCESS;
}

static xrt_result_t
set_active_client_locked(struct ipc_server *s, uint32_t client_id)
{
	volatile struct ipc_client_state *ics = find_client_locked(s, client_id);
	if (ics == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}

	// The active client is decided by index, so get that from the ics.
	int index = ics->server_thread_index;

	if (index != s->global_state.active_client_index) {
		s->global_state.active_client_index = index;
	}

	return XRT_SUCCESS;
}

static xrt_result_t
toggle_io_client_locked(struct ipc_server *s, uint32_t client_id)
{
	volatile struct ipc_client_state *ics = find_client_locked(s, client_id);
	if (ics == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}

	ics->io_active = !ics->io_active;

	return XRT_SUCCESS;
}


/*
 *
 * Exported functions.
 *
 */

xrt_result_t
ipc_server_get_client_app_state(struct ipc_server *s, uint32_t client_id, struct ipc_app_state *out_ias)
{
	os_mutex_lock(&s->global_state.lock);
	xrt_result_t xret = get_client_app_state_locked(s, client_id, out_ias);
	os_mutex_unlock(&s->global_state.lock);

	return xret;
}

xrt_result_t
ipc_server_set_active_client(struct ipc_server *s, uint32_t client_id)
{
	os_mutex_lock(&s->global_state.lock);
	xrt_result_t xret = set_active_client_locked(s, client_id);
	os_mutex_unlock(&s->global_state.lock);

	return xret;
}

xrt_result_t
ipc_server_toggle_io_client(struct ipc_server *s, uint32_t client_id)
{
	os_mutex_lock(&s->global_state.lock);
	xrt_result_t xret = toggle_io_client_locked(s, client_id);
	os_mutex_unlock(&s->global_state.lock);

	return xret;
}

void
ipc_server_activate_session(volatile struct ipc_client_state *ics)
{
	struct ipc_server *s = ics->server;

	// Already active, noop.
	if (ics->client_state.session_active) {
		return;
	}

	assert(ics->server_thread_index >= 0);

	// Multiple threads could call this at the same time.
	os_mutex_lock(&s->global_state.lock);

	ics->client_state.session_active = true;

	if (ics->client_state.session_overlay) {
		// For new active overlay sessions only update this session.
		handle_focused_client_events(ics, s->global_state.active_client_index,
		                             s->global_state.last_active_client_index);
		handle_overlay_client_events(ics, s->global_state.active_client_index,
		                             s->global_state.last_active_client_index);
	} else {
		// Update active client
		set_active_client_locked(s, ics->client_state.id);

		// For new active regular sessions update all clients.
		update_server_state_locked(s);
	}

	os_mutex_unlock(&s->global_state.lock);
}

void
ipc_server_deactivate_session(volatile struct ipc_client_state *ics)
{
	struct ipc_server *s = ics->server;

	// Multiple threads could call this at the same time.
	os_mutex_lock(&s->global_state.lock);

	ics->client_state.session_active = false;

	update_server_state_locked(s);

	os_mutex_unlock(&s->global_state.lock);
}

void
ipc_server_update_state(struct ipc_server *s)
{
	// Multiple threads could call this at the same time.
	os_mutex_lock(&s->global_state.lock);

	update_server_state_locked(s);

	os_mutex_unlock(&s->global_state.lock);
}

void
ipc_server_handle_failure(struct ipc_server *vs)
{
	// Right now handled just the same as a graceful shutdown.
	vs->running = false;
}

void
ipc_server_handle_shutdown_signal(struct ipc_server *vs)
{
	vs->running = false;
}

void
ipc_server_handle_client_connected(struct ipc_server *vs, xrt_ipc_handle_t ipc_handle)
{
	volatile struct ipc_client_state *ics = NULL;
	int32_t cs_index = -1;

	os_mutex_lock(&vs->global_state.lock);

	// Increment the connected client counter
	vs->global_state.connected_client_count++;

	// A client connected, so we're no longer in a delayed exit state
	// (The delay thread will still check the client count before exiting)
	vs->last_client_disconnect_ns = 0;

	// find the next free thread in our array (server_thread_index is -1)
	// and have it handle this connection
	for (uint32_t i = 0; i < IPC_MAX_CLIENTS; i++) {
		volatile struct ipc_client_state *_cs = &vs->threads[i].ics;
		if (_cs->server_thread_index < 0) {
			ics = _cs;
			cs_index = i;
			break;
		}
	}
	if (ics == NULL) {
		xrt_ipc_handle_close(ipc_handle);

		// Unlock when we are done.
		os_mutex_unlock(&vs->global_state.lock);

		U_LOG_E("Max client count reached!");
		return;
	}

	struct ipc_thread *it = &vs->threads[cs_index];
	if (it->state != IPC_THREAD_READY && it->state != IPC_THREAD_STOPPING) {
		// we should not get here
		xrt_ipc_handle_close(ipc_handle);

		// Unlock when we are done.
		os_mutex_unlock(&vs->global_state.lock);

		U_LOG_E("Client state management error!");
		return;
	}

	if (it->state != IPC_THREAD_READY) {
		os_thread_join(&it->thread);
		os_thread_destroy(&it->thread);
		it->state = IPC_THREAD_READY;
	}

	it->state = IPC_THREAD_STARTING;

	// Allocate a new ID, avoid zero.
	//! @todo validate ID.
	uint32_t id = ++vs->id_generator;

	// Reset everything.
	U_ZERO((struct ipc_client_state *)ics);

	// Set state.
	ics->client_state.id = id;
	ics->imc.ipc_handle = ipc_handle;
	ics->server = vs;
	ics->server_thread_index = cs_index;
	ics->io_active = true;

	ics->plane_detection_size = 0;
	ics->plane_detection_count = 0;
	ics->plane_detection_ids = NULL;
	ics->plane_detection_xdev = NULL;

	os_thread_start(&it->thread, ipc_server_client_thread, (void *)ics);

	// Unlock when we are done.
	os_mutex_unlock(&vs->global_state.lock);
}

xrt_result_t
ipc_server_get_system_properties(struct ipc_server *vs, struct xrt_system_properties *out_properties)
{
	memcpy(out_properties, &vs->xsys->properties, sizeof(*out_properties));
	return XRT_SUCCESS;
}

#ifndef XRT_OS_ANDROID
int
ipc_server_main(int argc, char **argv, const struct ipc_server_main_info *ismi)
{
	// Get log level first.
	enum u_logging_level log_level = debug_get_log_option_ipc_log();

	// Log very early who we are.
	U_LOG_IFL_I(log_level, "%s '%s' starting up...", u_runtime_description, u_git_tag);

	// Allocate the server itself.
	struct ipc_server *s = U_TYPED_CALLOC(struct ipc_server);

#ifdef XRT_OS_WINDOWS
	timeBeginPeriod(1);
#endif

	/*
	 * Need to create early before any vars are added. Not created in
	 * init_all since that function is shared with Android and the debug
	 * GUI isn't supported on Android.
	 */
	u_debug_gui_create(&ismi->udgci, &s->debug_gui);


	int ret = init_all(s, log_level);
	if (ret < 0) {
#ifdef XRT_OS_LINUX
		// Print information how to debug issues.
		print_linux_end_user_failed_information(log_level);
#endif

		u_debug_gui_stop(&s->debug_gui);
		free(s);
		return ret;
	}

	// Start the debug UI now (if enabled).
	u_debug_gui_start(s->debug_gui, s->xinst, s->xsysd);

#ifdef XRT_OS_LINUX
	// Print a very clear service started message.
	print_linux_end_user_started_information(log_level);
#endif
	// Main loop.
	ret = main_loop(s);

	// Stop the UI before tearing everything down.
	u_debug_gui_stop(&s->debug_gui);

	// Done after UI stopped.
	teardown_all(s);
	free(s);

#ifdef XRT_OS_WINDOWS
	timeEndPeriod(1);
#endif

	U_LOG_IFL_I(log_level, "Server exiting: '%i'", ret);

	return ret;
}

#endif // !XRT_OS_ANDROID

#ifdef XRT_OS_ANDROID
int
ipc_server_main_android(struct ipc_server **ps, void (*startup_complete_callback)(void *data), void *data)
{
	// Get log level first.
	enum u_logging_level log_level = debug_get_log_option_ipc_log();

	struct ipc_server *s = U_TYPED_CALLOC(struct ipc_server);
	U_LOG_D("Created IPC server!");

	int ret = init_all(s, log_level);
	if (ret < 0) {
		free(s);
		startup_complete_callback(data);
		return ret;
	}

	*ps = s;
	startup_complete_callback(data);

	ret = main_loop(s);

	teardown_all(s);
	free(s);

	U_LOG_I("Server exiting '%i'!", ret);

	return ret;
}
#endif // XRT_OS_ANDROID
