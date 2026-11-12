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
set(CON_IRX_DIR "" CACHE PATH "The disc's BG/0MOD folder (default: next to CON_ELF)")
set(CON_GEN_FILES 32 CACHE STRING "How many .cpp files the generated code is split into")
set(CON_GEN_OPT "-O1" CACHE STRING "Optimisation of the generated code")
option(CON_WINDOW "Build con, the game in a window (BozoEngine's bozo_app: SDL3, OpenGL 3.3)" OFF)
if(NOT BOZO_ENGINE_DIR OR NOT EXISTS "${BOZO_ENGINE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "CoN: set BOZO_ENGINE_DIR to BozoEngine's source tree")
endif()
if(NOT CON_ELF OR NOT EXISTS "${CON_ELF}")
    message(FATAL_ERROR "CoN: set CON_ELF to the game's executable (SLUS_205.65)")
endif()
if(NOT CON_IRX_DIR)
    get_filename_component(con_disc "${CON_ELF}" DIRECTORY)
    set(CON_IRX_DIR "${con_disc}/BG/0MOD")
endif()
foreach(m LIBSD SDRDRV HELLO)
    if(NOT EXISTS "${CON_IRX_DIR}/${m}.IRX")
        message(FATAL_ERROR "CoN: ${CON_IRX_DIR}/${m}.IRX not found (set CON_IRX_DIR to the disc's BG/0MOD folder)")
    endif()
endforeach()
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
list(REMOVE_ITEM CON_RUNTIME_SOURCES "${CMAKE_SOURCE_DIR}/runtime/con_textures.cpp" "${CMAKE_SOURCE_DIR}/runtime/con_water.cpp")

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

set(irx_dir "${gen_dir}/iop")
set(native_irx "${BOZO_ENGINE_DIR}/tools/native_irx")
set(irx_sources)
set(irx_images)
foreach(spec "LIBSD:libsd:0x8000:libsd" "SDRDRV:sdrdrv:0xe000:" "HELLO:hello:0x10000:")
    string(REPLACE ":" ";" spec "${spec}")
    list(GET spec 0 module)
    list(GET spec 1 stem)
    list(GET spec 2 base)
    list(GET spec 3 flag)
    add_custom_command(
        OUTPUT ${irx_dir}/${stem}.json ${irx_dir}/${stem}.bin ${irx_dir}/${stem}_iop.cpp
        COMMAND ${CMAKE_COMMAND} -E make_directory ${irx_dir}
        COMMAND Python3::Interpreter -I "${native_irx}/relocate.py" "${CON_IRX_DIR}/${module}.IRX" ${base} ${irx_dir}/${stem}
        COMMAND Python3::Interpreter -I "${native_irx}/recompile.py" ${irx_dir}/${stem}.json --out ${irx_dir}
        DEPENDS "${CON_IRX_DIR}/${module}.IRX" "${native_irx}/relocate.py" "${native_irx}/recompile.py"
        COMMENT "Compiling ${module}.IRX")
    list(APPEND irx_sources ${irx_dir}/${stem}_iop.cpp)
    if(flag)
        list(APPEND irx_images ${irx_dir}/${stem}.json:${flag})
    else()
        list(APPEND irx_images ${irx_dir}/${stem}.json)
    endif()
endforeach()
add_custom_command(
    OUTPUT ${irx_dir}/con_irx_images.cpp
    COMMAND Python3::Interpreter -I "${CMAKE_SOURCE_DIR}/tools/con_irx_images.py" "${native_irx}" ${irx_dir}/con_irx_images.cpp ${irx_images}
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/con_irx_images.py" ${irx_sources}
    COMMENT "Writing CoN's IOP module images")
add_library(con_iop OBJECT ${irx_sources} ${irx_dir}/con_irx_images.cpp)
target_include_directories(con_iop PRIVATE "${CMAKE_SOURCE_DIR}/runtime")
target_link_libraries(con_iop PUBLIC bozo_runtime_headers bozo_libs)
target_compile_options(con_iop PRIVATE -w)

add_library(con_runtime OBJECT ${CON_RUNTIME_SOURCES})
target_include_directories(con_runtime PUBLIC "${CMAKE_SOURCE_DIR}/runtime" ${gen_dir})
target_link_libraries(con_runtime PUBLIC con_textures con_water bozo_port bozo_render bozo_libs bozo_runtime_core)
add_dependencies(con_runtime con_generated)

add_executable(con-headless bozo/con_headless.cpp)
target_link_libraries(con-headless PRIVATE con_runtime con_textures con_water con_generated con_iop bozo_host)
target_compile_definitions(con-headless PRIVATE BOZO_RUN_DEFAULT_ELF="${CON_ELF}")

add_library(con_textures OBJECT runtime/con_textures.cpp)
target_include_directories(con_textures PUBLIC "${CMAKE_SOURCE_DIR}/runtime")
target_link_libraries(con_textures PUBLIC bozo_libs bozo_render)
add_executable(con-tex-check EXCLUDE_FROM_ALL bozo/con_tex_check.cpp)
target_link_libraries(con-tex-check PRIVATE con_textures bozo_libs bozo_render)
add_library(con_water OBJECT runtime/con_water.cpp)
target_include_directories(con_water PUBLIC "${CMAKE_SOURCE_DIR}/runtime")
add_executable(con-water-tests EXCLUDE_FROM_ALL tests/con_water_tests.cpp)
target_link_libraries(con-water-tests PRIVATE con_water)
add_executable(con-textures-tests EXCLUDE_FROM_ALL tests/con_textures_tests.cpp)
target_link_libraries(con-textures-tests PRIVATE con_textures bozo_libs bozo_render)

if(CON_WINDOW)
    add_executable(con bozo/con_window.cpp)
    target_link_libraries(con PRIVATE con_runtime con_textures con_water con_generated con_iop bozo_host bozo_app)
    target_compile_definitions(con PRIVATE BOZO_RUN_DEFAULT_ELF="${CON_ELF}")
endif()
