add_subdirectory(components/mp4dec)
add_subdirectory(components/h264dec)

target_link_libraries(${COMPONENT_LIB_NAME} PRIVATE mp4dec h264dec)
