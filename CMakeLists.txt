cmake_minimum_required(VERSION 3.20)
project(NiGBA CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(NIGBA_CONSOLE "Mostra o terminal junto com a janela (útil para desenvolver)" ON)
option(NIGBA_STATIC "Executável único, sem DLLs (para distribuir)" OFF)
if(NIGBA_STATIC AND MSVC)
  # Embute as bibliotecas do C++ no executável: quem recebe não precisa instalar nada.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Núcleo sem dependências: pode ser testado e reaproveitado sem SDL.
add_library(gba_core
  src/core/bus.cpp
  src/core/cartridge.cpp
  src/core/cpu.cpp
  src/core/cpu_thumb.cpp
  src/core/hle_bios.cpp
  src/core/gba.cpp
  src/core/dma.cpp
  src/core/backup.cpp
  src/core/timers.cpp
  src/core/analysis.cpp
  src/core/apu.cpp
  src/core/rtc.cpp
  src/core/ppu.cpp)
target_include_directories(gba_core PUBLIC src)
if(MSVC)
  target_compile_options(gba_core PRIVATE /W4 /utf-8)
else()
  target_compile_options(gba_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_executable(gba_tests tests/test_core.cpp)
target_link_libraries(gba_tests PRIVATE gba_core)
if(MSVC)
  target_compile_options(gba_tests PRIVATE /utf-8)
endif()
add_test(NAME core COMMAND gba_tests)

add_executable(gba_cpu_tests tests/test_cpu.cpp)
target_link_libraries(gba_cpu_tests PRIVATE gba_core)
if(MSVC)
  target_compile_options(gba_cpu_tests PRIVATE /utf-8)
endif()
add_test(NAME cpu COMMAND gba_cpu_tests)

add_executable(gba_system_tests tests/test_system.cpp)
target_link_libraries(gba_system_tests PRIVATE gba_core)
if(MSVC)
  target_compile_options(gba_system_tests PRIVATE /utf-8)
endif()
add_test(NAME system COMMAND gba_system_tests)

add_executable(gba_video_tests tests/test_video.cpp)
target_link_libraries(gba_video_tests PRIVATE gba_core)
if(MSVC)
  target_compile_options(gba_video_tests PRIVATE /utf-8)
endif()
add_test(NAME video COMMAND gba_video_tests)

add_executable(gba_audio_tests tests/test_audio.cpp)
target_link_libraries(gba_audio_tests PRIVATE gba_core)
if(MSVC)
  target_compile_options(gba_audio_tests PRIVATE /utf-8)
endif()
add_test(NAME audio COMMAND gba_audio_tests)

add_executable(gba_state_tests tests/test_state.cpp)
target_link_libraries(gba_state_tests PRIVATE gba_core)
if(MSVC)
  target_compile_options(gba_state_tests PRIVATE /utf-8)
endif()
add_test(NAME state COMMAND gba_state_tests)

# ROMs de teste do jsmolka (opcionais: baixadas pelo GitHub Actions; puladas se ausentes)
add_executable(gba_rom_tests tests/test_roms.cpp)
target_link_libraries(gba_rom_tests PRIVATE gba_core)
if(MSVC)
  target_compile_options(gba_rom_tests PRIVATE /utf-8)
endif()
add_test(NAME roms COMMAND gba_rom_tests ${CMAKE_SOURCE_DIR}/tests/roms)

find_package(SDL2 CONFIG)
if(SDL2_FOUND)
  # Dear ImGui: baixado do repositório oficial numa versão fixa e compilado junto
  # (não depende das opções do pacote do vcpkg, que mudam de nome entre versões).
  include(FetchContent)
  FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(imgui)
  add_library(imgui_sdl STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp)
  target_include_directories(imgui_sdl PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  # SDL2::SDL2 na compilação normal (DLL); SDL2::SDL2-static na de distribuição.
  set(NIGBA_SDL $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>)
  target_link_libraries(imgui_sdl PUBLIC ${NIGBA_SDL})

  add_executable(NiGBA
    src/frontend/main.cpp
    src/frontend/app.cpp
    src/frontend/ui.cpp
    src/frontend/config.cpp
    src/frontend/diagnostics.cpp
    src/frontend/debug_ui.cpp
    src/frontend/file_dialog.cpp)
  target_link_libraries(NiGBA PRIVATE gba_core imgui_sdl ${NIGBA_SDL} $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>)
  if(WIN32)
    target_sources(NiGBA PRIVATE src/frontend/nigba.rc) # ícone do executável
  endif()
  if(NOT NIGBA_CONSOLE)
    set_target_properties(NiGBA PROPERTIES WIN32_EXECUTABLE TRUE) # só a janela, sem o terminal
  endif()
  if(WIN32)
    target_link_libraries(NiGBA PRIVATE comdlg32) # diálogo "Abrir arquivo"
  endif()
  if(MSVC)
    target_compile_options(NiGBA PRIVATE /utf-8)
  endif()
else()
  message(WARNING "SDL2 não encontrado: só o núcleo e os testes serão compilados.")
endif()
