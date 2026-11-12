# The game executable needs the target's SDL2 and OpenGL ES 3.1 (Linux only).
if(NOT SMS_GLES OR APPLE)
  return()
endif()
find_path(SMS_SDL_INCLUDE_DIR SDL.h PATH_SUFFIXES SDL2 REQUIRED)
find_library(SMS_SDL_LIBRARY SDL2 REQUIRED)
add_custom_target(sunshine_version_source
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DVERSION=${PROJECT_VERSION}"
    "-DOUTPUT=${CMAKE_BINARY_DIR}/sunshine_version.cpp" -P "${CMAKE_CURRENT_LIST_DIR}/WriteVersion.cmake"
  BYPRODUCTS "${CMAKE_BINARY_DIR}/sunshine_version.cpp")
add_library(sunshine_host_app STATIC src/application/host_app.cpp)
target_include_directories(sunshine_host_app PRIVATE "${SMS_SDL_INCLUDE_DIR}")
target_link_libraries(sunshine_host_app PUBLIC sms_platform sms_audio starlet::gx_gles starlet::gx_fifo starlet::ai
  starlet::card_sdk starlet::dvd starlet::pad starlet::host "${SMS_SDL_LIBRARY}" ${CMAKE_DL_LIBS})
add_executable(sunshine src/application/main.cpp "${CMAKE_BINARY_DIR}/sunshine_version.cpp")
add_dependencies(sunshine sunshine_version_source)
target_include_directories(sunshine PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(sunshine PRIVATE sunshine_host_app sms_platform sms_audio sms_game starlet::gx_gles
  "${SMS_SDL_LIBRARY}" ${CMAKE_DL_LIBS})
# Without PIE the executable's image sits low, below the game's memory at 0x80000000
# (src/platform/memory.hpp). Fatal-signal reports name the build ID.
# sms-pc-port's misc/endian.cpp reaches its converters through a weak reference, which does not
# pull them from the archive by itself.
target_link_options(sunshine PRIVATE "LINKER:--undefined=port_endian_resource")
target_link_options(sunshine PRIVATE -no-pie -Wl,--build-id=sha1 -Wl,--gc-sections -Wl,--error-limit=0)
set_target_properties(sunshine PROPERTIES POSITION_INDEPENDENT_CODE OFF)
