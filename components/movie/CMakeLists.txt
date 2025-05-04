# Movie component CMakeLists.txt

# Register the component
idf_component_register(
    SRCS 
        "movie.cpp"
        "esp32_ffmpeg.c"  # Your existing file
    INCLUDE_DIRS 
        "."
    REQUIRES 
        "ffmpeg"          # Reference our FFmpeg component
)
