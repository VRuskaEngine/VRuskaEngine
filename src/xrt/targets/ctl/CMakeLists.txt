# Copyright 2020-2021, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0

add_executable(VRuska Engine-ctl main.c)
add_sanitizers(VRuska Engine-ctl)

target_include_directories(VRuska Engine-ctl PRIVATE ipc)

target_link_libraries(VRuska Engine-ctl PRIVATE aux_util ipc_client)

install(TARGETS VRuska Engine-ctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
