import importlib.util
from pathlib import Path
import tempfile
import unittest


TOOLS = Path(__file__).resolve().parents[1] / "tools"


def load(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


extract_nso_text = load("extract_nso_text")
apply_runtime_root = load("apply_runtime_root")


class RuntimeToolTests(unittest.TestCase):
    def test_lz4_literals_and_overlapping_match(self):
        self.assertEqual(extract_nso_text.decompress_lz4_block(b"\x50hello", 5), b"hello")
        self.assertEqual(extract_nso_text.decompress_lz4_block(b"\x11a\x01\x00", 6), b"aaaaaa")

    def test_runtime_root_is_sorted_and_updates_count(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            source_dir = project / "src"
            source_dir.mkdir()
            unit = source_dir / "recompiled_main_0.c"
            unit.write_text(
                "void blk_main_0000000000000100(GuestContext* c){}\n"
                "void blk_main_0000000000000200(GuestContext* c){}\n"
                "const struct _recomp_ent _seg_main_0[] = {\n"
                "  {0x100ULL, blk_main_0000000000000100},\n"
                "  {0x200ULL, blk_main_0000000000000200},\n"
                "};\nconst unsigned _segn_main_0 = 2U;\n"
            )
            function = "void blk_main_0000000000000150(GuestContext* c){}\n"
            changed = apply_runtime_root.apply(project, "main", 0x150, function)
            self.assertEqual(changed, unit)
            result = unit.read_text()
            self.assertLess(result.index("{0x100ULL"), result.index("{0x150ULL"))
            self.assertLess(result.index("{0x150ULL"), result.index("{0x200ULL"))
            self.assertIn("const unsigned _segn_main_0 = 3U;", result)


if __name__ == "__main__":
    unittest.main()
