# Host tests of this port's own code. Part of the build with BUILD_TESTING; on a development
# machine they also build alone: cmake -S tests -B build/host-tests && ctest --test-dir build/host-tests
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.24)
  project(SunshineHostTests LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  enable_testing()
endif()
set(sms_src "${CMAKE_CURRENT_SOURCE_DIR}/../src")
add_executable(sms_pixel_metric_tests pixel_metric_tests.cpp "${sms_src}/graphics/pixel_metric.cpp")
target_include_directories(sms_pixel_metric_tests PRIVATE "${sms_src}")
target_compile_options(sms_pixel_metric_tests PRIVATE -Wall -Wextra -ffp-contract=off)
add_test(NAME sms_pixel_metric COMMAND sms_pixel_metric_tests)
