cmake_minimum_required(VERSION 3.20)
project(gbaemu CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
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

find_package(SDL2 CONFIG)
if(SDL2_FOUND)
  add_executable(gbaemu src/frontend/main.cpp)
  target_link_libraries(gbaemu PRIVATE gba_core SDL2::SDL2 SDL2::SDL2main)
  if(MSVC)
    target_compile_options(gbaemu PRIVATE /utf-8)
  endif()
else()
  message(WARNING "SDL2 não encontrado: só o núcleo e os testes serão compilados.")
endif()
