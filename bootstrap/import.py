"""将已校验的项目归档恢复为原始字节，仅用于本次仓库初始化。"""
import base64
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import shutil
import tarfile

ROOT = Path.cwd().resolve()
BOOTSTRAP = ROOT / "bootstrap"
metadata = json.loads((BOOTSTRAP / "import.json").read_text(encoding="utf-8"))
encoded = "".join(path.read_text(encoding="ascii")
                  for path in sorted(BOOTSTRAP.glob("payload-*.b64")))
payload = base64.b64decode(encoded, validate=True)
if hashlib.sha256(payload).hexdigest() != metadata["sha256"]:
    raise ValueError("导入归档校验失败")
with tarfile.open(fileobj=io.BytesIO(payload), mode="r:gz") as archive:
    members = archive.getmembers()
    if len(members) != metadata["files"]:
        raise ValueError("导入文件数不匹配")
    seen = set()
    for member in members:
        name = PurePosixPath(member.name)
        if (not member.isfile() or name.is_absolute() or ".." in name.parts
                or "\\" in member.name or name.parts[0] in {".git", ".github", "bootstrap"}
                or member.name in seen):
            raise ValueError("拒绝越界路径、链接或重复文件")
        seen.add(member.name)
        target = ROOT.joinpath(*name.parts)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(archive.extractfile(member).read())
        target.chmod(0o755 if target.suffix == ".sh" else 0o644)
manifest = json.loads((ROOT / "agent/publish/manifest.json").read_text(encoding="utf-8"))
for item in manifest["files"]:
    data = (ROOT / item["path"]).read_bytes()
    if len(data) != item["bytes"] or hashlib.sha256(data).hexdigest() != item["sha256"]:
        raise ValueError("导入文件不匹配：" + item["path"])
# 归档恢复后删除传输副本，固件以ELF/KO/TAR原样进入Git和Release。
shutil.rmtree(BOOTSTRAP)
print("Verified project files:", len(members))
