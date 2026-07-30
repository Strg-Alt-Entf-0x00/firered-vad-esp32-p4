# FireVAD Console Example
cmake_minimum_required(VERSION 3.16)

# Add esp_firevad component from parent directory
set(EXTRA_COMPONENT_DIRS "../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(firered_vad_console)

# Automatically create SPIFFS image from models if they exist
if(EXISTS "${CMAKE_SOURCE_DIR}/converted_models")
    message(STATUS "Found converted_models/ - Creating SPIFFS image...")
    
    # SPIFFS has filename length limits (default 32 chars)
    # Solution: Flatten directory structure before creating image
    set(SPIFFS_STAGING_DIR "${CMAKE_BINARY_DIR}/spiffs_staging")
    file(REMOVE_RECURSE "${SPIFFS_STAGING_DIR}")
    file(MAKE_DIRECTORY "${SPIFFS_STAGING_DIR}")
    
    # Copy all .frvd files to flat staging directory
    file(GLOB_RECURSE FRVD_FILES "${CMAKE_SOURCE_DIR}/converted_models/*.frvd")
    foreach(frvd_file ${FRVD_FILES})
        get_filename_component(filename ${frvd_file} NAME)
        file(COPY ${frvd_file} DESTINATION "${SPIFFS_STAGING_DIR}")
        message(STATUS "  Adding: ${filename}")
    endforeach()
    
    list(LENGTH FRVD_FILES model_count)
    message(STATUS "Total models: ${model_count}")
    
    spiffs_create_partition_image(spiffs "${SPIFFS_STAGING_DIR}" FLASH_IN_PROJECT)
    
    message(STATUS "SPIFFS image will be flashed automatically with 'idf.py flash'")
else()
    message(WARNING "Models not found! Run: python download_models.py")
    message(WARNING "SPIFFS partition will be empty.")
endif()
