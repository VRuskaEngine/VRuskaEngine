# Copyright 2019-2024, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0

add_library(VRuska Engine SHARED VRuska Engine.c libVRuska Engine.def)
set(LIBVRuska Engine_HEADER_DIR ${CMAKE_INSTALL_INCLUDEDIR}/VRuska Engine)
target_link_libraries(VRuska Engine PRIVATE aux_util ipc_client)
target_include_directories(
	VRuska Engine INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
			 $<INSTALL_INTERFACE:${LIBVRuska Engine_HEADER_DIR}>
	)

set_target_properties(
	VRuska Engine PROPERTIES VERSION ${CMAKE_PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}
	)

install(TARGETS VRuska Engine RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES VRuska Engine.h DESTINATION ${LIBVRuska Engine_HEADER_DIR})

add_executable(libVRuska Engine-example example.c)
target_link_libraries(libVRuska Engine-example PRIVATE VRuska Engine)
