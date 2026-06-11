cmake_minimum_required(VERSION 3.24)
project(lsmkv LANGUAGES CXX)

# --- Newest C++ standard ---
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "" FORCE)
endif()

# --- Dependencies: nlohmann/json fetched at configure time (needs internet once) ---
include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

# --- The lsmkv CLI executable ---
add_executable(lsmkv
  src/main.cpp
  src/config.cpp
  src/engine.cpp
)
target_include_directories(lsmkv PRIVATE include)
target_link_libraries(lsmkv PRIVATE nlohmann_json::nlohmann_json)

if(MSVC)
  target_compile_options(lsmkv PRIVATE /W4 /utf-8 /permissive-)
else()
  target_compile_options(lsmkv PRIVATE -Wall -Wextra -Wpedantic)
endif()
