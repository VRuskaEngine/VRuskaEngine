// Copyright 2020, Collabora, Ltd.
// Copyright 2024-2025, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Main file for VRuska Engine service.
 * @author Pete Black <pblack@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup ipc
 */

#include "xrt/xrt_config_os.h"

#include "util/u_metrics.h"
#include "util/u_logging.h"
#include "util/u_trace_marker.h"

#ifdef XRT_OS_WINDOWS
#include "util/u_windows.h"
#endif

#include "server/ipc_server_interface.h"

#include "target_lists.h"


// Insert the on load constructor to init trace marker.
U_TRACE_TARGET_SETUP(U_TRACE_WHICH_SERVICE)


int
main(int argc, char *argv[])
{
#ifdef XRT_OS_WINDOWS
	u_win_try_privilege_or_priority_from_args(U_LOGGING_INFO, argc, argv);
#endif

	u_trace_marker_init();
	u_metrics_init();

	struct ipc_server_main_info ismi = {
	    .udgci =
	        {
	            .window_title = "VRuska Engine! ✨⚡🔥",
	            .open = U_DEBUG_GUI_OPEN_AUTO,
	        },
	};

	int ret = ipc_server_main(argc, argv, &ismi);

	u_metrics_close();

	return ret;
}
