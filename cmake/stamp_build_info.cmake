# Regenerate build_info.h from the current git HEAD. Invoked via `cmake -P` both
# at configure time and as a build-time step, so `bus version` reflects the
# source actually compiled rather than the last configure. configure_file is
# copy-if-different: an unchanged commit leaves the header untouched, so no
# recompile is triggered. Expects -DSRC_DIR, -DIN_FILE, -DOUT_FILE.
execute_process(
    COMMAND git -C "${SRC_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE BUS_BUILD_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT BUS_BUILD_COMMIT)
    set(BUS_BUILD_COMMIT "unknown")
endif()
configure_file("${IN_FILE}" "${OUT_FILE}" @ONLY)
