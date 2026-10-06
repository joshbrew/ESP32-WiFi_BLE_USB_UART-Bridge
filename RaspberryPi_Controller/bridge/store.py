"""Atomic, checksummed settings; runtime arming/output state is never persisted."""
import hashlib
import json
import os
from pathlib import Path


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")


class Store:
    def __init__(self, directory, filename="controller.json", max_bytes=65536):
        self.path = Path(directory) / filename
        self.max_bytes = max_bytes

    def load_generic(self, default):
        if not self.path.exists():
            return default
        if self.path.stat().st_size > self.max_bytes:
            raise ValueError("saved data exceeds limit")
        record = json.loads(self.path.read_text(encoding="utf-8"))
        data = record["data"]
        if record.get("version") != 1 or record.get("sha256") != hashlib.sha256(canonical(data)).hexdigest():
            raise ValueError("saved data failed checksum/version validation")
        return data

    def load(self):
        if not self.path.exists():
            return {"settings": None, "profiles": {}, "selected": "", "routines": {}}
        data = self.load_generic(None)
        if set(data) != {"settings", "profiles", "selected", "routines"}:
            raise ValueError("invalid saved controller data")
        return data

    def save(self, data):
        self.save_generic(data)

    def save_generic(self, data):
        payload = canonical({"version": 1, "data": data, "sha256": hashlib.sha256(canonical(data)).hexdigest()})
        if len(payload) > self.max_bytes:
            raise ValueError("saved data exceeds limit")
        self.path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.path.with_suffix(".tmp")
        try:
            with temporary.open("wb") as file:
                file.write(payload)
                file.flush()
                os.fsync(file.fileno())
            os.replace(temporary, self.path)
            if os.name == "posix":
                descriptor = os.open(str(self.path.parent), os.O_RDONLY | os.O_DIRECTORY)
                try:
                    os.fsync(descriptor)
                finally:
                    os.close(descriptor)
        finally:
            temporary.unlink(missing_ok=True)
