cmake_minimum_required(VERSION 3.16)
project(desktop C)

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

find_file(DESKTOP_UI_FONT
    NAMES WenQuanYiMicroHei.ttf NotoSansCJKsc-Regular.otf
          NotoSansCJK-Regular.otf
    PATHS ${FONT_PATHS} NO_DEFAULT_PATH)
find_file(DESKTOP_MONO_FONT
    NAMES WenQuanYiMicroHeiMono.ttf NotoSansMonoCJKsc-Regular.otf
          NotoSansMonoCJK-Regular.otf
    PATHS ${FONT_PATHS} NO_DEFAULT_PATH)
find_file(DESKTOP_FALLBACK_FONT
    NAMES unifont.otf unifont.ttf Unifont.otf
    PATHS ${FONT_PATHS} NO_DEFAULT_PATH)
set(FONT_CACHE "${CMAKE_BINARY_DIR}/fonts")
file(MAKE_DIRECTORY "${FONT_CACHE}")
if(NOT DESKTOP_UI_FONT)
    set(DESKTOP_UI_FONT "${FONT_CACHE}/WenQuanYiMicroHei.ttf")
    file(DOWNLOAD
        "https://github.com/notofonts/noto-cjk/raw/main/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf"
        "${DESKTOP_UI_FONT}" STATUS _font_status)
    list(GET _font_status 0 _font_code)
    if(_font_code)
        message(FATAL_ERROR "无法下载中文字体: ${_font_status}")
    endif()
endif()
if(NOT DESKTOP_MONO_FONT)
    set(DESKTOP_MONO_FONT "${FONT_CACHE}/NotoSansMonoCJKsc-Regular.otf")
    file(DOWNLOAD
        "https://github.com/notofonts/noto-cjk/raw/main/Sans/Mono/NotoSansMonoCJKsc-Regular.otf"
        "${DESKTOP_MONO_FONT}" STATUS _font_status)
    list(GET _font_status 0 _font_code)
    if(_font_code)
        message(FATAL_ERROR "无法下载等宽字体: ${_font_status}")
    endif()
endif()
if(NOT DESKTOP_FALLBACK_FONT)
    set(DESKTOP_FALLBACK_FONT "${FONT_CACHE}/unifont.otf")
    file(DOWNLOAD
        "https://unifoundry.com/pub/unifont/unifont-16.0.04/font-builds/unifont-16.0.04.otf"
        "${DESKTOP_FALLBACK_FONT}" STATUS _font_status)
    list(GET _font_status 0 _font_code)
    if(_font_code)
        set(DESKTOP_FALLBACK_FONT "${DESKTOP_UI_FONT}")
    endif()
endif()
message(STATUS "UI font: ${DESKTOP_UI_FONT}")
message(STATUS "Mono font: ${DESKTOP_MONO_FONT}")
message(STATUS "Fallback font: ${DESKTOP_FALLBACK_FONT}")

