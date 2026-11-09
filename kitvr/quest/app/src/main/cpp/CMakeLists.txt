cmake_minimum_required(VERSION 3.22.1)
project(kitvr LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenXR REQUIRED CONFIG)

add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

add_library(kitvr SHARED
    main.cpp
    session.cpp
    decoder.cpp
    audio.cpp)

target_compile_definitions(kitvr PRIVATE
    XR_USE_PLATFORM_ANDROID
    XR_USE_GRAPHICS_API_OPENGL_ES)
target_compile_options(kitvr PRIVATE -Wall -Wextra -Wno-unused-parameter)

target_link_libraries(kitvr PRIVATE
    native_app_glue
    OpenXR::openxr_loader
    android log EGL GLESv3 mediandk aaudio)

# Keep the entry point that NativeActivity looks up, since it lives in a static library.
set_target_properties(kitvr PROPERTIES LINK_FLAGS "-u ANativeActivity_onCreate")
