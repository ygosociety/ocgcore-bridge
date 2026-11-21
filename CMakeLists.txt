cmake_minimum_required(VERSION 3.16)
project(ocgcore-bridge VERSION 0.1.1 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(OCGBRIDGE_BUILD_SERVER "Build the stdio server (ocgcore-bridge)" ON)
option(OCGBRIDGE_BUILD_TESTS "Build the unit tests" ON)
option(OCGBRIDGE_WITH_SQLITE "Read BabelCDB .cdb card databases (needs SQLite3)" ON)

include(FetchContent)

# ── ygopro-core ───────────────────────────────────────────────────────────────
# Pinned on purpose: upstream rewrites its history. Move it deliberately, then
# re-run the unit and golden tests. A local checkout can be used with
# -DFETCHCONTENT_SOURCE_DIR_OCGCORE=/path/to/ygopro-core (lua/src included).
FetchContent_Declare(
    ocgcore
    GIT_REPOSITORY https://github.com/edo9300/ygopro-core.git
    GIT_TAG        6864373c24d7313c8e451df17877c12c4f713c46 # master, 2026-10-28
)
# ygopro-core has no CMakeLists.txt: this only fetches it.
FetchContent_MakeAvailable(ocgcore)

set(OCGCORE_SRC ${ocgcore_SOURCE_DIR})
set(LUA_SRC     ${OCGCORE_SRC}/lua/src)

# Lua (bundled with ygopro-core), compiled as C++: ygopro-core includes the Lua
# headers without extern "C". lua.c / luac.c (the interpreter and compiler
# mains) and the test sources are left out.
file(GLOB LUA_C_SOURCES "${LUA_SRC}/*.c")
list(FILTER LUA_C_SOURCES EXCLUDE REGEX "(lua|luac|ltests|onelua)\\.c$")
set_source_files_properties(${LUA_C_SOURCES} PROPERTIES LANGUAGE CXX)
add_library(ocgbridge_lua STATIC ${LUA_C_SOURCES})
target_include_directories(ocgbridge_lua PUBLIC ${LUA_SRC})
target_compile_definitions(ocgbridge_lua PRIVATE LUA_COMPAT_5_3)
target_compile_options(ocgbridge_lua PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wno-deprecated-declarations>
)
set_target_properties(ocgbridge_lua PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(ocgbridge_ygopro_core STATIC
    ${OCGCORE_SRC}/card.cpp
    ${OCGCORE_SRC}/duel.cpp
    ${OCGCORE_SRC}/effect.cpp
    ${OCGCORE_SRC}/field.cpp
    ${OCGCORE_SRC}/interpreter.cpp
    ${OCGCORE_SRC}/libcard.cpp
    ${OCGCORE_SRC}/libdebug.cpp
    ${OCGCORE_SRC}/libduel.cpp
    ${OCGCORE_SRC}/libeffect.cpp
    ${OCGCORE_SRC}/libgroup.cpp
    ${OCGCORE_SRC}/ocgapi.cpp
    ${OCGCORE_SRC}/operations.cpp
    ${OCGCORE_SRC}/playerop.cpp
    ${OCGCORE_SRC}/processor.cpp
    ${OCGCORE_SRC}/processor_visit.cpp
    ${OCGCORE_SRC}/scriptlib.cpp
)
target_include_directories(ocgbridge_ygopro_core PUBLIC ${OCGCORE_SRC})
target_link_libraries(ocgbridge_ygopro_core PUBLIC ocgbridge_lua)
target_compile_definitions(ocgbridge_ygopro_core PRIVATE LUA_COMPAT_5_3)

# ── nlohmann/json ────────────────────────────────────────────────────────────
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# ── libocgbridge (core/) ─────────────────────────────────────────────────────
add_library(ocgbridge STATIC
    core/src/announce.cpp
    core/src/answers.cpp
    core/src/cards.cpp
    core/src/cards_cdb.cpp
    core/src/decode.cpp
    core/src/events.cpp
    core/src/messages.cpp
    core/src/queries.cpp
    core/src/responder.cpp
    core/src/session.cpp
)
target_include_directories(ocgbridge PUBLIC core/include PRIVATE core/src)
target_link_libraries(ocgbridge PUBLIC ocgbridge_ygopro_core nlohmann_json::nlohmann_json)
target_compile_definitions(ocgbridge PUBLIC OCGBRIDGE_VERSION="${PROJECT_VERSION}")

if(OCGBRIDGE_WITH_SQLITE)
    find_package(SQLite3)
    if(SQLite3_FOUND)
        target_link_libraries(ocgbridge PRIVATE SQLite::SQLite3)
        target_compile_definitions(ocgbridge PRIVATE OCGBRIDGE_WITH_SQLITE)
    else()
        message(WARNING "SQLite3 not found: .cdb card databases will not be readable")
    endif()
endif()

# ── ocgcore-bridge (server/) ─────────────────────────────────────────────────
# The server is a library too (server/server.h): a program of your own can
# link ocgbridge_server, register extensions and run the same protocol.
if(OCGBRIDGE_BUILD_SERVER)
    add_library(ocgbridge_server STATIC server/server.cpp server/protocol.cpp)
    target_include_directories(ocgbridge_server PUBLIC server)
    target_link_libraries(ocgbridge_server PUBLIC ocgbridge)
    add_executable(ocgcore-bridge server/main.cpp)
    target_link_libraries(ocgcore-bridge PRIVATE ocgbridge_server)
    install(TARGETS ocgcore-bridge DESTINATION bin)
endif()

# ── examples/ (C++) ──────────────────────────────────────────────────────────
option(OCGBRIDGE_BUILD_EXAMPLES "Build the C++ extension example" ON)
if(OCGBRIDGE_BUILD_SERVER AND OCGBRIDGE_BUILD_EXAMPLES)
    add_executable(ocgcore-bridge-extended examples/extended-server/main.cpp)
    target_link_libraries(ocgcore-bridge-extended PRIVATE ocgbridge_server)
endif()

# ── tests ────────────────────────────────────────────────────────────────────
if(OCGBRIDGE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
