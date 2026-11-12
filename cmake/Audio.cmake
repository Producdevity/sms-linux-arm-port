# Audio: sms-pc-port's software DSP (MIT) mixing JAudio's voices, bound to Starlet's interrupts and
# ARAM (src/platform/audio_dsp.cpp); starlet::ai plays the mixed blocks.
add_library(sms_audio STATIC
  "${SMS_PC_PORT}/platform/audio/dsp_hle.cpp" "${SMS_PC_PORT}/platform/audio/dsp_mixer.cpp"
  "${CMAKE_SOURCE_DIR}/src/platform/audio_dsp.cpp")
target_compile_definitions(sms_audio PRIVATE SMS_AUDIO_DSP_HLE=1)
target_include_directories(sms_audio PRIVATE "${SMS_PC_PORT}/platform" "${SMS_PC_PORT}/platform/audio")
target_link_libraries(sms_audio PUBLIC sms_headers starlet::os starlet::ar starlet::ai Threads::Threads)
target_compile_options(sms_audio PRIVATE -ffunction-sections -fdata-sections -fno-pie -ffp-contract=off -w)
