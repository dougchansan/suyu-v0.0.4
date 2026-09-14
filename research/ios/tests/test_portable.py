#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run a content-free build/negative-test matrix; keep compiler logs locally."""
import os
import plistlib
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from make_fixture import create

HERE = Path(__file__).resolve().parents[1]
BASE = Path(os.environ.get('SWITCH_AOT_TEST_OUTPUT', tempfile.mkdtemp(prefix='switch-aot-tests-')))
BASE.mkdir(parents=True, exist_ok=True)

def command(args, success=True):
    result = subprocess.run([str(a) for a in args], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if success and result.returncode:
        raise AssertionError(result.stdout)
    if not success and not result.returncode:
        raise AssertionError('Unexpected success: ' + ' '.join(map(str, args)))
    return result.stdout

class PortableTests(unittest.TestCase):
    def config(self, name, export=None, flags=(), success=True):
        build = BASE/name
        args = ['cmake', '-S', HERE, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release']
        if export is not None: args.append('-DSWITCH_AOT_EXEFS='+str(export))
        log = command(args+list(flags), success)
        (BASE/(name+'.configure.log')).write_text(log)
        return build, log
    def build(self, build, success=True):
        log = command(['cmake', '--build', build, '--parallel', '2'], success)
        (BASE/(build.name+'.build.log')).write_text(log)
        return log
    def fixture(self, name):
        directory = BASE/name
        create(directory)
        return directory
    def test_01_release_dispatch_and_symbol_screen(self):
        build, _ = self.config('positive')
        self.build(build)
        log = command(['ctest', '--test-dir', build, '--output-on-failure'])
        (BASE/'positive.ctest.log').write_text(log)
        output = command([build/'switch_aot_probe'])
        self.assertIn('PASS:', output)
        symbols = command(['nm', '-C', build/'switch_aot_probe'])
        self.assertNotRegex(symbols, r'Dynarmic::|\b(?:dlopen|mprotect|mmap|pthread_jit_write_protect_np)\b')
        (BASE/'positive.symbols.log').write_text(symbols)
    def test_02_jit_configuration_rejected(self):
        _, log = self.config('reject-jit', flags=['-DSUYU_NO_JIT=OFF'], success=False)
        self.assertIn('requires SUYU_NO_JIT=ON', log)
    def test_03_unfixed_index_symbols_do_collide(self):
        build, _ = self.config('reject-collisions', flags=['-DSWITCH_AOT_TEST_OMIT_SYMBOL_FIX=ON'])
        log = self.build(build, success=False)
        self.assertRegex(log, 'multiple definition|duplicate symbol')
        self.assertIn('recomp_build_index', log)
    def test_04_mixed_headers_rejected(self):
        export = self.fixture('mixed-input')
        with (export/'sdk/recomp_runtime.h').open('a') as f: f.write('\n/* different revision */\n')
        _, log = self.config('reject-mixed', export, success=False)
        self.assertIn('Mixed runtime/header revisions', log)
    def test_05_uniform_but_wrong_abi_rejected(self):
        export = self.fixture('wrong-abi-input')
        for module in ('main', 'rtld', 'sdk'):
            path = export/module/'recomp_runtime.h'
            path.write_text(path.read_text().replace('int chain_budget;', 'uint64_t wrong_padding; int chain_budget;'))
        build, _ = self.config('reject-abi', export)
        log = self.build(build, success=False)
        self.assertIn('chain budget ABI', log)
    def test_06_spaces_and_brackets(self):
        export = self.fixture('inputs [synthetic] with spaces')
        build, _ = self.config('path-escaping', export)
        self.build(build)
        self.assertIn('PASS:', command([build/'switch_aot_probe']))
    def test_07_private_probe_never_executes_blocks(self):
        export = self.fixture('private-shaped-input')
        (export/'SYNTHETIC_FIXTURE.txt').unlink()
        for module in ('main', 'rtld', 'sdk'):
            path = export/module/'recomp_runtime.c'
            path.write_text('#include <stdlib.h>\n#include "recomp_runtime.h"\nvoid synthetic_add_svc(GuestContext* c) { (void)c; abort(); }\n')
        build, _ = self.config('private-shaped', export)
        self.build(build)
        self.assertIn('No game code executed', command([build/'switch_aot_probe']))
    def test_08_empty_entitlements_and_separate_identity(self):
        with (HERE/'apple/Research.entitlements').open('rb') as f:
            self.assertEqual(plistlib.load(f), {})
        with (HERE/'apple/Info.plist.in').open('rb') as f:
            self.assertEqual(plistlib.load(f)['CFBundleName'], 'iHorizon')

if __name__ == '__main__':
    print('Local evidence directory:', BASE, flush=True)
    unittest.main(verbosity=2)
