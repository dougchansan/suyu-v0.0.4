#!/usr/bin/env python3
"""Build the utility against an already-built Linux Suyu tree without reconfiguring it."""
import argparse
import json
import pathlib
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("build", type=pathlib.Path)
parser.add_argument("output", type=pathlib.Path)
args = parser.parse_args()
build = args.build.resolve()
output = args.output.resolve()
source = pathlib.Path(__file__).with_name("export_owner_content.cpp").resolve()
commands = json.loads((build / "compile_commands.json").read_text())
entry = next(item for item in commands if item["file"].endswith("/file_sys/card_image.cpp"))
compile_args = shlex.split(entry["command"])
object_file = str(output) + ".o"
compile_args[compile_args.index("-o") + 1] = object_file
compile_args[-1] = str(source)
subprocess.run(compile_args, cwd=build, check=True)
commands_text = subprocess.check_output(
    ["ninja", "-C", str(build), "-t", "commands", "suyu-cmd"], text=True
)
link = shlex.split(commands_text.strip().splitlines()[-1])
link = link[link.index("&&") + 1:]
link = link[:link.index("&&")]
link = [part for part in link if not part.endswith(".cpp.o")
        and not part.startswith("-Wl,--dependency-file")]
link.insert(1, object_file)
link[link.index("-o") + 1] = str(output)
subprocess.run(link, cwd=build, check=True)
