# sunshine-setup prepares gamedata/ from the user's disc image on the device.
get_filename_component(sms_setup_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# The supported mario.dol is the one the pinned decomp builds byte-identically.
set(sms_setup_dol_sha1_file "${sms_setup_root}/third_party/sms/config/GMSE01/build.sha1")
if(NOT EXISTS "${sms_setup_dol_sha1_file}")
    message(FATAL_ERROR "third_party/sms is missing; run git submodule update --init third_party/sms")
endif()
file(STRINGS "${sms_setup_dol_sha1_file}" sms_setup_dol_line REGEX "  .*/mario\\.dol$")
set(sms_setup_dol_sha1 "")
if(sms_setup_dol_line MATCHES "^([0-9a-f]+)  [^;]*$")
    set(sms_setup_dol_sha1 "${CMAKE_MATCH_1}")
endif()
string(LENGTH "${sms_setup_dol_sha1}" sms_setup_dol_sha1_length)
if(NOT sms_setup_dol_sha1_length EQUAL 40)
    message(FATAL_ERROR "${sms_setup_dol_sha1_file} does not name one mario.dol SHA-1")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${sms_setup_dol_sha1_file}")

set(sms_setup_dir "${sms_setup_root}/src/setup")
add_library(sunshine_setup STATIC "${sms_setup_dir}/prepare.cpp")
target_include_directories(sunshine_setup PUBLIC "${sms_setup_root}/src")
target_compile_definitions(sunshine_setup PRIVATE SUNSHINE_GMSE01_DOL_SHA1="${sms_setup_dol_sha1}")
target_link_libraries(sunshine_setup PUBLIC starlet::disc)

add_executable(sunshine-setup "${sms_setup_dir}/sunshine_setup.cpp")
target_link_libraries(sunshine-setup PRIVATE sunshine_setup)
