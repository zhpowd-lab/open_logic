"""整理发布副本并校验v15对应关系；不改变原始证据或连接现场装置。"""
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import tarfile

ROOT = Path(__file__).resolve().parents[2]
CAPTURES = (
    "di-100khz-v6", "di-1mhz-v12", "di-1mhz-final-v13",
    "di-1mhz-final-v13-repeat", "cpu3-pattern-final-v13",
)
SCREENSHOTS = (
    "desktop-final.png", "desktop-light.png", "narrow-dark.png", "narrow-light.png",
    "device-live-desktop.png", "device-live-narrow.png", "device-live-narrow-light.png",
    "device-mock-desktop.png", "device-mock-narrow.png", "device-mock-narrow-light.png",
)
EVIDENCE = (
    "browser-results.json", "browser-results.txt", "node-tests.txt", "node-final-results.txt",
    "reference-check.json", "service.stdout.log", "service.stderr.log",
    "device-tests-review.log", "device-build-v15.log", "device-live-results.txt",
    "device-browser-mock-results.txt", "device-browser-real-results.txt",
    "device-v15-final-state.txt", "device-start-v15.txt", "v15-source-check.json",
    "v15-release-check.json", "registers-live-v14.txt", "pinmux-live-v14.txt",
    "pinconf-live-v14.txt", "gpio-live-v14.txt", "pio-control-v15.txt",
)
SKIP_PARTS = {"__pycache__", "node_modules", ".git"}
SECRET_RULES = {
    "github-token": rb"(?:github_pat_|gh[pousr]_)[A-Za-z0-9_]{20,}",
    "private-key": rb"-----BEGIN (?:[A-Z ]+ )?PRIVATE KEY-----",
    "api-key": rb"sk-(?:proj-|ant-)?[A-Za-z0-9_-]{32,}",
    "credential-url": rb"https?://[^\s/@:]+:[^\s/@]+@",
}


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def checked_path(name):
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts or "\\" in name:
        raise ValueError("归档路径越界")
    return path.as_posix().removeprefix("./")


def write_member(archive, name, data, executable=False):
    info = tarfile.TarInfo(checked_path(name))
    info.size = len(data)
    info.mode = 0o755 if executable else 0o644
    info.mtime = 0
    archive.addfile(info, io.BytesIO(data))


def copy_public(path, destination, records):
    relative = path.relative_to(ROOT).as_posix()
    if SKIP_PARTS.intersection(path.parts) or path.suffix == ".pyc":
        return
    data = path.read_bytes()
    sanitized = False
    if relative.startswith("agent/") and path.suffix in {".json", ".txt", ".log"}:
        # 发布副本隐去设备会话字段；本地原始证据保留，原码不经过此分支。
        data, count = re.subn(rb'("sourceEpoch"\s*:\s*")[^"\r\n]*',
                              rb'\1redacted-for-publication', data)
        sanitized = bool(count)
    for label, pattern in SECRET_RULES.items():
        if re.search(pattern, data):
            raise ValueError(f"待发布文件命中凭证检查：{relative} ({label})")
    target = destination / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    records.append({"path": relative, "bytes": len(data), "sha256": sha256(data),
                    "sessionRedacted": sanitized})


def collect_files():
    files = {ROOT / name for name in ("README.md", ".gitattributes", ".gitignore")}
    for folder in ("app", "原理图", "agent/research", "agent/hardware", "agent/review"):
        files.update(path for path in (ROOT / folder).rglob("*") if path.is_file())
    files.update((ROOT / "agent").glob("*.md"))
    files.update(ROOT / "agent" / name for name in
                 ("reference-manifest.json", "device-link.py", "serial-command.ps1"))
    files.update((ROOT / "agent/recovery").glob("*.c"))
    files.add(ROOT / "agent/recovery/Makefile")
    verification = ROOT / "agent/verification"
    files.update(path for path in verification.iterdir() if path.suffix in {".js", ".mjs"})
    files.update(verification / name for name in EVIDENCE)
    files.update(path for path in (verification / "fixtures").iterdir() if path.is_file())
    for capture in CAPTURES:
        files.update(path for path in (verification / "device-captures" / capture).iterdir()
                     if path.is_file())
    files.update(ROOT / "agent/output/playwright" / name for name in SCREENSHOTS)
    files.update(path for path in (ROOT / "agent/publish").iterdir()
                 if path.suffix in {".md", ".py"})
    return sorted(files)


def checksum_entries(data):
    entries = {}
    for line in data.decode("ascii").splitlines():
        digest, name = line.split(None, 1)
        entries[checked_path(name.lstrip("*"))] = digest
    return entries


def package_v15(destination):
    release = destination / "app/device/release"
    runtime_data = (ROOT / "agent/build/release-v15.tar").read_bytes()
    with tarfile.open(fileobj=io.BytesIO(runtime_data), mode="r:") as archive:
        runtime = {checked_path(item.name): archive.extractfile(item).read()
                   for item in archive.getmembers() if item.isfile()}
    for name, digest in checksum_entries((release / "SHA256SUMS").read_bytes()).items():
        if sha256(runtime[name]) != digest or runtime[name] != (release / name).read_bytes():
            raise ValueError(f"v15运行包不匹配：{name}")
    (release / "open_logic-device-v15-20261016.tar").write_bytes(runtime_data)
    expected = checksum_entries((release / "SOURCE_SHA256SUMS").read_bytes())
    source_files = {}
    with tarfile.open(ROOT / "agent/build/device-v15-source.tar.gz", "r:gz") as archive:
        for member in archive.getmembers():
            if member.isfile() and "__pycache__" not in PurePosixPath(member.name).parts:
                source_files[checked_path(member.name)] = archive.extractfile(member).read()
    if set(source_files) != set(expected):
        raise ValueError("v15源码清单不完整或有未登记文件")
    source_path = release / "open_logic-device-v15-source.tar.gz"
    with source_path.open("wb") as output:
        with gzip.GzipFile(filename="", fileobj=output, mode="wb", mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w") as archive:
                for name, data in sorted(source_files.items()):
                    if sha256(data) != expected[name]:
                        raise ValueError(f"v15源码校验失败：{name}")
                    write_member(archive, name, data, name.endswith(".sh"))
    names = ("open_logic-device-v15-20261016.tar", source_path.name)
    checksums = "".join(f"{sha256((release / name).read_bytes())}  {name}\n" for name in names)
    (release / "DOWNLOAD_SHA256SUMS").write_text(checksums, encoding="ascii", newline="\n")
    return len(expected)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    destination = args.output.resolve()
    destination.mkdir(parents=True, exist_ok=False)
    records = []
    for path in collect_files():
        copy_public(path, destination, records)
    matched_source_count = package_v15(destination)
    existing = {item["path"] for item in records}
    for path in sorted((destination / "app/device/release").iterdir()):
        relative = path.relative_to(destination).as_posix()
        if relative not in existing:
            data = path.read_bytes()
            records.append({"path": relative, "bytes": len(data), "sha256": sha256(data),
                            "sessionRedacted": False})
    manifest = {"release": "v0.1.0-device-v15-preview", "files": sorted(records, key=lambda x: x["path"]),
                "matchedV15SourceFiles": matched_source_count}
    manifest_path = destination / "agent/publish/manifest.json"
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                             encoding="utf-8", newline="\n")
    print(json.dumps({"files": len(records) + 1, "matchedV15SourceFiles": matched_source_count,
                      "sessionRedactedFiles": sum(item["sessionRedacted"] for item in records),
                      "bytes": sum(item["bytes"] for item in records)}, ensure_ascii=False))


if __name__ == "__main__":
    main()
