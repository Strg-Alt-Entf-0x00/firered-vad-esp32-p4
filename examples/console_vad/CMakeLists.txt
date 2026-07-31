# FireVAD Console Example
cmake_minimum_required(VERSION 3.16)

# Add esp_firevad component from parent directory
set(EXTRA_COMPONENT_DIRS "../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(firered_vad_console)

# Automatically create SPIFFS image from models if they exist
set(SPIFFS_STAGING_DIR "${CMAKE_BINARY_DIR}/spiffs_staging")

if(EXISTS "${CMAKE_SOURCE_DIR}/converted_models" OR EXISTS "${CMAKE_SOURCE_DIR}/example_wave")
    message(STATUS "Configuring SPIFFS image generation...")
    
    # Create staging directory at configure time
    file(MAKE_DIRECTORY "${SPIFFS_STAGING_DIR}")
    
    # Collect source files for dependency tracking
    file(GLOB_RECURSE FRVD_FILES "${CMAKE_SOURCE_DIR}/converted_models/*.frvd")
    file(GLOB WAV_FILES "${CMAKE_SOURCE_DIR}/example_wave/*.wav")
    
    # Count files for user feedback
    list(LENGTH FRVD_FILES model_count)
    list(LENGTH WAV_FILES wav_count)
    message(STATUS "  Models to include: ${model_count}")
    message(STATUS "  Test WAV files to include: ${wav_count}")
    
    # Create custom target to populate staging directory at BUILD time
    # This ensures files are always fresh, even if added after initial configure
    add_custom_target(spiffs_staging_populate ALL
        COMMAND ${CMAKE_COMMAND} -E remove_directory "${SPIFFS_STAGING_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SPIFFS_STAGING_DIR}"
        COMMENT "Preparing SPIFFS staging directory..."
    )
    
    # Copy model files at build time
    if(FRVD_FILES)
        foreach(frvd_file ${FRVD_FILES})
            get_filename_component(filename ${frvd_file} NAME)
            add_custom_command(TARGET spiffs_staging_populate POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${frvd_file}" "${SPIFFS_STAGING_DIR}/${filename}"
                COMMENT "  Adding model: ${filename}"
            )
        endforeach()
    endif()
    
    # Copy WAV test files at build time
    if(WAV_FILES)
        foreach(wav_file ${WAV_FILES})
            get_filename_component(filename ${wav_file} NAME)
            add_custom_command(TARGET spiffs_staging_populate POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${wav_file}" "${SPIFFS_STAGING_DIR}/${filename}"
                COMMENT "  Adding test WAV: ${filename}"
            )
        endforeach()
    endif()
    
    # Re-enable this before publishing to ensure SPIFFS is generated and flashed!
    spiffs_create_partition_image(spiffs "${SPIFFS_STAGING_DIR}" FLASH_IN_PROJECT DEPENDS spiffs_staging_populate)
    
    message(STATUS "SPIFFS image will be created at build time and flashed automatically")
    message(STATUS "Note: Files are copied fresh on every build to ensure consistency")
else()
    message(WARNING "No source files found for SPIFFS!")
    message(WARNING "  - Models directory: ${CMAKE_SOURCE_DIR}/converted_models")
    message(WARNING "  - WAV files directory: ${CMAKE_SOURCE_DIR}/example_wave")
    message(WARNING "Run 'python download_models.py' to download models")
endif()
