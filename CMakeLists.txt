cmake_minimum_required(VERSION 3.20)
project(argus VERSION 0.7.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---------- core NTFS + Index + Search Library ----------
add_library(argus_core STATIC
    src/core/ntfs.cpp
    src/core/index.cpp
    src/core/search.cpp
    src/core/query.cpp
    src/core/multi_index.cpp
)
target_include_directories(argus_core PUBLIC src)

if(WIN32)
    target_compile_definitions(argus_core PUBLIC
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# ---------- Icon resource shared by all binaries ----------
set(_icon_path "${CMAKE_SOURCE_DIR}/res/argus.ico")

# ---------- Options ----------
# Set OFF when Qt6 was rebuilt against the same GCC that produced libstdc++
# in %MINGW_PREFIX%/bin. Currently ON locally because MSYS2 Qt6 6.11.2 was
# built against GCC 16.1 while the toolchain has moved to 16.2 which no longer
# exports _ZSt15__get_once_callv etc. CI turns this OFF.
option(ARGUS_ABI_FIX_LIBSTDCXX
       "Overlay the vendor/runtime_libs GCC 16.1 libstdc++/libgcc DLLs" ON)

# ---------- CLI: mftdump.exe ----------
if(WIN32 AND MINGW)
    set(_mft_rc "${CMAKE_BINARY_DIR}/mftdump.rc")
    file(WRITE "${_mft_rc}"
        "1 ICON \"${_icon_path}\"\n"
        "1 24 \"${CMAKE_SOURCE_DIR}/res/mftdump.exe.manifest\"\n")
endif()

add_executable(mftdump src/mftdump/main.cpp ${_mft_rc})
target_link_libraries(mftdump PRIVATE argus_core)

if(WIN32 AND MINGW)
    target_link_options(mftdump PRIVATE
        -static -static-libgcc -static-libstdc++ -municode
    )
endif()

# ---------- Console CLI: argus-cli.exe ----------
if(WIN32 AND MINGW)
    set(_cli_rc "${CMAKE_BINARY_DIR}/argus_cli.rc")
    file(WRITE "${_cli_rc}"
        "1 ICON \"${_icon_path}\"\n")
endif()
add_executable(argus-cli src/argus_cli/main.cpp ${_cli_rc})
target_link_libraries(argus-cli PRIVATE argus_core)
if(WIN32 AND MINGW)
    target_link_options(argus-cli PRIVATE
        -static -static-libgcc -static-libstdc++ -municode
    )
endif()

# ---------- GUI: argus.exe (Qt6 Widgets) ----------
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

if(WIN32 AND MINGW)
    set(_argus_rc "${CMAKE_BINARY_DIR}/argus.rc")
    file(WRITE "${_argus_rc}"
        "1 ICON \"${_icon_path}\"\n"
        "1 24 \"${CMAKE_SOURCE_DIR}/res/argus.exe.manifest\"\n")
endif()

add_executable(argus WIN32 src/argus/gui.cpp res/argus.qrc ${_argus_rc})
target_compile_definitions(argus PRIVATE ARGUS_VERSION="${PROJECT_VERSION}")
target_link_libraries(argus PRIVATE
    argus_core
    Qt6::Widgets
    # winpthread statisch mitnehmen (siehe README ABI-Fix)
    -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic
)

if(WIN32)
    target_compile_definitions(argus PRIVATE UNICODE _UNICODE)
    if(MINGW)
        target_link_options(argus PRIVATE
            -mwindows
            -static-libgcc -static-libstdc++
        )
    endif()
endif()

# ---------- Deploy-Chain fuer argus.exe ----------

# 1) windeployqt kopiert Qt-DLLs + Plugins.
find_program(WINDEPLOYQT windeployqt6 HINTS ${CMAKE_PREFIX_PATH}/bin /mingw64/bin)
if(WINDEPLOYQT)
    add_custom_command(TARGET argus POST_BUILD
        COMMAND ${WINDEPLOYQT}
            --release --no-translations --no-system-d3d-compiler
            --no-opengl-sw --no-quick-import
            "$<TARGET_FILE:argus>"
        COMMENT "windeployqt: Qt-DLLs kopieren"
    )
endif()

# 2) copy_mingw_deps.sh loest transitive MinGW/Qt-DLL-Deps rekursiv auf.
find_program(MSYS_BASH bash HINTS "C:/msys64/usr/bin" NO_DEFAULT_PATH)
if(MSYS_BASH)
    add_custom_command(TARGET argus POST_BUILD
        COMMAND "${MSYS_BASH}"
                "${CMAKE_SOURCE_DIR}/scripts/copy_mingw_deps.sh"
                "$<TARGET_FILE:argus>"
        COMMENT "Kopiere MinGW/Qt transitive DLLs"
        VERBATIM
    )
endif()

# 3) Optional ABI-Fix: siehe Option-Definition oben.
if(ARGUS_ABI_FIX_LIBSTDCXX)
    add_custom_command(TARGET argus POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_SOURCE_DIR}/vendor/runtime_libs/libstdc++-6.dll"
                "$<TARGET_FILE_DIR:argus>/libstdc++-6.dll"
        COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_SOURCE_DIR}/vendor/runtime_libs/libgcc_s_seh-1.dll"
                "$<TARGET_FILE_DIR:argus>/libgcc_s_seh-1.dll"
        COMMENT "ABI-Fix: 16.1er libstdc++/libgcc einspielen"
    )
endif()
