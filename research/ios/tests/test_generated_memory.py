#!/usr/bin/env python3
"""Compile actual emitted runtime against synthetic discontiguous guest pages."""
import os
import sys
from pathlib import Path
import subprocess
import tempfile
import unittest
from patch_generated_memory import patch, HELPER

LINK_GC = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
ROOT = Path(__file__).resolve().parents[3]
HARNESS = r'''
#include "recomp_runtime.c"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
static unsigned char backing[3][8192];
static unsigned char* pages[4];
static uintptr_t entries[4];
static unsigned loads, stores, faults;
static void map_page(unsigned page, unsigned char* ptr) {
  pages[page]=ptr;
  entries[page]=ptr ? (uintptr_t)ptr-page*4096 : 0;
}
static uint64_t read_cb(void* u,uint64_t a,uint32_t sz) {
  uint64_t v=0; (void)u; ++loads; a &= 0xffffffffffffULL;
  for(unsigned i=0;i<sz;++i) {
    if(a+i>=16384 || !pages[(a+i)>>12]) { ++faults; continue; }
    v |= (uint64_t)pages[(a+i)>>12][(a+i)&4095] << (i*8);
  } return v;
}
static void write_cb(void* u,uint64_t a,uint32_t sz,uint64_t v) {
  (void)u; ++stores; a &= 0xffffffffffffULL;
  for(unsigned i=0;i<sz;++i) {
    if(a+i>=16384 || !pages[(a+i)>>12]) { ++faults; continue; }
    pages[(a+i)>>12][(a+i)&4095]=(unsigned char)(v>>(i*8));
  }
}
int main(void) {
  RecompHostMem hm={0}; GuestContext c={0};
  hm.load=read_cb; hm.store=write_cb; hm.page_entries=entries;
  hm.page_entry_stride=sizeof(uintptr_t); hm.page_bits=12;
  hm.pointer_mask=UINT64_MAX; hm.address_space_max=16384; c.host_mem=&hm;
  uint64_t (*ld[])(GuestContext*,uint64_t)={recomp_load16,recomp_load32,recomp_load64};
  void (*st[])(GuestContext*,uint64_t,uint64_t)={recomp_store16,recomp_store32,recomp_store64};
  for(unsigned k=0;k<3;++k) {
    unsigned size=2U<<k; uint64_t mask=UINT64_MAX>>(64-size*8);
    for(unsigned cross=1;cross<size;++cross) {
      memset(backing,0xA5,sizeof(backing));
      map_page(0,backing[0]); map_page(1,backing[2]); map_page(2,backing[0]); map_page(3,0);
      uint64_t a=4096-cross, v=0x8877665544332211ULL & mask;
      unsigned before=stores; st[k](&c,a,v); assert(stores==before+1);
      before=loads; assert(ld[k](&c,a)==v); assert(loads==before+1);
      assert(backing[0][4096]==0xA5); /* Must not touch adjacent host bytes. */
      assert(ld[k](&c,8192+4096-cross)==(v & (UINT64_MAX>>(64-cross*8))));
      map_page(1,backing[1]); assert(ld[k](&c,a)==read_cb(0,a,size));
      map_page(1,0); before=faults; (void)ld[k](&c,a); assert(faults>before);
      before=faults; st[k](&c,a,v); assert(faults>before);
    }
    map_page(0,backing[0]); unsigned before=loads;
    st[k](&c,128,0x1234); assert(ld[k](&c,128)==0x1234); assert(loads==before);
    assert(ld[k](&c,0xabcd000000000080ULL)==0x1234);
    map_page(3,backing[1]); hm.address_space_max=16381;
    before=loads; (void)ld[k](&c,16380); assert(loads==before+1);
    hm.address_space_max=16384;
  }
  puts("PASS: emitted scalar memory spans, discontiguous pages, alias/remap/unmap, callbacks");
}
'''

