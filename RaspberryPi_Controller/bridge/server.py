from pathlib import Path
import hmac
import asyncio

from aiohttp import web

from .core import AdmissionError
from .release import MAX_BYTES


WEB = Path(__file__).resolve().parent.parent / "web"


def create_app(controller, manage_lifecycle=True):
    @web.middleware
    async def policy(request, handler):
        # Same-origin hosted UI. No wildcard CORS: other websites cannot submit
        # hardware commands through a browser on the same local network.
        if request.method == "POST":
            origin = request.headers.get("Origin")
            if origin and origin != f"{request.scheme}://{request.host}":
                return web.json_response(dict(ok=False, error="cross-origin commands are disabled"), status=403)
        if request.path.startswith("/api/") and request.path != "/api/ping" and not controller.radio.wifi_enabled:
            return web.json_response(dict(ok=False, error="HTTP commands disabled by active radio profile"), status=503)
        try:
            response = await handler(request)
        except (ValueError, UnicodeError) as error:
            response = web.json_response(dict(ok=False, error=str(error)), status=400)
        except TimeoutError:
            response = web.json_response(dict(ok=False, error="request timed out"), status=408)
        response.headers["Cache-Control"] = "no-store"
        response.headers["X-Content-Type-Options"] = "nosniff"
        return response

    app = web.Application(client_max_size=MAX_BYTES + 65536, middlewares=[policy],
                          handler_args={"max_line_size": 4096, "max_field_size": 4096})

    async def asset(request):
        return web.FileResponse(WEB / {"/": "index.html", "/app.js": "app.js", "/app.css": "app.css"}[request.path])

    async def ping(request):
        controller.http_seen = controller.clock()
        return web.json_response(dict(ok=True, version=controller.state()["version"], releaseId=controller.updates.release_id,
                                      ready=all(not task.done() for task in controller.tasks)))

    async def state(request):
        controller.http_seen = controller.clock()
        return web.json_response(controller.state())

    async def events(request):
        controller.http_seen = controller.clock()
        since = max(0, int(request.query.get("since", "0")))
        limit = max(1, min(32, int(request.query.get("limit", "8"))))
        return web.json_response(controller.event_page(since, limit))

    async def command(request):
        controller.http_seen = controller.clock()
        if request.content_type not in {"application/octet-stream", "text/plain"}:
            return web.json_response(dict(ok=False, accepted=False, error="send text/plain or application/octet-stream"), status=415)
        try:
            if request.content_length and request.content_length > 2048:
                raise web.HTTPRequestEntityTooLarge(max_size=2048, actual_size=request.content_length)
            async with asyncio.timeout(10):
                data = await request.content.read(2049)
                while not request.content.at_eof() and len(data) <= 2048:
                    data += await request.content.read(2049 - len(data))
            body = data.decode("utf-8")
            if len(body.encode()) > 2048:
                raise web.HTTPRequestEntityTooLarge(max_size=2048, actual_size=len(body.encode()))
            result = controller.submit(body, "WiFi", request.headers.get("X-Request-ID", ""))
            return web.json_response(result, status=202)
        except AdmissionError as error:
            return web.json_response(dict(ok=False, accepted=False, retryable=error.status == 503, error=str(error)), status=error.status)

    async def ota(request):
        token = controller.config["update_token"]
        supplied = request.headers.get("X-Update-Token", "")
        if not token or not hmac.compare_digest(token.encode(), supplied.encode()):
            return web.json_response(dict(ok=False, error="valid update token required"), status=403)
        if request.content_type not in {"application/zip", "application/octet-stream", "multipart/form-data"}:
            return web.json_response(dict(ok=False, error="upload a Pi .zip update bundle"), status=415)
        async def chunks():
            if request.content_type == "multipart/form-data":
                reader = await request.multipart()
                field = await reader.next()
                if field is None or field.name != "firmware" or not (field.filename or "").lower().endswith(".zip"):
                    raise ValueError("multipart field firmware must contain a Pi .zip bundle")
                while True:
                    data = await field.read_chunk(65536)
                    if not data: break
                    yield data
                if await reader.next() is not None:
                    raise ValueError("only one update file is allowed")
            else:
                async for data in request.content.iter_chunked(65536):
                    yield data
        try:
            async with asyncio.timeout(60):
                result = await controller.updates.upload(chunks(), request.headers.get("X-Update-SHA256", ""))
            return web.json_response(result, status=202)
        except (ValueError, OSError, TimeoutError) as error:
            return web.json_response(dict(ok=False, error=str(error)), status=400)

    async def position(request):
        controller.http_seen = controller.clock()
        if controller.geo.plan["source"] != "API":
            return web.json_response(dict(ok=False, error="select GeoSource:API first"), status=400)
        try:
            if request.content_type not in {"text/plain", "application/octet-stream"}:
                raise ValueError("position requires text/plain or application/octet-stream")
            if request.content_length and request.content_length > 128:
                raise ValueError("position body exceeds 128 bytes")
            async with asyncio.timeout(10):
                data = await request.content.read(129)
                while not request.content.at_eof() and len(data) <= 128:
                    data += await request.content.read(129 - len(data))
            if len(data) > 128:
                raise ValueError("position body exceeds 128 bytes")
            controller.geo.submit_position(data.decode("utf-8"))
        except (ValueError, UnicodeError, TimeoutError):
            # Malformed/stalled custom fixes revoke the previous position too.
            # The watchdog observes this even when the command queue is full.
            controller.geo.mailbox = (None, controller.clock())
            controller.geo.guard_invalid = True
            raise
        return web.json_response(dict(ok=True, accepted=True))

    async def update_status(request):
        return web.json_response(dict(ok=True, **controller.updates.state()))

    async def captive(request):
        raise web.HTTPFound("/")

    for path in ("/", "/app.js", "/app.css"):
        app.router.add_get(path, asset)
    app.router.add_get("/api/ping", ping)
    app.router.add_get("/api/state", state)
    app.router.add_get("/api/events", events)
    app.router.add_post("/api/command", command)
    app.router.add_post("/api/position", position)
    app.router.add_post("/api/ota", ota)
    app.router.add_get("/api/update/status", update_status)
    for path in ("/generate_204", "/gen_204", "/hotspot-detect.html", "/connecttest.txt", "/ncsi.txt", "/canonical.html", "/success.txt"):
        app.router.add_get(path, captive)
    if manage_lifecycle:
        async def lifecycle(app):
            await controller.start()
            try:
                yield
            finally:
                await controller.close()
        app.cleanup_ctx.append(lifecycle)
    return app
