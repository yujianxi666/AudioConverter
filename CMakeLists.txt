cmake_minimum_required(VERSION 3.16)
project(AudioConverter VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(AudioConverter WIN32
    src/main.cpp
    src/cli.cpp
    src/core/util.cpp
    src/core/crypto.cpp
    src/core/ncm.cpp
    src/core/audio.cpp
    src/core/decode_miniaudio.cpp
    src/core/decode_mediafoundation.cpp
    src/core/flac_encoder.cpp
    src/core/metadata.cpp
    src/core/i18n.cpp
    src/core/converter.cpp
    src/gui/theme.cpp
    src/gui/main_window.cpp
    res/app.rc
)

target_include_directories(AudioConverter PRIVATE src src/core src/gui src/third_party)
target_compile_definitions(AudioConverter PRIVATE UNICODE _UNICODE)

target_link_libraries(AudioConverter PRIVATE
    gdiplus
    mfplat
    mfreadwrite
    mfuuid
    ole32
    uuid
    shell32
    shlwapi
    dwmapi
    winmm
)

if(MINGW)
    # wWinMain entry point plus a fully self-contained executable.
    target_link_options(AudioConverter PRIVATE
        -municode -static -static-libgcc -static-libstdc++)
    target_compile_options(AudioConverter PRIVATE -municode)
elseif(MSVC)
    target_compile_options(AudioConverter PRIVATE /W4 /utf-8)
endif()
