# Runs tools/extract_mt7612u_tables.py --check, but only when the pinned mt76
# submodule is actually present. A fresh clone without `git submodule update`
# would otherwise fail a test for a missing input rather than a real drift.
if(NOT EXISTS "${REF}/mt76x2/init.c")
    message(STATUS "reference/mt76 not checked out - skipping table check")
    return()
endif()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_FOUND)
    message(STATUS "no python3 - skipping table check")
    return()
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" "${SCRIPT}" --check
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "extract_mt7612u_tables.py --check failed: ${out}${err}")
endif()
message(STATUS "${out}")
