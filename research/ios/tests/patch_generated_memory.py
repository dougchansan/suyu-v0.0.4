#!/usr/bin/env python3
"""Patch only private staged generated runtimes; fail closed on unknown revisions."""
import argparse
import re
from pathlib import Path

HELPER = '''/* Direct scalar access must remain inside one guest page and the address space.
   Crossing accesses use the host callback, which resolves each guest page. */
static int recomp_scalar_same_page(const RecompHostMem* hm, uint64_t a, uint64_t bytes){
  uint64_t psz;
  if(!hm || !hm->page_entries || hm->page_bits >= 64) return 0;
  a &= 0xffffffffffffULL;
  if(a >= hm->address_space_max || bytes > hm->address_space_max - a) return 0;
  psz = (uint64_t)1 << hm->page_bits;
  return bytes <= psz - (a & (psz - 1));
}

'''

def patch(source):
    if 'static int recomp_scalar_same_page(' in source:
        raise ValueError('Already patched; refusing to guess revision')
    anchor = 'uint64_t recomp_load8 (GuestContext* c,uint64_t a){'
    if source.count(anchor) != 1:
        raise ValueError('Expected exactly one scalar helper group')
    for op in ('load', 'store'):
        for width in (16, 32, 64):
            pattern = r'(recomp_' + op + str(width) + r'\(GuestContext\* c,uint64_t a[^\n]*\n  )unsigned char\* p=recomp_host_ptr\(c,a\);'
            replacement = r'\g<1>unsigned char* p=recomp_scalar_same_page(c->host_mem,a,' + str(width // 8) + ')?recomp_host_ptr(c,a):0;'
            source, count = re.subn(pattern, replacement, source)
            if count != 1:
                raise ValueError(f'Expected exactly one unmodified {op}{width}, got {count}')
    return source.replace(anchor, HELPER + anchor)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runtime', type=Path, nargs='+', help='Explicit private staged recomp_runtime.c files')
    args = parser.parse_args()
    # Validate every module before writing any module.
    changes = []
    for path in args.runtime:
        raw = path.read_bytes()
        newline = '\r\n' if b'\r\n' in raw else '\n'
        changes.append((path, patch(raw.decode().replace('\r\n', '\n')).replace('\n', newline).encode()))
    for path, data in changes:
        path.write_bytes(data)
        print('Patched scalar page spans:', path)
