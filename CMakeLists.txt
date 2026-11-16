cmake_minimum_required(VERSION 3.24)
project(SunshineLinuxArm VERSION 0.0.1 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(CTest)
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)

# Probe builds only: development hooks (src/dev). The package script refuses such builds.
option(SMS_DEV_TOOLS "Build the development hooks" OFF)

# The generated, patched decompilation. SMS_DECOMP_DIR overrides it with a
# working tree during patch development.
set(SMS_DECOMP_DIR "" CACHE PATH "Use this decompilation tree instead of preparing the pinned one")
if(SMS_DECOMP_DIR)
  set(SMS_DECOMP "${SMS_DECOMP_DIR}")
else()
  set(SMS_DECOMP "${CMAKE_BINARY_DIR}/sms-decomp")
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/prepare_sms.py" --output "${SMS_DECOMP}"
    RESULT_VARIABLE prepare_result)
  if(NOT prepare_result EQUAL 0)
    message(FATAL_ERROR "Could not prepare the pinned decompilation")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/third_party/sms.lock.json"
    "${CMAKE_SOURCE_DIR}/tools/prepare_sms.py"
    "${CMAKE_SOURCE_DIR}/patches/sms/series")
  file(GLOB sms_patches CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/patches/sms/*.patch")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${sms_patches})
endif()
# sms-pc-port (MIT): the compatibility headers, resource converters and Gekko maths the
# patch series relies on.
set(SMS_PC_PORT "${CMAKE_SOURCE_DIR}/third_party/sms-pc-port")

foreach(module Setup Profile Starlet Decomp Platform Audio Application)
  include("${CMAKE_SOURCE_DIR}/cmake/${module}.cmake")
endforeach()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
