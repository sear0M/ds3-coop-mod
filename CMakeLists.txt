cmake_minimum_required(VERSION 3.20)
project(ds3_coop_mod LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Dark Souls III is 64-bit: build with a 64-bit compiler.")
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- MinHook ---------------------------------------------------------------------------------
add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC third_party/minhook/include)

# --- Dear ImGui ------------------------------------------------------------------------------
add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp)
target_include_directories(imgui PUBLIC third_party/imgui third_party/imgui/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS UNICODE _UNICODE)

# --- The mod: dinput8.dll --------------------------------------------------------------------
add_library(dinput8 SHARED
    src/dllmain.cpp
    src/game.cpp
    src/hooks.cpp
    src/items.cpp
    src/log.cpp
    src/menu.cpp
    src/password.cpp
    src/exports.def)
set_target_properties(dinput8 PROPERTIES PREFIX "" OUTPUT_NAME "dinput8")
target_compile_definitions(dinput8 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(dinput8 PRIVATE imgui minhook d3d11 dxgi d3dcompiler bcrypt version dwmapi)

if(MSVC)
    target_compile_options(imgui PRIVATE /utf-8)
    target_compile_options(dinput8 PRIVATE /utf-8 /W3)
else()
    target_compile_options(dinput8 PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type)
    # No MinGW runtime DLLs next to the game: link libgcc/libstdc++/winpthread statically.
    target_link_options(dinput8 PRIVATE -static -static-libgcc -static-libstdc++ $<$<CONFIG:Release>:-s>)
endif()

# --- Launcher for friends: DS3CoopLauncher.exe -----------------------------------------------
set(DS3_BUILD_VERSION "dev" CACHE STRING "Version shown by the launcher (CI sets build-<number>)")
add_executable(DS3CoopLauncher WIN32 launcher/main.cpp launcher/launcher.rc)
target_compile_definitions(DS3CoopLauncher PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00
                           DS3_BUILD_VERSION="${DS3_BUILD_VERSION}")
target_link_libraries(DS3CoopLauncher PRIVATE winhttp bcrypt gdiplus dwmapi shell32 ole32 uuid version)
if(MSVC)
    target_compile_options(DS3CoopLauncher PRIVATE /utf-8 /W3)
else()
    target_compile_options(DS3CoopLauncher PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_options(DS3CoopLauncher PRIVATE -municode -static -static-libgcc -static-libstdc++
                        $<$<CONFIG:Release>:-s>)
endif()
