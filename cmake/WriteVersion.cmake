# Writes OUTPUT with the release VERSION and the source commit; unchanged text keeps the file's timestamp.
execute_process(COMMAND git -C "${SOURCE_DIR}" describe --always --dirty --exclude "*"
    OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT commit)
    set(commit "unknown commit")
endif()
set(text "extern const char sunshine_port_version[] = \"${VERSION} (${commit})\";\n")
set(old "")
if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" old)
endif()
if(NOT old STREQUAL text)
    file(WRITE "${OUTPUT}" "${text}")
endif()
