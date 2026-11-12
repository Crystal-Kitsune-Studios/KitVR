cmake_minimum_required(VERSION 3.16)
project(kitvr_layer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND)
    find_path(VULKAN_INCLUDE_DIR vulkan/vulkan.h REQUIRED)
    set(Vulkan_INCLUDE_DIRS "${VULKAN_INCLUDE_DIR}")
endif()

add_library(kitvr_layer SHARED kitvr_layer.cpp)
target_include_directories(kitvr_layer PRIVATE ${Vulkan_INCLUDE_DIRS})
set_target_properties(kitvr_layer PROPERTIES PREFIX "" OUTPUT_NAME "kitvr_layer")

install(TARGETS kitvr_layer LIBRARY DESTINATION kitvr)
install(FILES kitvr_layer.json DESTINATION kitvr)
