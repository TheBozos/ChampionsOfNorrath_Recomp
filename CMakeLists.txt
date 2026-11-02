cmake_minimum_required(VERSION 3.21)
project(ChampionsOfNorrath_Recomp LANGUAGES C CXX)

set(CON_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated" CACHE PATH
    "Output of ps2_recomp for SLUS_205.65 (scripts/recomp.sh)")

set(PS2X_BUILD_RECOMP OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_ANALYZER OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_STUDIO OFF CACHE BOOL "" FORCE)
set(PS2X_APP_TITLE "Champions of Norrath" CACHE STRING "" FORCE)
set(PS2X_APP_FOLDER "ChampionsOfNorrath_Recomp" CACHE STRING "" FORCE)
set(PS2X_APP_XDG_FOLDER "championsofnorrath_recomp" CACHE STRING "" FORCE)
set(PS2X_APP_BOOT_ELF "SLUS_205.65" CACHE STRING "" FORCE)
set(PS2X_APP_DATA_FOLDER "game" CACHE STRING "" FORCE)

add_subdirectory(PS2Recomp)

file(GLOB CON_GENERATED_SOURCES CONFIGURE_DEPENDS "${CON_GENERATED_DIR}/*.cpp")
if(NOT CON_GENERATED_SOURCES)
    message(FATAL_ERROR "No generated sources in ${CON_GENERATED_DIR}. Run scripts/recomp.sh first.")
endif()

# The runtime's placeholder function table clashes with the generated register_functions.cpp.
get_target_property(_con_runner_sources ps2EntryRunner SOURCES)
list(FILTER _con_runner_sources EXCLUDE REGEX "src/runner/register_functions\\.cpp$")
set_property(TARGET ps2EntryRunner PROPERTY SOURCES ${_con_runner_sources})

file(GLOB CON_RUNTIME_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/runtime/*.cpp")

file(GLOB CON_IOP_SOURCES CONFIGURE_DEPENDS "${CON_GENERATED_DIR}/iop/*.cpp")
if(CON_IOP_SOURCES)
    add_library(con_iop_native OBJECT ${CON_IOP_SOURCES})
    target_link_libraries(con_iop_native PRIVATE ps2x::iop)
    set_target_properties(con_iop_native PROPERTIES UNITY_BUILD OFF)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(con_iop_native PRIVATE -O2 -g0)
    endif()
    target_sources(ps2EntryRunner PRIVATE $<TARGET_OBJECTS:con_iop_native>)
endif()

target_sources(ps2EntryRunner PRIVATE ${CON_GENERATED_SOURCES} ${CON_RUNTIME_SOURCES})
target_include_directories(ps2EntryRunner PRIVATE "${CON_GENERATED_DIR}")

list(LENGTH CON_GENERATED_SOURCES _con_count)
list(LENGTH CON_IOP_SOURCES _con_iop_count)
message(STATUS "ChampionsOfNorrath_Recomp: ${_con_count} generated sources, ${_con_iop_count} recompiled IOP module(s)")