def run(args):
    result = subprocess.run(list(map(str,args)), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode: raise AssertionError(result.stdout)
    return result.stdout

class GeneratedMemory(unittest.TestCase):
    def test_production_host_callbacks(self):
        source=(ROOT/'src/core/arm/recomp/arm_recomp.cpp').read_text()
        start=source.index('    static u64 HostLoad(void* user')
        end=source.index('    /// Base address',start)
        callbacks=source[start:end]
        # Compile the exact production callback bodies, not a test reimplementation.
        # The fake Memory offers only byte operations; old scalar callbacks cannot compile.
        stub = r'''
using u64=uint64_t; using u32=uint32_t; using u8=uint8_t;
struct TestMemory {
    u8 Read8(u64 a) { return (u8)read_cb(nullptr,a,1); }
    void Write8(u64 a,u8 v) { write_cb(nullptr,a,1,v); }
};
struct TestSystem { TestMemory memory; TestMemory& ApplicationMemory() { return memory; } };
struct Impl { TestSystem system;
''' + callbacks + '\n};\n'
        h=HARNESS.replace('#include "recomp_runtime.c"', 'extern "C" {\n#include "recomp_runtime.h"\n}')
        h=h.replace('int main(void) {',stub+'int main(void) {\n  Impl impl;')
        h=h.replace('hm.load=read_cb; hm.store=write_cb;', 'hm.user=&impl; hm.load=Impl::HostLoad; hm.store=Impl::HostStore;')
        h=h.replace('assert(stores==before+1);','assert(stores==before+size);')
        h=h.replace('assert(loads==before+1);','assert(loads==before+size);')
        h=h.replace('  puts("PASS:', r'''  unsigned before=loads; assert(Impl::HostLoad(&impl,UINT64_MAX,8)==0); assert(loads==before);
  before=stores; Impl::HostStore(&impl,UINT64_MAX,8,1); assert(stores==before);
  assert(Impl::HostLoad(&impl,0,3)==0);
  puts("PASS:''')
        with tempfile.TemporaryDirectory(prefix='ihorizon-host-memory-') as tmp:
            out=Path(tmp)
            run([os.environ.get('CXX','c++'),'-std=c++20','-O0','-I'+str(ROOT/'src'),ROOT/'research/ios/tests/export_synthetic.cpp','-o',out/'emit'])
            run([out/'emit',out/'modules'])
            module=out/'modules/main'
            (module/'host_memory_test.cpp').write_text(h)
            run([os.environ.get('CC','cc'),'-std=c11','-O1','-DSUYU_HOSTED_RECOMP=1','-DRECOMP_STATIC_HOST=1','-ffunction-sections','-fdata-sections','-c',module/'recomp_runtime.c','-o',out/'runtime.o'])
            run([os.environ.get('CXX','c++'),'-std=c++20','-O1','-ffunction-sections','-fdata-sections',module/'host_memory_test.cpp',out/'runtime.o',LINK_GC,'-lm','-o',out/'test'])
            self.assertIn('PASS:',run([out/'test']))

    def test_real_emitted_memory(self):
        with tempfile.TemporaryDirectory(prefix='ihorizon-memory-') as tmp:
            out=Path(tmp)
            run([os.environ.get('CXX','c++'),'-std=c++20','-O0','-I'+str(ROOT/'src'),ROOT/'research/ios/tests/export_synthetic.cpp','-o',out/'emit'])
            run([out/'emit',out/'modules'])
            module=out/'modules/main'
            (module/'memory_test.c').write_text(HARNESS)
            run([os.environ.get('CC','cc'),'-std=c11','-O1','-DSUYU_HOSTED_RECOMP=1','-DRECOMP_STATIC_HOST=1','-ffunction-sections','-fdata-sections',module/'memory_test.c',LINK_GC,'-lm','-o',out/'test'])
            self.assertIn('PASS:',run([out/'test']))
            source=(module/'recomp_runtime.c').read_text()
            old=source.replace(HELPER,'')
            for size in (2,4,8):
                old=old.replace(f'recomp_scalar_same_page(c->host_mem,a,{size})?recomp_host_ptr(c,a):0','recomp_host_ptr(c,a)')
            self.assertEqual(patch(old),source)
            with self.assertRaises(ValueError): patch(source)
            # Prove the regression is detected against the pre-fix emitted runtime.
            (module/'recomp_runtime.c').write_text(old)
            run([os.environ.get('CC','cc'),'-std=c11','-O1','-DSUYU_HOSTED_RECOMP=1','-DRECOMP_STATIC_HOST=1','-ffunction-sections','-fdata-sections',module/'memory_test.c',LINK_GC,'-lm','-o',out/'old-test'])
            result=subprocess.run([out/'old-test'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            self.assertNotEqual(result.returncode,0)

if __name__=='__main__': unittest.main(verbosity=2)

