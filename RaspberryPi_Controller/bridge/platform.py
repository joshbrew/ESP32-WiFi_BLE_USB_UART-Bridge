"""Narrow asynchronous access to the optional installed OS management helper."""
import asyncio
import json


class AdminClient:
    def __init__(self, config):
        self.config = config

    async def run(self, operation, **values):
        if self.config["simulate"]:
            return dict(ok=True, simulated=True, role=values.get("settings", {}).get("wifiMode", "STA"))
        if operation != "update" and not self.config["enable_os_control"]:
            raise ValueError("OS control is disabled; enable_os_control and install the management helper first")
        process = await asyncio.create_subprocess_exec(
            "/usr/bin/sudo", "-n", "/usr/bin/python3", "-I", "/usr/local/lib/pi-controller/admin.py",
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
        )
        try:
            output, _ = await asyncio.wait_for(process.communicate(json.dumps(dict(operation=operation, **values)).encode()), 95)
        except (asyncio.TimeoutError, asyncio.CancelledError):
            process.kill()
            await process.wait()
            raise
        if process.returncode:
            # Privileged helper errors never echo nmcli arguments or secrets.
            raise ValueError("OS operation failed; check pi-controller-admin in the system journal")
        response = json.loads(output.decode())
        if not response.get("ok"):
            raise ValueError(response.get("error", "OS operation failed"))
        return response
