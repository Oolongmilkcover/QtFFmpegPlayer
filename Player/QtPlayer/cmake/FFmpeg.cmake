# ============================ FFmpeg 依赖 ============================
# 统一封装成 ffmpeg_deps INTERFACE 目标，主程序与所有测试目标共用。
# 这样"头文件路径 + 库"是一次性定义的，新加 target 只要链接 ffmpeg_deps 就行，
# 不会出现"忘了加 include 目录 → C1083: libavformat/avformat.h: No such file"。
add_library(ffmpeg_deps INTERFACE)

if(WIN32)
    set(FFMPEG_PATH "C:/Program Files/ffmpeg/ffmpeg8.1"
        CACHE PATH "FFmpeg 安装目录（需含 include/、lib/、bin/），可用 -DFFMPEG_PATH=... 覆盖")

    target_include_directories(ffmpeg_deps INTERFACE ${FFMPEG_PATH}/include)

    # 采集设备（dshow）用 avdevice；播放器本身不用，但推流端以后会要
    foreach(ff_lib avcodec avformat avdevice avutil swresample swscale avfilter)
        find_library(FFMPEG_${ff_lib}_LIB ${ff_lib}
            PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
        if(NOT FFMPEG_${ff_lib}_LIB)
            message(FATAL_ERROR
                "找不到 FFmpeg 库 ${ff_lib}（在 ${FFMPEG_PATH}/lib 下）。"
                "请用 -DFFMPEG_PATH=<安装目录> 指定。")
        endif()
        target_link_libraries(ffmpeg_deps INTERFACE ${FFMPEG_${ff_lib}_LIB})
    endforeach()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavcodec libavformat libavdevice libavutil libswresample libswscale libavfilter)
    # PkgConfig::FFMPEG 自带 include 路径与链接参数
    target_link_libraries(ffmpeg_deps INTERFACE PkgConfig::FFMPEG)
endif()
