add_executable(ocgbridge_unit_tests unit/main.cpp)
target_link_libraries(ocgbridge_unit_tests PRIVATE ocgbridge)
target_compile_definitions(ocgbridge_unit_tests PRIVATE OCGBRIDGE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_test(NAME unit COMMAND ocgbridge_unit_tests)

# Golden protocol transcripts (node + the pinned card data: tests/golden/fetch-data.sh).
find_program(NODE_EXECUTABLE node)
set(OCGBRIDGE_TEST_SCRIPTS "${CMAKE_CURRENT_SOURCE_DIR}/data/CardScripts" CACHE PATH "CardScripts checkout for the golden tests")
set(OCGBRIDGE_TEST_CDB "${CMAKE_CURRENT_SOURCE_DIR}/data/BabelCDB/cards.cdb" CACHE FILEPATH "cards.cdb for the golden tests")
if(NODE_EXECUTABLE AND OCGBRIDGE_BUILD_SERVER)
    set(OCGBRIDGE_GOLDEN_EXTRA)
    if(TARGET ocgcore-bridge-extended)
        set(OCGBRIDGE_GOLDEN_EXTRA --extended-bin $<TARGET_FILE:ocgcore-bridge-extended>)
    endif()
    add_test(NAME golden COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/golden/run.mjs
             --bin $<TARGET_FILE:ocgcore-bridge> --scripts ${OCGBRIDGE_TEST_SCRIPTS} --cdb ${OCGBRIDGE_TEST_CDB}
             ${OCGBRIDGE_GOLDEN_EXTRA})
    set_tests_properties(golden PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
endif()

# The tree names no downstream user (tests/agnostic_test.sh).
find_program(SH_EXECUTABLE sh)
if(SH_EXECUTABLE)
    add_test(NAME agnostic COMMAND ${SH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/agnostic_test.sh ${PROJECT_SOURCE_DIR})
endif()
