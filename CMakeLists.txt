cmake_minimum_required(VERSION 4.1)
project(lossylab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

option(LOSSYLAB_BUILD_TESTS "Build the C++ test suite" ON)
option(LOSSYLAB_BUILD_TOOLS "Build the command-line tools" ON)
option(BUILD_PYTHON_BINDINGS "Build the Python extension module" OFF)

# ---------------------------------------------------------------------------
# FFmpeg
# ---------------------------------------------------------------------------

# FFmpeg lives outside the default prefixes, so point pkg-config at it.
set(FFMPEG_ROOT "/opt/ffmpeg" CACHE PATH "Root of the FFmpeg installation to link against")

find_package(PkgConfig REQUIRED)

if(EXISTS "${FFMPEG_ROOT}/lib/pkgconfig")
    set(ENV{PKG_CONFIG_PATH} "${FFMPEG_ROOT}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()

pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavcodec>=62.11.100
        libavformat>=62.3.100
        libavfilter>=11.4.100
        libavutil>=60.8.100
        libswscale>=9.1.100
        libswresample>=6.1.100
)

message(STATUS "FFmpeg: libavcodec ${FFMPEG_libavcodec_VERSION} from ${FFMPEG_libavcodec_LIBDIR}")

# zlib decompresses a PNG's embedded ICC profile. FFmpeg depends on it already.
find_package(ZLIB REQUIRED)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

add_library(lossylab STATIC
        src/lossylab/core/error.cpp
        src/lossylab/core/json_io.cpp
        src/lossylab/core/geometry.cpp
        src/lossylab/core/rng.cpp
        src/lossylab/core/strict.cpp
        src/lossylab/core/availability.cpp
        src/lossylab/core/codec_id.cpp
        src/lossylab/core/color_spec.cpp
        src/lossylab/core/pixel_format.cpp
        src/lossylab/core/rational.cpp
        src/lossylab/core/frame.cpp
        src/lossylab/core/record.cpp
        src/lossylab/core/kernel.cpp
        src/lossylab/core/result.cpp
        src/lossylab/detail/ff_support.cpp
        src/lossylab/env/build_info.cpp
        src/lossylab/env/capabilities.cpp
        src/lossylab/env/log.cpp
        src/lossylab/io/source.cpp
        src/lossylab/io/input_context.cpp
        src/lossylab/io/probe.cpp
        src/lossylab/io/icc_profile.cpp
        src/lossylab/io/image_container.cpp
        src/lossylab/io/jpeg_markers.cpp
        src/lossylab/io/orientation.cpp
        src/lossylab/io/source_reader.cpp
        src/lossylab/io/decode_image.cpp
        src/lossylab/io/file_result.cpp
        src/lossylab/io/read_headers.cpp
        src/lossylab/io/video_reader.cpp
        src/lossylab/convert/convert.cpp
        src/lossylab/core/pipeline_spec.cpp
        src/lossylab/resample/resize.cpp
        src/lossylab/filter/filter_graph.cpp
        src/lossylab/codec/encode.cpp
        src/lossylab/codec/encoder_plans.cpp
        src/lossylab/codec/encoder_session.cpp
        src/lossylab/motion/animate_still.cpp
        src/lossylab/measure/measure.cpp
        src/lossylab/measure/compression_history.cpp
        src/lossylab/pipeline/pipeline.cpp
)
add_library(lossylab::lossylab ALIAS lossylab)

target_include_directories(lossylab
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# FFmpeg is an implementation detail: no public header includes libav*.
target_link_libraries(lossylab PRIVATE PkgConfig::FFMPEG ZLIB::ZLIB)
lossylab_set_warnings(lossylab)
set_target_properties(lossylab PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Python bindings
# ---------------------------------------------------------------------------

if(BUILD_PYTHON_BINDINGS)
    find_package(Python 3.10 COMPONENTS Interpreter Development.Module REQUIRED)
    find_package(nanobind CONFIG REQUIRED)

    nanobind_add_module(_lossylab
            python/src/module.cpp
            python/src/json_convert.cpp
            python/src/bind_errors.cpp
            python/src/bind_core_types.cpp
            python/src/bind_record.cpp
            python/src/bind_frame.cpp
            python/src/bind_env.cpp
            python/src/bind_io.cpp
            python/src/bind_video_reader.cpp
            python/src/bind_measure.cpp
            python/src/bind_encode.cpp
            python/src/bind_file_result.cpp
            python/src/bind_convert.cpp
    )
    target_link_libraries(_lossylab PRIVATE lossylab::lossylab)

    nanobind_add_stub(_lossylab_stub
            MODULE _lossylab
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/_lossylab.pyi"
            PYTHON_PATH "$<TARGET_FILE_DIR:_lossylab>"
            DEPENDS _lossylab
    )

    install(TARGETS _lossylab LIBRARY DESTINATION lossylab)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/_lossylab.pyi" DESTINATION lossylab)
endif()

# ---------------------------------------------------------------------------
# Tools and tests
# ---------------------------------------------------------------------------

if(LOSSYLAB_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(LOSSYLAB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
