import os
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parent
OUTPUT_DIR = ROOT / "bin"
SOURCES = (
    ROOT / "src" / "PluginLoader.cpp",
    ROOT / "src" / "main.cpp",
)


def main():
    missing = [source for source in SOURCES if not source.is_file()]
    if missing:
        paths = "\n".join(f"  - {source.relative_to(ROOT)}" for source in missing)
        raise SystemExit(f"Required source files are missing:\n{paths}")

    requested_compiler = os.environ.get("CXX", "g++")
    compiler = shutil.which(requested_compiler)
    if compiler is None:
        raise SystemExit(
            f"Could not find {requested_compiler!r}. Install an x86_64 "
            "MinGW-w64 toolchain or set the CXX environment variable."
        )

    result = subprocess.run(
        [compiler, "-dumpmachine"],
        check=True,
        capture_output=True,
        text=True,
    )
    target = result.stdout.strip().lower()
    if "x86_64" not in target or not (
        "mingw" in target or "windows" in target
    ):
        raise SystemExit(
            f"{compiler} targets {target!r}; an x86_64 Windows "
            "MinGW-w64 compiler is required."
        )

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    output = OUTPUT_DIR / "dinput8.dll"
    command = [
        compiler,
        "-shared",
        "-O2",
        "-std=c++17",
        "-static",
        "-static-libgcc",
        "-static-libstdc++",
        "-I",
        str(ROOT / "include"),
        *(str(source) for source in SOURCES),
        "-o",
        str(output),
    ]
    print("+ " + subprocess.list2cmdline(command))
    subprocess.run(command, cwd=ROOT, check=True)
    print(f"Built {output.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
