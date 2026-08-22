# FireVAD Console Example
cmake_minimum_required(VERSION 3.16)

# Add esp_firevad component from parent directory
set(EXTRA_COMPONENT_DIRS "../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(firered_vad_console)

