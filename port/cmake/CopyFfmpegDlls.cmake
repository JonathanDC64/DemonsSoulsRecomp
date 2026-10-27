# Copies FFmpeg's DLLs from FROM (a runtime build's ffmpeg-win64/bin) to TO (an exe's folder), when FROM has any.
# Run after each link of des_boot and des_host (des_copy_ffmpeg_dlls in CMakeLists.txt). file(COPY) skips a DLL
# that is already there with the same time stamp.
file(GLOB _dlls "${FROM}/avcodec-*.dll" "${FROM}/avutil-*.dll")
if(_dlls)
    file(COPY ${_dlls} DESTINATION "${TO}")
endif()
