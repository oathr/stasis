cmake_minimum_required(VERSION 3.22)
project(Stasis VERSION 0.1.6 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(STASIS_BUILD_RENDER "Build the offline render sandbox" ON)
option(STASIS_BUILD_PLUGIN "Build the plugin (fetches JUCE)" ON)

# DSP core: header-only, no framework dependency, shared by the sandbox and the plugin.
add_library(stasis_dsp INTERFACE)
target_include_directories(stasis_dsp INTERFACE dsp/include)

# Phase 0 sandbox: renders test sounds through each algorithm to WAV files.
if(STASIS_BUILD_RENDER)
  add_executable(stasis_render tools/render/main.cpp)
  target_link_libraries(stasis_render PRIVATE stasis_dsp)
  if(NOT MSVC)
    target_compile_options(stasis_render PRIVATE -Wall -Wextra -O3)
  endif()
endif()

if(STASIS_BUILD_PLUGIN)
  add_subdirectory(plugin)
endif()
