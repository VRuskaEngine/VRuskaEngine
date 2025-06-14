# Copyright 2019-2021, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0

######
# Create a cli interface for VRuska Engine.

add_executable(
	cli
	cli_cmd_calibration_dump.c
	cli_cmd_info.c
	cli_cmd_lighthouse.c
	cli_cmd_probe.c
	cli_cmd_slambatch.c
	cli_cmd_test.c
	cli_common.h
	cli_main.c
	)
add_sanitizers(cli)

if(NOT WIN32)
	# No getline on Windows, so until we have a portable impl
	target_sources(cli PRIVATE cli_cmd_calibrate.c)
endif()

if(XRT_HAVE_OPENCV)
	target_link_libraries(cli PRIVATE aux_tracking)
endif()

set_target_properties(cli PROPERTIES OUTPUT_NAME VRuska Engine-cli PREFIX "")

target_link_libraries(
	cli
	PRIVATE
		aux_os
		aux_os_ble
		aux_util
		aux_math
		target_instance_no_comp
		drv_includes
	)

install(TARGETS cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
