cmake_minimum_required(VERSION 3.28)
project(toyc_compiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

add_library(toyc_lib
    src/support/diagnostic.cpp
    src/frontend/lexer.cpp
    src/frontend/ast_printer.cpp
    src/frontend/parser.cpp
    src/frontend/semantic.cpp
    src/ir/cfg_builder.cpp
    src/ir/cfg_utils.cpp
    src/ir/dominator.cpp
    src/ir/ssa_builder.cpp
    src/ir/ir_printer.cpp
    src/ir/verifier.cpp
    src/opt/ir_utils.cpp
    src/opt/function_effects.cpp
    src/opt/instcombine.cpp
    src/opt/inline.cpp
    src/opt/local_dag.cpp
    src/opt/sccp.cpp
    src/opt/simplify_cfg.cpp
    src/opt/tail_recursion.cpp
    src/opt/dce.cpp
    src/opt/gvn.cpp
    src/opt/loop_analysis.cpp
    src/opt/loop_transform.cpp
    src/opt/licm.cpp
    src/opt/pipeline.cpp
    src/backend/mir.cpp
    src/backend/mir_printer.cpp
    src/backend/mir_verifier.cpp
    src/backend/phi_lowering.cpp
    src/backend/isel.cpp
    src/backend/machine_combine.cpp
    src/backend/machine_peephole.cpp
    src/backend/liveness.cpp
    src/backend/regalloc.cpp
    src/backend/frame.cpp
    src/backend/asm_printer.cpp)
target_include_directories(toyc_lib PUBLIC include)
target_compile_options(toyc_lib PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion>
    $<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive->)

add_executable(compiler src/main.cpp)
target_link_libraries(compiler PRIVATE toyc_lib)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
