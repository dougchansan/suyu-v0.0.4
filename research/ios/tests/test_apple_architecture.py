"""Exercise Apple architecture lists without requiring an Apple SDK."""

import pathlib
import subprocess
import tempfile
import unittest


MODULE = pathlib.Path(__file__).resolve().parents[3] / "externals/cmake-modules/DetectArchitecture.cmake"


class AppleArchitectureTest(unittest.TestCase):
    def test_architecture_lists(self):
        for architectures in ("arm64", "arm64;x86_64"):
            with self.subTest(architectures=architectures), tempfile.TemporaryDirectory() as tmp:
                source = pathlib.Path(tmp)
                (source / "CMakeLists.txt").write_text(
                    'cmake_minimum_required(VERSION 3.22)\n'
                    'project(architecture_probe LANGUAGES NONE)\n'
                    f'set(CMAKE_OSX_ARCHITECTURES "{architectures}")\n'
                    f'include("{MODULE.as_posix()}")\n'
                    'if(NOT "${ARCHITECTURE}" STREQUAL "${CMAKE_OSX_ARCHITECTURES}")\n'
                    '  message(FATAL_ERROR "Architecture list was not preserved")\n'
                    'endif()\n'
                    'get_directory_property(definitions COMPILE_DEFINITIONS)\n'
                    'foreach(arch IN LISTS CMAKE_OSX_ARCHITECTURES)\n'
                    '  if(NOT ARCHITECTURE_${arch})\n'
                    '    message(FATAL_ERROR "Missing architecture variable: ${arch}")\n'
                    '  endif()\n'
                    '  if(NOT "ARCHITECTURE_${arch}=1" IN_LIST definitions)\n'
                    '    message(FATAL_ERROR "Missing architecture definition: ${arch}")\n'
                    '  endif()\n'
                    'endforeach()\n',
                    encoding="utf-8",
                )
                result = subprocess.run(
                    ["cmake", "-S", str(source), "-B", str(source / "build"), "-Werror=dev"],
                    capture_output=True, text=True,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
