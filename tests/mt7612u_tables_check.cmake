# Runs tools/extract_mt7612u_tables.py --check, but only when the pinned mt76
# submodule is actually present. A fresh clone without `git submodule update`
# would otherwise fail a test for a missing input rather than a real drift.
# The submodule's presence is decided at configure time (CMakeLists.txt only
# registers this test when reference/mt76 is checked out), so reaching here
# without it means it vanished between configure and test: a real failure, not
# a skip. It used to return 0, which reported "checked nothing" as a PASS.
if(NOT EXISTS "${REF}/mt76x2/init.c")
    message(FATAL_ERROR
        "reference/mt76 is missing at test time (it was present at configure)")
endif()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_FOUND)
    message(FATAL_ERROR "python3 is required to verify the generated tables")
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" "${SCRIPT}" --check
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "extract_mt7612u_tables.py --check failed: ${out}${err}")
endif()
message(STATUS "${out}")
