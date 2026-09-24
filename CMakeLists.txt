# Works both as an ESP-IDF component and as a plain CMake library.
if(ESP_PLATFORM)
    idf_component_register(
        SRCS "src/eccles_rtmem.c"
        INCLUDE_DIRS "src"
    )
else()
    cmake_minimum_required(VERSION 3.13)
    project(EcclesRTLib VERSION 1.0.0 LANGUAGES C)

    add_library(eccles_rtlib src/eccles_rtmem.c)
    add_library(eccles::rtlib ALIAS eccles_rtlib)
    target_include_directories(eccles_rtlib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(eccles_rtlib PUBLIC c_std_99)
endif()
