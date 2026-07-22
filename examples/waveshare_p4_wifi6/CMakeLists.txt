cmake_minimum_required(VERSION 3.16)

list(APPEND EXTRA_COMPONENT_DIRS "../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(waveshare_p4_firevad_example)

spiffs_create_partition_image(spiffs ../../converted_models FLASH_IN_PROJECT)
