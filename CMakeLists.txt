cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  find_program(VCPKG_EXECUTABLE vcpkg)
  if(VCPKG_EXECUTABLE)
    get_filename_component(VCPKG_ROOT ${VCPKG_EXECUTABLE} DIRECTORY)
    set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
  else()
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/vcpkg")
      message(STATUS "vcpkg not found, auto-cloning into project...")
      execute_process(COMMAND git clone https://github.com/microsoft/vcpkg.git "${CMAKE_SOURCE_DIR}/vcpkg")
    endif()
    if(UNIX)
      execute_process(COMMAND "${CMAKE_SOURCE_DIR}/vcpkg/bootstrap-vcpkg.sh")
    else()
      execute_process(COMMAND "${CMAKE_SOURCE_DIR}/vcpkg/bootstrap-vcpkg.bat")
    endif()
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
  endif()
endif()

project(titanbench LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_POSITION_INDEPENDENT_CODE OFF)

add_executable(titanbench
  src/app/main.cpp
  src/cli/args.cpp
  src/cli/report.cpp
  src/core/engine.cpp
  src/core/stats.cpp
  src/net/network.cpp
  src/net/ssl.cpp
  src/protocol/http.cpp
)

target_include_directories(titanbench PRIVATE
  src
  src/app
  src/cli
  src/core
  src/net
  src/protocol
)
target_compile_options(titanbench PRIVATE -O2 -Wall -Wextra -Wpedantic)
target_link_options(titanbench PRIVATE -static -static-libstdc++ -static-libgcc)

# Allow project-local vcpkg_installed lookup for IDE/linter and CMake package find.
set(TITANBENCH_VCPKG_INSTALLED_DIR "${CMAKE_SOURCE_DIR}/vcpkg_installed")
set(TITANBENCH_VCPKG_TRIPLET "x64-linux")
if(DEFINED VCPKG_TARGET_TRIPLET)
  set(TITANBENCH_VCPKG_TRIPLET "${VCPKG_TARGET_TRIPLET}")
endif()
set(TITANBENCH_VCPKG_INCLUDE_DIR
    "${TITANBENCH_VCPKG_INSTALLED_DIR}/${TITANBENCH_VCPKG_TRIPLET}/include")
set(TITANBENCH_VCPKG_SHARE_DIR
    "${TITANBENCH_VCPKG_INSTALLED_DIR}/${TITANBENCH_VCPKG_TRIPLET}/share")
if(EXISTS "${TITANBENCH_VCPKG_INCLUDE_DIR}")
  target_include_directories(titanbench PRIVATE "${TITANBENCH_VCPKG_INCLUDE_DIR}")
endif()
if(EXISTS "${TITANBENCH_VCPKG_SHARE_DIR}")
  list(APPEND CMAKE_PREFIX_PATH "${TITANBENCH_VCPKG_SHARE_DIR}")
endif()

find_package(wolfssl CONFIG REQUIRED)
if (TARGET wolfssl::wolfssl)
  target_link_libraries(titanbench PRIVATE wolfssl::wolfssl)
elseif (TARGET unofficial::wolfssl::wolfssl)
  target_link_libraries(titanbench PRIVATE unofficial::wolfssl::wolfssl)
else()
  message(FATAL_ERROR "wolfssl target not found in package config")
endif()