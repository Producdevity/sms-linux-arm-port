# The console underneath the game. Starlet's SDK modules provide the OS, disc, controllers, ARAM,
# audio interface, memory card, THP decoding and the GX hardware layer; sms-pc-port (MIT) provides
# the resource byte-order converters, the Gekko-exact maths and the DSP's software mixer; this port
# binds them (src/platform, src/graphics).
find_package(Threads REQUIRED)
set(sms_pc_port_platform
  "${SMS_PC_PORT}/platform/endian/endian_dispatch.cpp" "${SMS_PC_PORT}/platform/endian/endian_game.cpp"
  "${SMS_PC_PORT}/platform/endian/endian_j3d.cpp" "${SMS_PC_PORT}/platform/endian/endian_jaudio.cpp"
  "${SMS_PC_PORT}/platform/endian/endian_jpa.cpp" "${SMS_PC_PORT}/platform/endian/endian_seq.cpp"
  "${SMS_PC_PORT}/platform/misc/endian.cpp"
  "${SMS_PC_PORT}/platform/misc/msl_math.c" "${SMS_PC_PORT}/platform/misc/msl_rand.c"
  "${SMS_PC_PORT}/platform/mtx/mtx.cpp")
file(GLOB sms_platform_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/platform/*.cpp" "${CMAKE_SOURCE_DIR}/src/graphics/*.cpp" "${CMAKE_SOURCE_DIR}/src/game/*.cpp")
list(FILTER sms_platform_sources EXCLUDE REGEX "/audio_dsp\\.cpp$")
add_library(sms_platform STATIC ${sms_platform_sources} ${sms_pc_port_platform})
target_include_directories(sms_platform PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_include_directories(sms_platform PRIVATE "${SMS_PC_PORT}/platform" "${SMS_PC_PORT}/platform/misc")
target_link_libraries(sms_platform PUBLIC sms_headers starlet::os starlet::dvd starlet::pad starlet::ar
  starlet::card_sdk starlet::thp_sdk starlet::gx_fifo starlet::disc starlet::gx Threads::Threads ${CMAKE_DL_LIBS})
target_compile_options(sms_platform PRIVATE -ffunction-sections -fdata-sections -ffp-contract=off -fno-pie)
