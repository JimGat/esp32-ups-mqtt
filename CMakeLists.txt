# Minimum CMake version
cmake_minimum_required(VERSION 3.16)

# Single source of truth for firmware version. Update this line only.
set(PROJECT_VER "v0.4.34-dev")

# Include the ESP-IDF build system
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Project name
project(esp32_ups_mqtt_bridge)

# Expose PROJECT_VER to firmware and web UI code.
idf_build_set_property(COMPILE_DEFINITIONS "FW_VERSION=\"${PROJECT_VER}\"" APPEND)

