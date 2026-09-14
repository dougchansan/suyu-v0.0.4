"""Evaluate backend selection without a compiler, SDK, or downloaded dependencies."""

import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]

# Script mode records target mutations while CMake itself evaluates the real
# conditions. This checks selected sources, links and definitions, not C++ builds.
RECORD_TARGETS = """
macro(add_library target)
    list(APPEND ${target}_sources ${ARGN})
endmacro()
macro(target_sources target)
    list(APPEND ${target}_sources ${ARGN})
endmacro()
macro(target_link_libraries target)
    list(APPEND ${target}_links ${ARGN})
endmacro()
macro(target_compile_definitions target)
    list(APPEND ${target}_definitions ${ARGN})
endmacro()
macro(target_compile_options)
endmacro()
macro(target_include_directories)
endmacro()
macro(create_target_directory_groups)
endmacro()
"""


class SDLConfigurationTest(unittest.TestCase):
    def test_backend_selection(self):
        for android, sdl in ((False, True), (False, False), (True, False)):
            with self.subTest(android=android, sdl=sdl), tempfile.TemporaryDirectory() as tmp:
                directory = pathlib.Path(tmp)
                script = RECORD_TARGETS
                script += f'set(ANDROID {"ON" if android else "OFF"})\n'
                script += f'set(ENABLE_SDL3 {"ON" if sdl else "OFF"})\n'
                # Keep unrelated optional backends out of this test matrix.
                script += 'set(ENABLE_LIBUSB OFF)\nset(ENABLE_CUBEB OFF)\n'
                for target in ("input_common", "audio_core"):
                    script += f'include("{ROOT.as_posix()}/src/{target}/CMakeLists.txt")\n'
                    for property_name in ("sources", "links", "definitions"):
                        output = directory / f"{target}_{property_name}.txt"
                        script += (
                            f'file(WRITE "{output.as_posix()}" '
                            f'"${{{target}_{property_name}}}")\n'
                        )
                script_path = directory / "probe.cmake"
                script_path.write_text(script, encoding="utf-8")
                result = subprocess.run(
                    ["cmake", "-Werror=dev", "-P", str(script_path)],
                    capture_output=True, text=True,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

                def selected(target, property_name):
                    return set((directory / f"{target}_{property_name}.txt").read_text().split(";"))

                input_sources = selected("input_common", "sources")
                audio_sources = selected("audio_core", "sources")
                for source in (
                    "drivers/sdl_driver.cpp", "drivers/joycon.cpp",
                    "helpers/joycon_driver.cpp", "helpers/joycon_protocol/common_protocol.cpp",
                ):
                    self.assertEqual(source in input_sources, sdl, source)
                self.assertEqual("sink/sdl3_sink.cpp" in audio_sources, sdl)
                for target in ("input_common", "audio_core"):
                    self.assertEqual("SDL3::SDL3" in selected(target, "links"), sdl)
                    self.assertEqual("HAVE_SDL3" in selected(target, "definitions"), sdl)
                self.assertEqual("drivers/android.cpp" in input_sources, android)
                self.assertEqual("android" in selected("input_common", "links"), android)
                self.assertEqual("sink/oboe_sink.cpp" in audio_sources, android)
                self.assertEqual("oboe" in selected("audio_core", "links"), android)
                self.assertEqual("HAVE_OBOE" in selected("audio_core", "definitions"), android)
                # Disabling a host backend must retain platform-independent code.
                self.assertIn("input_engine.cpp", input_sources)
                self.assertIn("sink/sink_details.cpp", audio_sources)


if __name__ == "__main__":
    unittest.main()
