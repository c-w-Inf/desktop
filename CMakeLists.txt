cmake_minimum_required(VERSION 3.16)
project(image_canvas C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT EMSCRIPTEN)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)
    pkg_check_modules(LIBUV REQUIRED IMPORTED_TARGET libuv)
    find_package(ZLIB REQUIRED)
endif()

find_package(raylib CONFIG QUIET)
if(NOT TARGET raylib)
    include(FetchContent)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib.git
        GIT_TAG 6.2
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(raylib)
endif()
if(NOT TARGET raylib)
    pkg_check_modules(RAYLIB REQUIRED IMPORTED_TARGET raylib)
endif()
if(TARGET raylib)
    get_target_property(_raylib_imported raylib IMPORTED)
    if(NOT _raylib_imported)
        target_compile_definitions(raylib PUBLIC
            SUPPORT_FILEFORMAT_JPG=1
            SUPPORT_FILEFORMAT_TGA=1
            SUPPORT_FILEFORMAT_PSD=1)
    endif()
endif()

if(NOT EMSCRIPTEN)
    find_package(libzip CONFIG QUIET)
    if(NOT TARGET libzip::zip AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_package(libzip CONFIG QUIET
            PATHS "${CMAKE_CURRENT_SOURCE_DIR}/.local/libzip/lib/cmake/libzip"
            NO_DEFAULT_PATH)
    endif()
    if(NOT TARGET libzip::zip)
        pkg_check_modules(LIBZIP REQUIRED IMPORTED_TARGET libzip)
    endif()
endif()


# 字体：优先使用系统字体；没有时下载到构建目录，再由 post-build 复制到 assets。
set(FONT_PATHS
    "${CMAKE_CURRENT_SOURCE_DIR}/assets"
    /usr/share/fonts
    /usr/share/fonts/truetype
    /usr/share/fonts/opentype
    /usr/share/fonts/truetype/wqy
    /usr/share/fonts/wenquanyi/wqy-microhei
    /usr/share/fonts/truetype/unifont
    /usr/share/fonts/opentype/unifont
    /usr/local/share/fonts
    /Library/Fonts
    /System/Library/Fonts
    /System/Library/Fonts/Supplemental
    "$ENV{WINDIR}/Fonts")

find_file(IMAGE_CANVAS_UI_FONT
    NAMES WenQuanYiMicroHei.ttf NotoSansCJKsc-Regular.otf
          NotoSansCJK-Regular.otf
    PATHS ${FONT_PATHS} NO_DEFAULT_PATH)
find_file(IMAGE_CANVAS_MONO_FONT
    NAMES WenQuanYiMicroHeiMono.ttf NotoSansMonoCJKsc-Regular.otf
          NotoSansMonoCJK-Regular.otf
    PATHS ${FONT_PATHS} NO_DEFAULT_PATH)
find_file(IMAGE_CANVAS_FALLBACK_FONT
    NAMES unifont.otf unifont.ttf Unifont.otf
    PATHS ${FONT_PATHS} NO_DEFAULT_PATH)
set(FONT_CACHE "${CMAKE_BINARY_DIR}/fonts")
file(MAKE_DIRECTORY "${FONT_CACHE}")
if(NOT IMAGE_CANVAS_UI_FONT)
    set(IMAGE_CANVAS_UI_FONT "${FONT_CACHE}/WenQuanYiMicroHei.ttf")
    file(DOWNLOAD
        "https://github.com/notofonts/noto-cjk/raw/main/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf"
        "${IMAGE_CANVAS_UI_FONT}" STATUS _font_status)
    list(GET _font_status 0 _font_code)
    if(_font_code)
        message(FATAL_ERROR "无法下载中文字体: ${_font_status}")
    endif()
endif()
if(NOT IMAGE_CANVAS_MONO_FONT)
    set(IMAGE_CANVAS_MONO_FONT "${FONT_CACHE}/NotoSansMonoCJKsc-Regular.otf")
    file(DOWNLOAD
        "https://github.com/notofonts/noto-cjk/raw/main/Sans/Mono/NotoSansMonoCJKsc-Regular.otf"
        "${IMAGE_CANVAS_MONO_FONT}" STATUS _font_status)
    list(GET _font_status 0 _font_code)
    if(_font_code)
        message(FATAL_ERROR "无法下载等宽字体: ${_font_status}")
    endif()
endif()
if(NOT IMAGE_CANVAS_FALLBACK_FONT)
    set(IMAGE_CANVAS_FALLBACK_FONT "${FONT_CACHE}/unifont.otf")
    file(DOWNLOAD
        "https://unifoundry.com/pub/unifont/unifont-16.0.04/font-builds/unifont-16.0.04.otf"
        "${IMAGE_CANVAS_FALLBACK_FONT}" STATUS _font_status)
    list(GET _font_status 0 _font_code)
    if(_font_code)
        set(IMAGE_CANVAS_FALLBACK_FONT "${IMAGE_CANVAS_UI_FONT}")
    endif()
endif()
message(STATUS "UI font: ${IMAGE_CANVAS_UI_FONT}")
message(STATUS "Mono font: ${IMAGE_CANVAS_MONO_FONT}")
message(STATUS "Fallback font: ${IMAGE_CANVAS_FALLBACK_FONT}")

if(NOT EMSCRIPTEN)
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
    target_include_directories(image_canvas PRIVATE src third_party)
    set_target_properties(image_canvas PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    add_custom_command(TARGET image_canvas POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${IMAGE_CANVAS_UI_FONT}"
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/WenQuanYiMicroHei.ttf"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${IMAGE_CANVAS_MONO_FONT}"
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/WenQuanYiMicroHeiMono.ttf"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${IMAGE_CANVAS_FALLBACK_FONT}"
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/unifont.otf")
    target_link_libraries(image_canvas PRIVATE
        tinyfiledialogs PkgConfig::JSONC PkgConfig::LIBUV ZLIB::ZLIB)
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
            WIN32_LEAN_AND_MEAN NOGDI NOUSER _CRT_SECURE_NO_WARNINGS)
        target_link_libraries(image_canvas PRIVATE
            ws2_32 iphlpapi userenv psapi comdlg32 ole32 shell32 uuid)
    else()
        target_link_libraries(image_canvas PRIVATE m ${CMAKE_DL_LIBS})
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            set_target_properties(image_canvas PROPERTIES
                BUILD_RPATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/raylib/lib")
        endif()
    endif()
    if(MSVC)
        target_compile_options(image_canvas PRIVATE /W4)
    else()
        target_compile_options(image_canvas PRIVATE -Wall -Wextra)
    endif()
else()
    add_executable(image_canvas
        src/assets.c
        src/client.c
        src/client_ui.c
        src/cutils.c
        src/entity.c
        src/main_web.c
        src/menu.c
        src/menu_array.c
        src/netutils.c
        src/object.c
        src/strmap.c
        src/ui.c
        src/user.c
        src/utils.c
        src/view.c)
    target_include_directories(image_canvas PRIVATE src third_party)
    if(TARGET raylib)
        target_link_libraries(image_canvas PRIVATE raylib)
    else()
        target_link_libraries(image_canvas PRIVATE PkgConfig::RAYLIB)
    endif()
    set_target_properties(image_canvas PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/web"
        SUFFIX ".html")
    add_custom_command(TARGET image_canvas PRE_LINK
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${IMAGE_CANVAS_UI_FONT}"
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/WenQuanYiMicroHei.ttf"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${IMAGE_CANVAS_MONO_FONT}"
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/WenQuanYiMicroHeiMono.ttf"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${IMAGE_CANVAS_FALLBACK_FONT}"
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/unifont.otf")
    add_custom_command(TARGET image_canvas POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:image_canvas>"
                "${CMAKE_BINARY_DIR}/web/index.html")
    target_link_options(image_canvas PRIVATE
        -sUSE_GLFW=3
        --pre-js "${CMAKE_CURRENT_SOURCE_DIR}/src/web_font_cache.js"
        -lwebsocket.js
        -sASYNCIFY
        -sASYNCIFY_STACK_SIZE=1048576
        -sALLOW_MEMORY_GROWTH
        -sMAXIMUM_MEMORY=2147483648
        -sFORCE_FILESYSTEM=1
        -sENVIRONMENT=web
        -sEXPORTED_RUNTIME_METHODS=ccall)
    if(MSVC)
        target_compile_options(image_canvas PRIVATE /W4)
    else()
        target_compile_options(image_canvas PRIVATE -Wall -Wextra)
    endif()
endif()
