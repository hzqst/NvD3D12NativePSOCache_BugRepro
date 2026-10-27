cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED REPRO_EXE OR NOT DEFINED CACHE_FILE)
    message(FATAL_ERROR "Pass -DREPRO_EXE=<executable> and -DCACHE_FILE=<output library>.")
endif()
if(NOT DEFINED ADAPTER)
    set(ADAPTER 0)
endif()

set(common_args --adapter "${ADAPTER}" --cache "${CACHE_FILE}")
if(DEBUG_LAYER)
    list(APPEND common_args --debug)
endif()

function(run_case label mode)
    message(STATUS "=== ${label} ===")
    # Every case is a fresh process. Only cold writes the library; warm never falls back to fresh PSO creation.
    execute_process(
        COMMAND "${REPRO_EXE}" "${mode}" ${common_args} ${ARGN}
        RESULT_VARIABLE result
        TIMEOUT 60
    )
    if(NOT "${result}" MATCHES "^[01]$")
        message(FATAL_ERROR "${label}: setup/API failure or timeout (${result}); this is not a pixel-mismatch reproduction.")
    endif()
    set(case_result "${result}" PARENT_SCOPE)
endfunction()

run_case("cold: point then linear" cold)
if(NOT case_result EQUAL 0)
    message(FATAL_ERROR "Cold baseline is incorrect; cannot attribute the result to library reload.")
endif()
run_case("warm: same root and PSO order" warm)
if(NOT case_result EQUAL 0)
    message(FATAL_ERROR "Same-order control is incorrect; investigate this separately.")
endif()
run_case("warm: reverse only PSO load order" warm --reverse-psos)
if(NOT case_result EQUAL 0)
    message(FATAL_ERROR "PSO-order control is incorrect; investigate this separately.")
endif()

set(mismatches 0)
run_case("warm: reverse only root signature creation order" warm --reverse-roots)
math(EXPR mismatches "${mismatches} + ${case_result}")
run_case("warm: reverse both orders" warm --reverse-roots --reverse-psos)
math(EXPR mismatches "${mismatches} + ${case_result}")

if(mismatches GREATER 0)
    message(STATUS "REPRODUCED: controls passed; ${mismatches}/2 reversed-root cases returned incorrect pixels after successful cache loads.")
else()
    message(STATUS "NOT REPRODUCED: all five cases returned correct pixels on this adapter/driver.")
endif()
# The runner succeeds when the experiment completed. The executable returns 1 for incorrect pixels and 2 for errors.
