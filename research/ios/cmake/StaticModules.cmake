# SPDX-License-Identifier: GPL-2.0-or-later
include_guard(GLOBAL)
function(switch_aot_add_images target exefs)
    if(NOT EXISTS "${exefs}/recomp_registration.c")
        message(FATAL_ERROR "Expected an ExeFS export with recomp_registration.c")
    endif()
    # Reject CMake list separators; spaces and square brackets remain supported.
    if("${exefs}" MATCHES ";")
        message(FATAL_ERROR "Export paths containing semicolons are unsupported")
    endif()
    set(RECOMP_STATIC_ONLY ON)
    # Glob relative to a temporary path that contains no owner/title characters.
    # Avoid file(GLOB) interpreting brackets in an owner's export path.
    execute_process(COMMAND "${Python3_EXECUTABLE}" -c
        "import pathlib,sys; p=pathlib.Path(sys.argv[1]); print(';'.join(sorted(x.name for x in p.iterdir() if x.is_dir() and (x/'recomp_export.c').is_file() and (x/'CMakeLists.txt').is_file())))"
        "${exefs}" OUTPUT_VARIABLE modules OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    if(NOT modules)
        message(FATAL_ERROR "No generated modules found")
    endif()
    add_library(${target} STATIC "${exefs}/recomp_registration.c")
    set(first_hash "")
    foreach(module IN LISTS modules)
        if(NOT module MATCHES "^(rtld|main|sdk|subsdk[0-9]+)$")
            message(FATAL_ERROR "Unexpected module directory name")
        endif()
        foreach(file recomp_runtime.h recomp_runtime.c)
            if(NOT EXISTS "${exefs}/${module}/${file}")
                message(FATAL_ERROR "Incomplete generated module: ${module}")
            endif()
        endforeach()
        file(SHA256 "${exefs}/${module}/recomp_runtime.h" header_hash)
        file(SHA256 "${exefs}/${module}/recomp_runtime.c" runtime_hash)
        set(pair_hash "${header_hash}:${runtime_hash}")
        if(first_hash AND NOT pair_hash STREQUAL first_hash)
            message(FATAL_ERROR "Mixed runtime/header revisions: re-export ALL modules together")
        endif()
        set(first_hash "${pair_hash}")
        add_subdirectory("${exefs}/${module}" "${CMAKE_CURRENT_BINARY_DIR}/images/${module}")
        if(NOT TARGET recomp_static_${module})
            message(FATAL_ERROR "Export has no recomp_static_${module} target")
        endif()
        # 4f1b898's emitter renames its older symbols but not these newer index
        # functions. Several module archives in one executable otherwise collide.
        # Add at target scope; do NOT alter any owner-generated source files.
        if(NOT SWITCH_AOT_TEST_OMIT_SYMBOL_FIX)
            target_compile_definitions(recomp_static_${module} PRIVATE
                recomp_build_index=recomp_build_index_${module}
                _recomp_index_view=_recomp_index_view_${module}
                recomp_image_index=recomp_image_index_${module})
        endif()
        target_link_libraries(${target} PUBLIC recomp_static_${module})
        # Compile the ACTUAL generated header, not a locally invented prefix.
        set(probe "${CMAKE_CURRENT_BINARY_DIR}/abi_${module}.c")
        file(WRITE "${probe}" "#include <stddef.h>\n#include \"recomp_runtime.h\"\n_Static_assert(sizeof(void*)==8, \"64-bit host required\");\n_Static_assert(offsetof(GuestContext, pc)==256, \"pc ABI\");\n_Static_assert(offsetof(GuestContext, pending_svc)==304, \"svc ABI\");\n_Static_assert(offsetof(GuestContext, vreg)==312, \"SIMD ABI\");\n_Static_assert(offsetof(GuestContext, tpidr_el0)==824, \"TLS ABI\");\n_Static_assert(offsetof(GuestContext, host_mem)==832, \"memory ABI\");\n_Static_assert(offsetof(GuestContext, tpidrro_el0)==840, \"read-only TLS ABI\");\n_Static_assert(offsetof(GuestContext, fpcr)==848, \"FPCR ABI\");\n_Static_assert(offsetof(GuestContext, fpsr)==856, \"FPSR ABI\");\n_Static_assert(offsetof(GuestContext, chain_budget)==864, \"chain budget ABI\");\n_Static_assert(sizeof(RecompHostMem)==112, \"host memory ABI\");\n_Static_assert(offsetof(RecompHostMem, excl_store_pair)==64, \"exclusive ABI\");\n_Static_assert(offsetof(RecompHostMem, page_entries)==72, \"page table ABI\");\n")
        add_library(switch_aot_abi_${module} OBJECT "${probe}")
        target_include_directories(switch_aot_abi_${module} PRIVATE "${exefs}/${module}")
        target_sources(${target} PRIVATE $<TARGET_OBJECTS:switch_aot_abi_${module}>)
    endforeach()
    if(UNIX AND NOT APPLE)
        target_link_libraries(${target} PUBLIC m)
    endif()
endfunction()

# Future full-core integration seam, not a claim that the core is iOS-portable.
function(switch_aot_attach_suyu target)
    if(NOT TARGET core OR NOT SUYU_NO_JIT OR TARGET dynarmic)
        message(FATAL_ERROR "Attach requires a real SUYU_NO_JIT core without dynarmic")
    endif()
    target_sources(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/core_bridge.cpp")
    target_link_libraries(${target} PRIVATE core switch_aot_registry switch_aot_images)
endfunction()
