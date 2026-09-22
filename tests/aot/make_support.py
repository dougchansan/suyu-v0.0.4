# SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate explicit test doubles for unrelated PerfStats filesystem/settings dependencies.

The production perf_stats.cpp, perf_stats.h and AOT headers are compiled unmodified.
This is a component integration test, NOT a build of the full Suyu core/HLE/GPU.
"""
from pathlib import Path
import sys

root = Path(sys.argv[1])
headers = {
    "common/common_types.h": "#pragma once\n#include <cstdint>\nusing u32 = std::uint32_t;\nusing u64 = std::uint64_t;\n",
    "common/settings.h": """#pragma once
namespace Settings {
struct TestSetting {
    bool value = false;
    bool GetValue() const { return value; }
    operator bool() const { return value; }
};
struct TestValues {
    TestSetting record_frame_times, use_multi_core, use_speed_limit;
};
inline TestValues values;
inline double SpeedLimit() { return 100; }
}
""",
    "common/fs/file.h": """#pragma once
#include <filesystem>
#include <string>
namespace Common::FS {
enum class FileAccessMode { Write };
enum class FileType { TextFile };
class IOFile {
public:
    IOFile(const std::filesystem::path&, FileAccessMode, FileType) {}
    std::size_t WriteString(const std::string& s) { return s.size(); }
};
}
""",
    "common/fs/fs.h": """#pragma once
#include <filesystem>
namespace Common::FS {
inline bool CreateParentDir(const std::filesystem::path&) { return true; }
}
""",
    "common/fs/path_util.h": """#pragma once
#include <filesystem>
namespace Common::FS {
enum class SuyuPath { LogDir };
inline std::filesystem::path GetSuyuPath(SuyuPath) { return {}; }
}
""",
    "fmt/chrono.h": """#pragma once
#include <ctime>
#include <iomanip>
#include <string>
namespace fmt {
template<class... T> std::string format(const char*, const T&...) { return "test.csv"; }
}
""",
    "fmt/ranges.h": "#pragma once\n",
}
for path, content in headers.items():
    target = root / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(content, encoding="utf-8")
