cmake_minimum_required(VERSION 3.22)
project(Stasis VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# DSP core: header-only, no framework dependency, reused as-is by the plugin later.
add_library(stasis_dsp INTERFACE)
target_include_directories(stasis_dsp INTERFACE dsp/include)

# Phase 0 sandbox: renders test sounds through each algorithm to WAV files.
add_executable(stasis_render tools/render/main.cpp)
target_link_libraries(stasis_render PRIVATE stasis_dsp)
target_compile_options(stasis_render PRIVATE -Wall -Wextra -O3)
