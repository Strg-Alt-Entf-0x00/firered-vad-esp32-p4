idf_component_register(
    SRCS
        "file_protocol.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        log
        freertos
        filesystem_manager
)
