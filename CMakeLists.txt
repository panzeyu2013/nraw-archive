cmake_minimum_required(VERSION 3.16)
project(nraw_archive VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale)
find_package(Threads REQUIRED)

if(APPLE)
    # macOS 自带 OpenCL.framework（头文件在框架内，无需额外依赖）
    find_library(OPENCL_LIB OpenCL)
    if(NOT OPENCL_LIB)
        message(FATAL_ERROR "缺少 OpenCL.framework")
    endif()
else()
    find_path(OPENCL_HEADER_DIR CL/opencl.h)
    if(NOT OPENCL_HEADER_DIR)
        message(FATAL_ERROR "缺少 OpenCL 头文件（GPU 路径编译需要）: sudo apt install opencl-headers")
    endif()
endif()

set(R3D_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/R3DSDKv9_2_1" CACHE PATH
    "Path to the RED R3DSDK v9.2.1 directory (see README for where to place it)")
if(NOT EXISTS "${R3D_SDK_ROOT}/Include/R3DSDK.h")
    message(FATAL_ERROR
        "R3D SDK not found at ${R3D_SDK_ROOT}.\n"
        "The SDK is not bundled with this repository (see README). Place the "
        "R3DSDK v9.2.1 folder at <repo>/R3DSDKv9_2_1, or point CMake at it:\n"
        "  cmake -B build -DR3D_SDK_ROOT=/path/to/R3DSDKv9_2_1")
endif()

add_executable(nraw_archive
    src/main.cpp
    src/sdk_decode.cpp
    src/gpu_process.cpp
    src/av_encode.cpp
    src/audio.cpp
    src/sha256.cpp
)

target_include_directories(nraw_archive PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${R3D_SDK_ROOT}/Include
)
if(APPLE)
    # OpenCL.framework 头文件位于框架顶层（无 CL/ 子目录），而本代码与
    # R3DSDK 头文件都使用 #include <CL/opencl.h>。在 build 目录生成兼容布局
    # （CL/ 与 OpenCL/ 两份拷贝——opencl.h 内部用 <OpenCL/...> 引用自身）。
    get_filename_component(OPENCL_FW_HEADERS "${OPENCL_LIB}/Headers" ABSOLUTE)
    set(OPENCL_COMPAT_DIR "${CMAKE_BINARY_DIR}/opencl-headers")
    file(MAKE_DIRECTORY "${OPENCL_COMPAT_DIR}/CL" "${OPENCL_COMPAT_DIR}/OpenCL")
    file(GLOB OPENCL_FW_HEADER_FILES "${OPENCL_FW_HEADERS}/*.h")
    file(COPY ${OPENCL_FW_HEADER_FILES} DESTINATION "${OPENCL_COMPAT_DIR}/CL")
    file(COPY ${OPENCL_FW_HEADER_FILES} DESTINATION "${OPENCL_COMPAT_DIR}/OpenCL")
    target_include_directories(nraw_archive PRIVATE "${OPENCL_COMPAT_DIR}")
endif()

if(APPLE)
    # macOS：R3DSDK mac64 通用二进制（x86_64 + arm64）
    target_link_libraries(nraw_archive PRIVATE
        PkgConfig::FFMPEG
        ${R3D_SDK_ROOT}/Lib/mac64/libR3DSDK-libcpp.a
        Threads::Threads
        m
    )
    add_custom_command(TARGET nraw_archive POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${R3D_SDK_ROOT}/Redistributable/mac/REDR3D.dylib
                ${R3D_SDK_ROOT}/Redistributable/mac/REDDecoder.dylib
                ${R3D_SDK_ROOT}/Redistributable/mac/REDOpenCL.dylib
                ${R3D_SDK_ROOT}/Redistributable/mac/REDMetal.dylib
                $<TARGET_FILE_DIR:nraw_archive>
        COMMENT "拷贝 R3D SDK 动态库到可执行文件目录")
else()
    target_link_libraries(nraw_archive PRIVATE
        PkgConfig::FFMPEG
        ${R3D_SDK_ROOT}/Lib/linux64/libR3DSDK-cpp11.a
        dl
        pthread
        m
    )
    target_link_options(nraw_archive PRIVATE -no-pie)
    add_custom_command(TARGET nraw_archive POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${R3D_SDK_ROOT}/Redistributable/linux/REDR3D-x64.so
                ${R3D_SDK_ROOT}/Redistributable/linux/REDDecoder-x64.so
                ${R3D_SDK_ROOT}/Redistributable/linux/REDOpenCL-x64.so
                ${R3D_SDK_ROOT}/Redistributable/linux/REDCuda-x64.so
                $<TARGET_FILE_DIR:nraw_archive>
        COMMENT "拷贝 R3D SDK 动态库到可执行文件目录")
endif()

target_compile_options(nraw_archive PRIVATE -O2 -Wall -Wextra)

include(CTest)
if(BUILD_TESTING)
    add_executable(nraw_test_encode
        tests/test_encode.cpp
        src/av_encode.cpp
        src/audio.cpp
        src/sha256.cpp
    )

    target_include_directories(nraw_test_encode PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_link_libraries(nraw_test_encode PRIVATE PkgConfig::FFMPEG Threads::Threads)
    target_compile_options(nraw_test_encode PRIVATE -O2 -Wall -Wextra)

    add_test(NAME encode_roundtrip
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_roundtrip.sh $<TARGET_FILE:nraw_test_encode>)
    # 死锁/挂死回归时不让 CI 无限等待
    set_tests_properties(encode_roundtrip PROPERTIES TIMEOUT 600)
endif()
