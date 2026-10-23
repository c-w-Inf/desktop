cmake_minimum_required(VERSION 3.16)
project(image_canvas C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)
pkg_check_modules(LIBUV REQUIRED IMPORTED_TARGET libuv)
find_package(ZLIB REQUIRED)

find_package(raylib CONFIG QUIET)
if(NOT TARGET raylib AND NOT WIN32)
    find_package(raylib CONFIG QUIET
        PATHS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/raylib/lib/cmake/raylib"
        NO_DEFAULT_PATH)
endif()
if(NOT TARGET raylib)
    pkg_check_modules(RAYLIB REQUIRED IMPORTED_TARGET raylib)
endif()

find_package(libzip CONFIG QUIET)
if(NOT TARGET libzip::zip AND NOT WIN32)
    find_package(libzip CONFIG QUIET
        PATHS "${CMAKE_CURRENT_SOURCE_DIR}/.local/libzip/lib/cmake/libzip"
        NO_DEFAULT_PATH)
endif()
if(NOT TARGET libzip::zip)
    pkg_check_modules(LIBZIP REQUIRED IMPORTED_TARGET libzip)
endif()


file(GLOB IMAGE_CANVAS_SOURCES CONFIGURE_DEPENDS src/*.c)

add_library(tinyfiledialogs STATIC
    third_party/tinyfiledialogs/tinyfiledialogs.c)
target_include_directories(tinyfiledialogs PUBLIC
    third_party/tinyfiledialogs)
if(MSVC)
    target_compile_options(tinyfiledialogs PRIVATE /W0)
else()
    target_compile_options(tinyfiledialogs PRIVATE -w)
endif()

add_executable(image_canvas ${IMAGE_CANVAS_SOURCES})
target_include_directories(image_canvas PRIVATE
    src
    third_party)
if(NOT WIN32)
    target_include_directories(image_canvas PRIVATE third_party/raylib/include)
endif()
set_target_properties(image_canvas PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(image_canvas PRIVATE
    tinyfiledialogs
    PkgConfig::JSONC
    PkgConfig::LIBUV
    ZLIB::ZLIB)
if(TARGET raylib)
    target_link_libraries(image_canvas PRIVATE raylib)
else()
    target_link_libraries(image_canvas PRIVATE PkgConfig::RAYLIB)
endif()
if(TARGET libzip::zip)
    target_link_libraries(image_canvas PRIVATE libzip::zip)
else()
    target_link_libraries(image_canvas PRIVATE PkgConfig::LIBZIP)
endif()

if(WIN32)
    target_compile_definitions(image_canvas PRIVATE
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(image_canvas PRIVATE
        ws2_32 iphlpapi userenv psapi comdlg32 ole32 shell32 uuid)
else()
    target_link_libraries(image_canvas PRIVATE m ${CMAKE_DL_LIBS})
    set_target_properties(image_canvas PROPERTIES
        BUILD_RPATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/raylib/lib")
endif()

if(MSVC)
    target_compile_options(image_canvas PRIVATE /W4)
else()
    target_compile_options(image_canvas PRIVATE -Wall -Wextra)
endif()
