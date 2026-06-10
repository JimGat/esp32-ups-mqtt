# Minimum CMake version
cmake_minimum_required(VERSION 3.16)

# Include the ESP-IDF build system
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Project name
project(esp32_ups_mqtt_bridge)
