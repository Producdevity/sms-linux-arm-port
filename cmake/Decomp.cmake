# The game: the decompilation's game, JSystem and THPPlayer sources, and the SDK's GX, compiled
# natively. The other SDK libraries, MSL and the runtime come from Starlet, the host C library and
# src/platform.
if(NOT EXISTS "${SMS_DECOMP}/configure.py")
  message(FATAL_ERROR "No decompilation at ${SMS_DECOMP}")
endif()

execute_process(
  COMMAND "${Python3_EXECUTABLE}" -I "${SMS_PC_PORT}/tools/gen_sources.py" "${SMS_DECOMP}"
          "${CMAKE_BINARY_DIR}/decomp_sources.cmake"
  RESULT_VARIABLE sms_sources_result)
if(NOT sms_sources_result EQUAL 0)
  message(FATAL_ERROR "Could not list the decompilation's sources")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SMS_DECOMP}/configure.py")
include("${CMAKE_BINARY_DIR}/decomp_sources.cmake")

# The SDK's GX: register encoding over Starlet's write-gather pipe. GXFifo.c and GXMisc.c program
# the hardware FIFO and the pixel engine; starlet::gx_fifo and src/graphics replace them.
file(GLOB sms_gx_sources "${SMS_DECOMP}/libs/dolphin/src/gx/*.c")
list(FILTER sms_gx_sources EXCLUDE REGEX "/(GXFifo|GXMisc|GXPerf)\\.c$")
# The SDK's display list builder (GD) and its software heap and stopwatch: plain memory work.
file(GLOB sms_gd_sources "${SMS_DECOMP}/libs/dolphin/src/gd/*.c")
list(APPEND sms_gx_sources ${sms_gd_sources}
  "${SMS_DECOMP}/libs/dolphin/src/os/OSAlloc.c" "${SMS_DECOMP}/libs/dolphin/src/os/OSStopwatch.c")

set(SMS_DECOMP_INCLUDE_DIRS
  "${SMS_DECOMP}/include" "${SMS_DECOMP}/libs/dolphin/include" "${SMS_DECOMP}/libs/JSystem/include"
  "${SMS_DECOMP}/libs/THPPlayer/include" "${SMS_DECOMP}/libs/PowerPC_EABI_Support/include"
  "${SMS_DECOMP}/libs/TRK_MINNOW_DOLPHIN/include" "${SMS_DECOMP}/libs/OdemuExi2/include")

add_library(sms_headers INTERFACE)
# Order matters: this port's overrides, then sms-pc-port's (its dolphin/types.h holds 32-bit
# pointer slots on a 64-bit host), then the decompilation's own headers.
target_include_directories(sms_headers BEFORE INTERFACE
  "${CMAKE_SOURCE_DIR}/src/compat/include"
  "${SMS_PC_PORT}/src/port_include"
  "${SMS_PC_PORT}/src"
  "${SMS_PC_PORT}/platform/endian"
  ${SMS_DECOMP_INCLUDE_DIRS}
  "${SMS_STARLET_DIR}/include")
target_compile_definitions(sms_headers INTERFACE GEKKO TARGET_PC VERSION_GMSE01 BUILD_VERSION=2 NDEBUG=1)
# The SDK headers declare register parameters, which C++17 platform code may include.
target_compile_options(sms_headers INTERFACE "$<$<COMPILE_LANGUAGE:CXX>:-Wno-register>")

add_library(sms_game STATIC ${SMS_DECOMP_CXX_SOURCES} ${SMS_DECOMP_C_SOURCES} ${sms_gx_sources}
  "${CMAKE_SOURCE_DIR}/src/graphics/gx_misc.c" "${CMAKE_SOURCE_DIR}/src/compat/sdk_layout_game.cpp")
set_source_files_properties("${CMAKE_SOURCE_DIR}/src/graphics/gx_misc.c" PROPERTIES
  INCLUDE_DIRECTORIES "${SMS_DECOMP}/libs/dolphin/src/gx")
target_link_libraries(sms_game PUBLIC sms_headers)
# Decompiled code is written for MWCC: its warnings are not actionable here. It reinterprets memory
# through casts (type punning MWCC allowed), relies on wrapping signed arithmetic, and the Gekko
# never fuses a multiply-add the compiler did not write.
target_compile_options(sms_game PRIVATE
  -w -fno-strict-aliasing -fwrapv -fno-omit-frame-pointer -ffp-contract=off -fsigned-char
  -fno-stack-protector -fms-extensions -fno-pie -ffunction-sections -fdata-sections
  "$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++03;-fno-gnu-keywords;-fno-threadsafe-statics;-fno-exceptions>"
  "$<$<COMPILE_LANGUAGE:C>:-std=gnu99;-fcommon>"
  "SHELL:-include ${SMS_PC_PORT}/src/port_compat.h"
  "SHELL:-include ${CMAKE_SOURCE_DIR}/src/compat/include/sunshine_compat.h")
# MWCC built JSystem and THPPlayer's .c files as C++ (-lang=c++).
set(sms_cxx_c_sources ${SMS_DECOMP_CXX_SOURCES})
list(FILTER sms_cxx_c_sources INCLUDE REGEX "\\.c$")
set_source_files_properties(${sms_cxx_c_sources} PROPERTIES LANGUAGE CXX)
set_source_files_properties(${SMS_DECOMP_PCH_SOURCES} PROPERTIES COMPILE_OPTIONS "-include;${SMS_DECOMP}/include/SMS.pch")

# The game defines the global operator new and delete over JKRHeap. They are renamed, with every
# reference in the game library, so game code allocates from its JKR heaps and everything else
# (Starlet, the converters, the host libraries) from malloc.
find_program(SMS_OBJCOPY NAMES llvm-objcopy PATHS /opt/homebrew/opt/llvm/bin REQUIRED)
file(WRITE "${CMAKE_BINARY_DIR}/game_new_syms.txt"
  "_Znwm __sms_game_new\n_Znam __sms_game_new_array\n_ZdlPv __sms_game_delete\n_ZdaPv __sms_game_delete_array\n")
add_custom_command(TARGET sms_game POST_BUILD
  COMMAND "${SMS_OBJCOPY}" "--redefine-syms=${CMAKE_BINARY_DIR}/game_new_syms.txt" "$<TARGET_FILE:sms_game>"
  COMMENT "Renaming the game's global operator new and delete" VERBATIM)

