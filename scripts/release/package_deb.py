"""Build a native Ubuntu-compatible .deb; infer Depends from the actual ELF."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", default=".build/debian", type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r"v\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)?", args.version):
        parser.error("invalid release version")
    arch = {"x86_64": "amd64", "aarch64": "arm64"}.get(platform.machine())
    if platform.system() != "Linux" or not arch:
        parser.error("requires a native Linux amd64/arm64 build host")
    binary = args.binary.resolve(strict=True)
    description = subprocess.check_output(["file", str(binary)], text=True)
    if "ELF" not in description or ("x86-64" if arch == "amd64" else "aarch64") not in description:
        parser.error("binary architecture does not match the native package host")
    result = subprocess.run([str(binary)], input=b"version\nexit\n", capture_output=True, timeout=15)
    if result.returncode != 0 or ("version " + args.version).encode() not in result.stdout:
        parser.error("binary does not report the requested release version")
    version = args.version[1:].replace("-", "~", 1)
    args.output.mkdir(parents=True, exist_ok=True)
    output = (args.output / f"nero_{version}_{arch}.deb").resolve()
    with tempfile.TemporaryDirectory(prefix="nero-deb-") as tmp:
        work = Path(tmp)
        (work / "debian").mkdir()
        (work / "debian/control").write_text("Source: nero\nSection: net\nPriority: optional\n"
            "Maintainer: NERO maintainers <181492817+thekiran@users.noreply.github.com>\n\n"
            "Package: nero\nArchitecture: any\nDescription: Native C11 terminal and network diagnostics\n")
        dependencies = subprocess.check_output(["dpkg-shlibdeps", "-O", "-e" + str(binary)],
                                               cwd=work, text=True)
        depends = next(line.removeprefix("shlibs:Depends=") for line in dependencies.splitlines()
                       if line.startswith("shlibs:Depends="))
        package = work / "package"
        (package / "DEBIAN").mkdir(parents=True)
        (package / "usr/bin").mkdir(parents=True)
        shutil.copy2(binary, package / "usr/bin/nero")
        (package / "usr/bin/nero").chmod(0o755)
        docs = package / "usr/share/doc/nero"
        docs.mkdir(parents=True)
        shutil.copy2(ROOT / "LICENSE.txt", docs / "copyright")
        shutil.copy2(ROOT / "docs/installation.md", docs / "README.md")
        (docs / "BUILD-INFO.json").write_text(json.dumps({
            "version": args.version, "architecture": arch,
            "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
            "dependencies": depends,
        }, indent=2) + "\n")
        (package / "DEBIAN/control").write_text(f"Package: nero\nVersion: {version}\n"
            f"Architecture: {arch}\nSection: net\nPriority: optional\n"
            "Maintainer: NERO maintainers <181492817+thekiran@users.noreply.github.com>\n"
            f"Depends: {depends}\nHomepage: https://github.com/thekiran/nero\n"
            "Description: Native C11 terminal and experimental network diagnostics\n"
            " For isolated and explicitly authorized laboratory environments.\n")
        subprocess.run(["dpkg-deb", "--root-owner-group", "--build", str(package), str(output)], check=True)
        extracted = work / "extracted"
        subprocess.run(["dpkg-deb", "--extract", str(output), str(extracted)], check=True)
        if (extracted / "usr/bin/nero").read_bytes() != binary.read_bytes():
            raise RuntimeError("Debian archive binary mismatch")
        if subprocess.check_output(["dpkg-deb", "--field", str(output), "Architecture"], text=True).strip() != arch:
            raise RuntimeError("Debian archive architecture mismatch")
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    output.with_name(output.name + ".sha256").write_text(f"{digest}  {output.name}\n")
    print(output)


if __name__ == "__main__": main()
