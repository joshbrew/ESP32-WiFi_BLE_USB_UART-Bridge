"""Standard-library-only bundle validation, shared with the root installer helper."""
import hashlib
import io
import json
import re
import stat
import zipfile
from pathlib import Path, PurePosixPath


MAX_BYTES = 8 * 1024 * 1024
REQUIRED = {"bridge/__init__.py", "bridge/__main__.py", "bridge/core.py", "bridge/config.py", "bridge/server.py", "web/index.html", "web/app.js", "web/app.css"}


def valid_path(name):
    path = PurePosixPath(name)
    return (str(path) == name and not path.is_absolute() and len(path.parts) == 2
            and path.parts[0] in {"bridge", "web"} and "\\" not in name
            and re.fullmatch(r"[A-Za-z0-9_.-]+", path.parts[1]) is not None
            and path.parts[1] not in {".", ".."}
            and path.suffix in ({".py"} if path.parts[0] == "bridge" else {".html", ".css", ".js"}))


def validate_bundle(payload):
    if len(payload) > MAX_BYTES:
        raise ValueError("update bundle exceeds 8 MiB")
    with zipfile.ZipFile(io.BytesIO(payload)) as archive:
        entries = archive.infolist()
        if not 2 <= len(entries) <= 128 or len({item.filename for item in entries}) != len(entries):
            raise ValueError("bundle has duplicate or excessive entries")
        if any(item.file_size > 1024 * 1024 or item.flag_bits & 1 or stat.S_ISLNK(item.external_attr >> 16) for item in entries):
            raise ValueError("bundle has oversized, encrypted, or symbolic-link entries")
        if sum(item.file_size for item in entries) > MAX_BYTES:
            raise ValueError("unpacked update exceeds 8 MiB")
        if "manifest.json" not in archive.namelist() or archive.getinfo("manifest.json").file_size > 65536:
            raise ValueError("bundle requires a bounded manifest.json")
        manifest = json.loads(archive.read("manifest.json"))
        if not isinstance(manifest, dict) or set(manifest) != {"format", "version", "files"} or manifest["format"] != "pi-controller-v1":
            raise ValueError("unsupported update manifest")
        if not isinstance(manifest["version"], str) or not re.fullmatch(r"[A-Za-z0-9_.+-]{1,64}", manifest["version"]):
            raise ValueError("invalid update version")
        hashes = manifest["files"]
        if not isinstance(hashes, dict) or not REQUIRED <= set(hashes) or set(hashes) != set(archive.namelist()) - {"manifest.json"}:
            raise ValueError("manifest and bundle contents do not match")
        files = {}
        for name, expected in hashes.items():
            if not valid_path(name) or not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected):
                raise ValueError("unsafe bundle path or hash")
            content = archive.read(name)
            if hashlib.sha256(content).hexdigest() != expected:
                raise ValueError("update file checksum mismatch")
            if name.endswith(".py"):
                compile(content, name, "exec")
            files[name] = content
    return dict(version=manifest["version"], releaseId=hashlib.sha256(payload).hexdigest(), files=files)


def build_bundle(directory, version):
    base = Path(directory)
    files = {str(path.relative_to(base)).replace("\\", "/"): path.read_bytes()
             for folder in ("bridge", "web") for path in sorted((base / folder).iterdir())
             if path.is_file() and valid_path(str(path.relative_to(base)).replace("\\", "/"))}
    manifest = dict(format="pi-controller-v1", version=version,
                    files={name: hashlib.sha256(content).hexdigest() for name, content in files.items()})
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, content in {"manifest.json": json.dumps(manifest, sort_keys=True).encode(), **files}.items():
            info = zipfile.ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, content)
    payload = output.getvalue()
    validate_bundle(payload)
    return payload
