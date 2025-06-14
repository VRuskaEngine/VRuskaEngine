# Copyright 2019-2022, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0

add_library(
	android_app_glue STATIC
	${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
	)
target_include_directories(android_app_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)
# disable these warnings in external code
target_compile_options(android_app_glue PRIVATE -Wno-format-pedantic -Wno-strict-prototypes)

add_library(
	aux_android STATIC
	android_ahardwarebuffer_allocator.c
	android_ahardwarebuffer_allocator.h
	android_custom_surface.cpp
	android_custom_surface.h
	android_globals.cpp
	android_globals.h
	android_instance_base.c
	android_instance_base.h
	android_lifecycle_callbacks.cpp
	android_lifecycle_callbacks.h
	android_load_class.cpp
	android_load_class.hpp
	android_looper.cpp
	android_looper.h
	org.freedesktop.VRuska Engine.auxiliary.cpp
	org.freedesktop.VRuska Engine.auxiliary.hpp
	org.freedesktop.VRuska Engine.auxiliary.impl.hpp
	)
target_link_libraries(
	aux_android
	PUBLIC aux_util xrt-interfaces
	PRIVATE
		${ANDROID_LIBRARY}
		${ANDROID_LOG_LIBRARY}
		xrt-external-jni-wrap
		xrt-external-jnipp
		android_app_glue
	)
