import asyncio
import json
import os
from pathlib import Path
import zipfile

from .release import MAX_BYTES, validate_bundle


class Updates:
    def __init__(self, controller, admin):
        self.controller, self.admin = controller, admin
        self.directory = Path(controller.config["data_dir"])
        self.busy = False
        self.result = dict(phase="idle")
        parent = Path(__file__).resolve().parent.parent
        self.release_id = parent.name if len(parent.name) == 64 else "development"
        status = self.state()
        self.busy = status.get("phase") == "applying" and status.get("candidate") == self.release_id

    def state(self):
        record = self.directory / "update-status.json"
        if record.is_file() and record.stat().st_size <= 8192:
            try:
                external = json.loads(record.read_text())
                initial = not self.busy and self.result.get("phase") == "idle"
                matches = bool(self.result.get("candidate")) and external.get("candidate") == self.result.get("candidate") if isinstance(external, dict) else False
                if isinstance(external, dict) and (initial or matches):
                    self.result = external
                    if external.get("phase") in {"failed", "rolled-back", "committed"}:
                        self.busy = False
            except (ValueError, OSError):
                pass
        return dict(self.result, busy=self.busy, releaseId=self.release_id,
                    enabled=bool(self.controller.config["update_token"]))

    async def upload(self, chunks, expected_sha=""):
        if self.controller.features.busy:
            raise ValueError("an administrative operation is already active")
        self.busy = True
        self.result = dict(phase="uploading")
        self.controller.stop_all("software update")
        self.controller.clear_queue()
        self.directory.mkdir(parents=True, exist_ok=True)
        temporary = self.directory / "update-upload.tmp"
        try:
            size = 0
            with temporary.open("wb") as file:
                async for chunk in chunks:
                    size += len(chunk)
                    if size > MAX_BYTES:
                        raise ValueError("update exceeds 8 MiB")
                    file.write(chunk)
                file.flush()
                os.fsync(file.fileno())
            try:
                bundle = await asyncio.to_thread(validate_bundle, temporary.read_bytes())
            except (zipfile.BadZipFile, zipfile.LargeZipFile, SyntaxError, KeyError, TypeError, NotImplementedError, RuntimeError, EOFError) as error:
                raise ValueError("invalid Pi update bundle") from error
            if expected_sha and expected_sha.lower() != bundle["releaseId"]:
                raise ValueError("uploaded bundle does not match declared SHA-256")
            os.replace(temporary, self.directory / "pending-update.zip")
            self.result = dict(phase="applying", version=bundle["version"], candidate=bundle["releaseId"])
            result = await self.admin.run("update")
            if result.get("simulated"):
                self.result["phase"] = "validated-simulation"
            else:
                self.result["phase"] = "applying"
            return dict(ok=True, **self.result)
        except Exception:
            self.result["phase"] = "failed"
            raise
        finally:
            temporary.unlink(missing_ok=True)
            if self.result.get("phase") != "applying":
                self.busy = False
