# Champions of Norrath on BozoEngine: the game's EE code recompiled by bozo-recomp, CoN's runtime/ on BozoEngine's port
# API, every Sony library native.
#
#   cmake -S . -B build/bozo -G Ninja -DCON_ELF=<SLUS_205.65> [-DBOZO_ENGINE_DIR=<BozoEngine>] [-DBOZO_FFMPEG_ROOT=<dir>]
#
# Targets: con-headless (no window, BozoEngine's free host).

set(BOZO_ENGINE_DIR "" CACHE PATH "BozoEngine's source tree (default: the BozoEngine submodule)")
if(NOT BOZO_ENGINE_DIR AND EXISTS "${CMAKE_SOURCE_DIR}/BozoEngine/CMakeLists.txt")
    set(BOZO_ENGINE_DIR "${CMAKE_SOURCE_DIR}/BozoEngine" CACHE PATH "BozoEngine's source tree (default: the BozoEngine submodule)" FORCE)
endif()
set(CON_ELF "" CACHE FILEPATH "CoN's executable (SLUS_205.65), extracted from the disc")
set(CON_GEN_FILES 32 CACHE STRING "How many .cpp files the generated code is split into")
set(CON_GEN_OPT "-O1" CACHE STRING "Optimisation of the generated code")
option(CON_WINDOW "Build con, the game in a window (BozoEngine's bozo_app: SDL3, OpenGL 3.3)" OFF)
if(NOT BOZO_ENGINE_DIR OR NOT EXISTS "${BOZO_ENGINE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "CoN: set BOZO_ENGINE_DIR to BozoEngine's source tree")
endif()
if(NOT CON_ELF OR NOT EXISTS "${CON_ELF}")
    message(FATAL_ERROR "CoN: set CON_ELF to the game's executable (SLUS_205.65)")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(BOZO_TMB_ELF "" CACHE FILEPATH "" FORCE)
set(BOZO_APP ${CON_WINDOW} CACHE BOOL "" FORCE)
add_subdirectory("${BOZO_ENGINE_DIR}" bozo)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(gen_dir "${CMAKE_BINARY_DIR}/con_gen")
set(overrides_list "${gen_dir}/con_overrides.txt")
set(externals "${gen_dir}/con_externals.txt")
file(GLOB CON_RUNTIME_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/runtime/*.cpp")

add_custom_command(
    OUTPUT ${overrides_list}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${gen_dir}
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/con_overrides_list.py" "${CMAKE_SOURCE_DIR}/runtime" ${overrides_list}
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/con_overrides_list.py" ${CON_RUNTIME_SOURCES}
    COMMENT "Listing CoN's overrides")
add_custom_command(
    OUTPUT ${externals}
    COMMAND bozo-externals ${CON_ELF} ${externals} --overrides ${overrides_list} --stubs "${CMAKE_SOURCE_DIR}/recomp/con_stubs.txt"
    DEPENDS bozo-externals ${overrides_list} "${CMAKE_SOURCE_DIR}/recomp/con_stubs.txt" ${CON_ELF}
    COMMENT "Writing the externals list")

set(gen_files ${gen_dir}/con_functions.h ${gen_dir}/con_table.cpp)
math(EXPR last "${CON_GEN_FILES} - 1")
foreach(i RANGE ${last})
    string(LENGTH "${i}" len)
    if(len EQUAL 1)
        set(n "00${i}")
    elseif(len EQUAL 2)
        set(n "0${i}")
    else()
        set(n "${i}")
    endif()
    list(APPEND gen_files ${gen_dir}/con_${n}.cpp)
endforeach()
add_custom_command(
    OUTPUT ${gen_files}
    COMMAND bozo-recomp ${CON_ELF} ${gen_dir} --prefix con --files ${CON_GEN_FILES} --no-comments --externals ${externals}
    DEPENDS bozo-recomp ${externals} ${CON_ELF}
    COMMENT "Recompiling CoN")
add_custom_target(con_recompile DEPENDS ${gen_files})
add_library(con_generated OBJECT ${gen_files})
target_link_libraries(con_generated PUBLIC bozo_runtime_headers)
target_include_directories(con_generated PUBLIC ${gen_dir})
target_compile_options(con_generated PRIVATE ${CON_GEN_OPT} -w)

add_library(con_runtime OBJECT ${CON_RUNTIME_SOURCES})
target_include_directories(con_runtime PUBLIC "${CMAKE_SOURCE_DIR}/runtime" ${gen_dir})
target_link_libraries(con_runtime PUBLIC bozo_port bozo_render bozo_libs bozo_runtime_core)
add_dependencies(con_runtime con_generated)

add_executable(con-headless bozo/con_headless.cpp)
target_link_libraries(con-headless PRIVATE con_runtime con_generated bozo_host)
target_compile_definitions(con-headless PRIVATE BOZO_RUN_DEFAULT_ELF="${CON_ELF}")
