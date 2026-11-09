cmake_minimum_required(VERSION 3.22)
project(kitvr_steamvr_driver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OPENVR_DRIVER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/openvr/openvr_driver.h")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/openvr")
file(DOWNLOAD
    "https://raw.githubusercontent.com/ValveSoftware/openvr/v2.15.6/headers/openvr_driver.h"
    "${OPENVR_DRIVER_HEADER}"
    EXPECTED_HASH SHA256=1036efe998d63e82d1d3db2b32a2f58df4a8eeaf5280f50aaf28220ff60a40ab
    TLS_VERIFY ON
    STATUS OPENVR_DOWNLOAD_STATUS)
list(GET OPENVR_DOWNLOAD_STATUS 0 OPENVR_DOWNLOAD_CODE)
if(NOT OPENVR_DOWNLOAD_CODE EQUAL 0)
    list(GET OPENVR_DOWNLOAD_STATUS 1 OPENVR_DOWNLOAD_MESSAGE)
    message(FATAL_ERROR "Could not fetch OpenVR driver header: ${OPENVR_DOWNLOAD_MESSAGE}")
endif()

add_library(driver_kitvr SHARED src/driver.cpp)
target_include_directories(driver_kitvr PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/openvr")
target_compile_definitions(driver_kitvr PRIVATE OPENVR_BUILD_STATIC)
if(WIN32)
    target_link_libraries(driver_kitvr PRIVATE ws2_32)
    set_target_properties(driver_kitvr PROPERTIES PREFIX "")
    install(TARGETS driver_kitvr RUNTIME DESTINATION kitvr/bin/win64)
else()
    set_target_properties(driver_kitvr PROPERTIES PREFIX "")
    install(TARGETS driver_kitvr LIBRARY DESTINATION kitvr/bin/linux64)
endif()

install(FILES driver/driver.vrdrivermanifest DESTINATION kitvr)
install(DIRECTORY driver/resources/ DESTINATION kitvr/resources)
install(FILES README.md DESTINATION kitvr)
