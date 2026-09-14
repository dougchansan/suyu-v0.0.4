#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Create ORIGINAL synthetic C fixtures; these are not output from the emitter.

The module target/symbol shape mirrors arm64_to_c.h at 4f1b898. The separate
export_synthetic.cpp integration test uses the real emitter in a full checkout.
No game, key, firmware, network access, or external Python package is used.
"""
import sys
from pathlib import Path

HEADER = r"""#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct RecompHostMem {
 void* user;
 uint64_t (*load)(void*,uint64_t,uint32_t);
 void (*store)(void*,uint64_t,uint32_t,uint64_t);
 uint64_t (*excl_load)(void*,uint64_t,uint32_t);
 uint32_t (*excl_store)(void*,uint64_t,uint32_t,uint64_t);
 void (*clear_excl)(void*);
 uint64_t (*read_cntpct)(void*);
 void (*excl_load_pair)(void*,uint64_t,uint32_t,uint64_t*,uint64_t*);
 uint32_t (*excl_store_pair)(void*,uint64_t,uint32_t,uint64_t,uint64_t);
 const void* page_entries;
 uint64_t page_entry_stride, page_bits, pointer_mask, address_space_max;
} RecompHostMem;
typedef struct GuestContext {
 uint64_t x[32], pc;
 uint8_t n,z,c,v;
 uint8_t* mem;
 uint64_t mem_size, mem_base_vaddr;
 int halted;
 uint64_t pending_svc, vreg[32][2], tpidr_el0;
 const RecompHostMem* host_mem;
 uint64_t tpidrro_el0, fpcr, fpsr;
 int chain_budget;
} GuestContext;
typedef void (*BlockFn)(void*);
extern uint64_t g_module_base;
void synthetic_add_svc(GuestContext* c);
BlockFn recomp_lookup(uint64_t pc);
void recomp_build_index(void);
int _recomp_index_view(uint64_t*,uint64_t*,BlockFn**);
"""
CMAKE = r"""cmake_minimum_required(VERSION 3.13)
if(NOT RECOMP_STATIC_ONLY)
 message(FATAL_ERROR "Synthetic fixture is static-only")
endif()
if(NOT TARGET recomp_runtime_shared)
 add_library(recomp_runtime_shared STATIC recomp_runtime.c)
 target_compile_definitions(recomp_runtime_shared PRIVATE SUYU_HOSTED_RECOMP=1 RECOMP_STATIC_HOST=1)
 target_include_directories(recomp_runtime_shared PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
endif()
add_library(recomp_static_@MODULE@ STATIC recomp_export.c src/recompiled_@MODULE@.c)
target_include_directories(recomp_static_@MODULE@ PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(recomp_static_@MODULE@ PUBLIC recomp_runtime_shared)
target_compile_definitions(recomp_static_@MODULE@ PRIVATE SUYU_HOSTED_RECOMP=1 RECOMP_STATIC_MODULE=1
 g_module_base=g_module_base_@MODULE@ recomp_lookup=recomp_lookup_@MODULE@
 recomp_image_lookup=recomp_image_lookup_@MODULE@
 recomp_image_set_base=recomp_image_set_base_@MODULE@
 recomp_image_entry=recomp_image_entry_@MODULE@)
"""
EXPORT = r"""#include "recomp_runtime.h"
uint64_t g_module_base;
BlockFn recomp_image_lookup(uint64_t pc) { return recomp_lookup(pc-g_module_base); }
void recomp_image_set_base(uint64_t base) { g_module_base=base; recomp_build_index(); }
uint64_t recomp_image_entry(void) { return 0x100; }
int recomp_image_index(uint64_t* lo,uint64_t* hi,BlockFn** idx) {
 if(!_recomp_index_view(lo,hi,idx)) return 0;
 *lo+=g_module_base; *hi+=g_module_base; return 1;
}
"""
BODY = r"""#include "recomp_runtime.h"
static void block(void* p) { synthetic_add_svc((GuestContext*)p); }
static BlockFn index_entry;
void recomp_build_index(void) { index_entry=block; }
int _recomp_index_view(uint64_t* lo,uint64_t* hi,BlockFn** idx) {
 *lo=*hi=0x100; *idx=&index_entry; return 1;
}
BlockFn recomp_lookup(uint64_t pc) { return pc==0x100 ? index_entry : 0; }
"""
RUNTIME = r"""#include "recomp_runtime.h"
void synthetic_add_svc(GuestContext* c) { c->x[0]+=1; c->pending_svc=1; c->pc+=8; }
"""

def registration():
    text = '#include <stdint.h>\ntypedef void (*BlockFn)(void*);\n'
    text += 'struct SuyuRecompStaticModule { const char* name; BlockFn (*lookup)(uint64_t); void (*set_base)(uint64_t); };\n'
    for m in ('rtld', 'main', 'sdk'):
        text += f'extern BlockFn recomp_image_lookup_{m}(uint64_t);\nextern void recomp_image_set_base_{m}(uint64_t);\n'
    text += 'static const struct SuyuRecompStaticModule modules[] = {\n'
    for m in ('rtld', 'main', 'sdk'):
        text += f'{{"{m}", recomp_image_lookup_{m}, recomp_image_set_base_{m}}},\n'
    return text + '};\nconst struct SuyuRecompStaticModule* suyu_recomp_static_modules(unsigned* count) { *count=3; return modules; }\n'

def create(root: Path) -> None:
    root.mkdir(parents=True, exist_ok=True)
    for m in ('rtld', 'main', 'sdk'):
        d = root/m
        (d/'src').mkdir(parents=True, exist_ok=True)
        for filename, content in {'CMakeLists.txt': CMAKE.replace('@MODULE@', m),
                'recomp_runtime.h': HEADER, 'recomp_runtime.c': RUNTIME,
                'recomp_export.c': EXPORT, f'src/recompiled_{m}.c': BODY}.items():
            (d/filename).write_text(content, encoding='utf-8')
    (root/'recomp_registration.c').write_text(registration(), encoding='utf-8')
    (root/'SYNTHETIC_FIXTURE.txt').write_text('Original ABI-mirror fixture; NOT real emitter output.\n')

if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: make_fixture.py OUTPUT_DIRECTORY')
    create(Path(sys.argv[1]))
