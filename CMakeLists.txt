cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
    find_package(obs-frontend-api REQUIRED)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

# ----------------------------------------------------------
# Task 3.3a / 3.5: FFmpeg from OBS prebuilt deps
#
# 優先使用 find_package(FFmpeg)。若失敗，退回 build 目錄下載的
# obs-deps（由 buildspec.json 決定版本），而不是硬編碼 C:/obs-deps。
#
# 這樣 plugin link 的 FFmpeg 版本會與 OBS 執行時載入的版本一致，
# 不需要額外複製 FFmpeg DLL 到 plugin 目錄。
# ----------------------------------------------------------

if(FFmpeg_FOUND)
    message(STATUS "Relay: using find_package(FFmpeg) — version ${FFmpeg_VERSION}")
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
        FFmpeg::avcodec
        FFmpeg::avutil
        FFmpeg::swscale
    )
else()
    # obs-deps 版本必須與 buildspec.json 的 prebuilt.version 一致。
    set(OBS_DEPS_VERSION "2026-06-25")

    # build 目錄下載的 obs-deps 路徑。
    set(OBS_DEPS_ROOT
        "${CMAKE_SOURCE_DIR}/.deps/obs-deps-${OBS_DEPS_VERSION}-x64"
        CACHE PATH "OBS prebuilt deps root")

    set(FFMPEG_INC "${OBS_DEPS_ROOT}/include")
    set(FFMPEG_LIB "${OBS_DEPS_ROOT}/lib")

    if(NOT EXISTS "${FFMPEG_INC}/libavcodec/avcodec.h")
        message(FATAL_ERROR
            "FFmpeg not found under ${OBS_DEPS_ROOT}. "
            "Expected ${FFMPEG_INC}/libavcodec/avcodec.h. "
            "請確認 buildspec.json 的 prebuilt.version 為 ${OBS_DEPS_VERSION}，"
            "並重新執行 cmake configure 讓 obs-deps 下載完成。")
    endif()

    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${FFMPEG_INC}")
    target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE "${FFMPEG_LIB}")

    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
        "${FFMPEG_LIB}/avcodec.lib"
        "${FFMPEG_LIB}/avutil.lib"
        "${FFMPEG_LIB}/swscale.lib"
    )

    message(STATUS "Relay: linked FFmpeg from ${OBS_DEPS_ROOT}")
endif()

if(ENABLE_QT)
    find_package(Qt6 COMPONENTS Widgets Core)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
    target_compile_options(
        ${CMAKE_PROJECT_NAME}
        PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
    )
    set_target_properties(
        ${CMAKE_PROJECT_NAME}
        PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
    )
endif()

target_sources(
    ${CMAKE_PROJECT_NAME}
    PRIVATE
        src/plugin-main.cpp
        src/relay-preview-window.cpp
        src/relay-preview-window.hpp
        src/h264-ring-buffer.cpp
        src/h264-ring-buffer.hpp
        src/h264-decoder.cpp
        src/h264-decoder.hpp
        src/playback-session.cpp
        src/playback-session.hpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