if(NOT EMSCRIPTEN)
    set(DESKTOP_COMMON_SOURCES
        src/assets.c
        src/control.c
        src/cutils.c
        src/entity.c
        src/menu.c
        src/menu_array.c
        src/netutils.c
        src/net.c
        src/object.c
        src/strmap.c
        src/user.c
        src/utils/idgroup.c
        src/utils/path.c
        src/utils/random.c
        src/utils/rawfile.c
        src/utils/signal.c
        src/utils/str.c
        src/utils/textbuffer.c)
    set(DESKTOP_CLIENT_SOURCES ${DESKTOP_COMMON_SOURCES}
        src/client.c
        src/main.c
        src/save.c
        src/ui.c
        src/view.c)
    set(DESKTOP_SERVER_SOURCES ${DESKTOP_COMMON_SOURCES}
        src/hjson.c
        src/main_server.c
        src/save.c
        src/script.c
        src/server.c)

    add_library(tinyfiledialogs STATIC
        third_party/tinyfiledialogs/tinyfiledialogs.c)
    target_include_directories(tinyfiledialogs PUBLIC
        third_party/tinyfiledialogs)
    if(MSVC)
        target_compile_options(tinyfiledialogs PRIVATE /W0)
    else()
        target_compile_options(tinyfiledialogs PRIVATE -w)
    endif()

    function(add_desktop_target DESKTOP_TARGET DESKTOP_DEFINE DESKTOP_SOURCE_FILES)
    add_executable(${DESKTOP_TARGET} ${DESKTOP_SOURCE_FILES})
    target_include_directories(${DESKTOP_TARGET} PRIVATE src third_party)
    target_compile_definitions(${DESKTOP_TARGET} PRIVATE ${DESKTOP_DEFINE}=1)
    set_target_properties(${DESKTOP_TARGET} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    add_custom_command(TARGET ${DESKTOP_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DESKTOP_UI_FONT}"
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/WenQuanYiMicroHei.ttf"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DESKTOP_MONO_FONT}"
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/WenQuanYiMicroHeiMono.ttf"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DESKTOP_FALLBACK_FONT}"
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/unifont.otf")
    target_link_libraries(${DESKTOP_TARGET} PRIVATE
        tinyfiledialogs PkgConfig::JSONC PkgConfig::LIBUV ZLIB::ZLIB)
    if(TARGET raylib)
        target_link_libraries(${DESKTOP_TARGET} PRIVATE raylib)
    else()
        target_link_libraries(${DESKTOP_TARGET} PRIVATE PkgConfig::RAYLIB)
    endif()
    if(TARGET libzip::zip)
        target_link_libraries(${DESKTOP_TARGET} PRIVATE libzip::zip)
    else()
        target_link_libraries(${DESKTOP_TARGET} PRIVATE PkgConfig::LIBZIP)
    endif()
    if(WIN32)
        target_compile_definitions(${DESKTOP_TARGET} PRIVATE
            WIN32_LEAN_AND_MEAN NOGDI NOUSER _CRT_SECURE_NO_WARNINGS)
        target_link_libraries(${DESKTOP_TARGET} PRIVATE
            ws2_32 iphlpapi userenv psapi comdlg32 ole32 shell32 uuid)
    else()
        target_link_libraries(${DESKTOP_TARGET} PRIVATE m ${CMAKE_DL_LIBS})
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            set_target_properties(${DESKTOP_TARGET} PROPERTIES
                BUILD_RPATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/raylib/lib")
        endif()
    endif()
    if(MSVC)
        target_compile_options(${DESKTOP_TARGET} PRIVATE /W4)
    else()
        target_compile_options(${DESKTOP_TARGET} PRIVATE -Wall -Wextra)
    endif()
    endfunction()
    add_desktop_target(desktop DESKTOP_CLIENT "${DESKTOP_CLIENT_SOURCES}")
    add_desktop_target(desktop_server DESKTOP_SERVER "${DESKTOP_SERVER_SOURCES}")
else()
    add_executable(desktop
        src/assets.c
        src/net.c
        src/client.c
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
        src/utils/idgroup.c
        src/utils/path.c
        src/utils/random.c
        src/utils/rawfile.c
        src/utils/signal.c
        src/utils/str.c
        src/utils/textbuffer.c
        src/view.c)
    target_include_directories(desktop PRIVATE src third_party)
    target_compile_definitions(desktop PRIVATE DESKTOP_CLIENT=1)
    if(TARGET raylib)
        target_link_libraries(desktop PRIVATE raylib)
    else()
        target_link_libraries(desktop PRIVATE PkgConfig::RAYLIB)
    endif()
    set_target_properties(desktop PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/web"
        SUFFIX ".html")
    add_custom_command(TARGET desktop PRE_LINK
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DESKTOP_UI_FONT}"
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/WenQuanYiMicroHei.ttf"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DESKTOP_MONO_FONT}"
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/WenQuanYiMicroHeiMono.ttf"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DESKTOP_FALLBACK_FONT}"
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/unifont.otf")
    add_custom_command(TARGET desktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:desktop>"
                "${CMAKE_BINARY_DIR}/web/index.html")
    target_link_options(desktop PRIVATE
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
        target_compile_options(desktop PRIVATE /W4)
    else()
        target_compile_options(desktop PRIVATE -Wall -Wextra)
    endif()
endif()
