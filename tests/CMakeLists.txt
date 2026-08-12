foreach(test_name lexer_tests parser_tests semantic_tests cfg_tests dominator_tests
        ssa_tests mir_tests liveness_tests regalloc_tests abi_tests codegen_tests
        optimization_tests machine_combine_tests machine_peephole_tests)
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE toyc_lib)
    target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

if(UNIX)
    find_program(TOYC_RISCV_GCC riscv64-linux-gnu-gcc)
    find_program(TOYC_QEMU_RISCV32 qemu-riscv32)
    if(TOYC_RISCV_GCC AND TOYC_QEMU_RISCV32)
        add_test(
            NAME e2e_tests
            COMMAND sh ${PROJECT_SOURCE_DIR}/scripts/run_e2e.sh
                    $<TARGET_FILE:compiler>
                    ${TOYC_RISCV_GCC}
                    ${TOYC_QEMU_RISCV32})
    else()
        message(STATUS
            "RV32 end-to-end tests disabled: install gcc-riscv64-linux-gnu and qemu-user")
    endif()
endif()
