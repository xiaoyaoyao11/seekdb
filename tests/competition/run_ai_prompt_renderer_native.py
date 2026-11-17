"""Compile real AI prompt utilities and native regression cases using a seekdb build.

Run inside Linux/WSL; this does NOT build or substitute a complete SQL server:
  python tests/competition/run_ai_prompt_renderer_native.py \
    --build-dir /path/to/initialized/seekdb/build_release \
    --source-root /path/to/source/under/test

The build must already contain the real oblib, Rust exports and dependencies.
The source checkout must be compatible with that build's generated headers.
Exact commands, tested source hash and process results are retained in output-dir.
"""

import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    source_root = args.source_root.resolve()
    output = (args.output_dir or build / "competition-native" / "ai-prompt").resolve()
    output.mkdir(parents=True, exist_ok=True)
    source = source_root / "src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.cpp"
    test = Path(__file__).resolve().with_name("ai_prompt_renderer_native.cpp")
    entries = json.loads((build / "compile_commands.json").read_text())
    entry = next(item for item in entries
                 if "expr" in item["file"] and Path(item["file"]).is_file()
                 and "ob_ai_func_utils.cpp" in Path(item["file"]).read_text())
    original = entry.get("arguments") or shlex.split(entry["command"])
    compiler, flags = original[0], []
    index = 1
    while index < len(original):
        token = original[index]
        if token in ("-o", "-c"):
            index += 2
            continue
        if (token.startswith("-O") or token.startswith("-g")
                or token == "-fdebug-info-for-profiling"):
            index += 1
            continue
        flags.append(token)
        index += 1
    flags += ["-O0", "-g0", "-ffunction-sections", "-fdata-sections"]
    report = {"source": str(source),
              "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
              "test": str(test), "steps": []}

    def run(name, command):
        print(name, "START", flush=True)
        result = subprocess.run(command, cwd=entry["directory"], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (output / (name + ".log")).write_text(result.stdout)
        report["steps"].append({"name": name, "argv": command,
                                "returncode": result.returncode})
        (output / "result.json").write_text(json.dumps(report, indent=2))
        print(result.stdout, end="", flush=True)
        print(name, "EXIT=" + str(result.returncode), flush=True)
        return result.returncode

    print("TESTED_SOURCE:", source, flush=True)
    print("SOURCE_SHA256:", report["source_sha256"], flush=True)
    for name, filename in (("utils", source), ("test", test)):
        rc = run("compile_" + name,
                 [compiler, *flags, "-c", str(filename), "-o", str(output / (name + ".o"))])
        if rc:
            return 2
    link_directory = build / "src/observer"
    tokens = shlex.split((link_directory / "CMakeFiles/seekdb.dir/link.txt").read_text())
    libraries = []
    for token in tokens:
        if token.endswith(".a"):
            library = (link_directory / token).resolve()
            # No SQL/server object archives: the tested TU supplies its own symbols.
            if (library.is_file()
                    and not any(directory in library.parents
                                for directory in (build / "src/sql", build / "src/share",
                                                  build / "src/storage", build / "src/observer"))):
                libraries.append(str(library))
        elif token.startswith("-L"):
            libraries.append(token)
    libraries = list(dict.fromkeys(libraries))
    devtools = Path(compiler).parent.parent
    executable = output / "ai_prompt_native"
    link = [compiler, "--gcc-toolchain=" + str(devtools),
            "-fuse-ld=" + str(devtools / "bin/ld.lld"),
            "-static-libgcc", "-static-libstdc++", "-Wl,--gc-sections", "-no-pie",
            str(output / "test.o"), str(output / "utils.o"),
            "-Wl,--start-group", *libraries, "-Wl,--end-group",
            "-lpthread", "-ldl", "-lrt", "-lm", "-o", str(executable)]
    if run("link", link):
        return 3
    return run("run", [str(executable)])


if __name__ == "__main__":
    raise SystemExit(main())
