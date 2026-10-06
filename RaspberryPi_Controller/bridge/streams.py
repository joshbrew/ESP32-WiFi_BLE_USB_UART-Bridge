"""Bounded newline streams shared by UART, USB CDC and BLE RX."""
import asyncio
import json

from .core import AdmissionError


class LineInput:
    def __init__(self, controller, source, reply):
        self.controller, self.source, self.reply = controller, source, reply
        self.buffer = bytearray()
        self.discarding = False

    def feed(self, data):
        for byte in data:
            if byte == 10:
                if not self.discarding:
                    try:
                        line = self.buffer.decode("utf-8").strip()
                        if line == "@STATE":
                            self.reply("@STATE " + json.dumps(self.controller.state(include_geo_points=False), separators=(",", ":")) + "\n")
                        elif line.startswith("@GEO:"):
                            offset = line[5:]
                            if not offset.isdecimal() or len(offset) > 3: raise ValueError("use @GEO:offset (0-500)")
                            self.reply("@GEO " + json.dumps(self.controller.geo.page(int(offset)), separators=(",", ":")) + "\n")
                        elif line:
                            self.controller.submit(line, self.source)
                    except (UnicodeError, AdmissionError, ValueError) as error:
                        self.reply("@ERROR " + str(error) + "\n")
                self.buffer.clear()
                self.discarding = False
            elif not self.discarding:
                self.buffer.append(byte)
                if len(self.buffer) > 256:
                    self.buffer.clear()
                    self.discarding = True
                    self.reply("@ERROR command exceeds 256 bytes; discarding until newline\n")


class BufferedOutput:
    def __init__(self):
        self.queue = asyncio.Queue(maxsize=32)
        self.ready = False
        self.dropped = 0

    def send(self, text):
        if not self.ready:
            return False
        try:
            self.queue.put_nowait(text.encode("utf-8"))
            return True
        except asyncio.QueueFull:
            self.dropped += 1
            return False

    def clear(self):
        while not self.queue.empty():
            self.queue.get_nowait()
            self.queue.task_done()


class SerialTransport(BufferedOutput):
    def __init__(self, controller, source, device, baud):
        super().__init__()
        self.controller, self.source = controller, source
        self.device, self.baud = device, baud
        self.port = None
        self.task = None
        self.input = LineInput(controller, source, self.send)

    async def start(self):
        self.controller.outputs[self.source] = self
        self.task = asyncio.create_task(self.run())

    async def run(self):
        import serial
        while True:
            try:
                self.port = await asyncio.to_thread(serial.Serial, self.device, self.baud, timeout=0, write_timeout=0.2)
                self.ready = True
                self.controller.publish(f"{self.source} opened {self.device}")
                while True:
                    data = self.port.read(256)
                    if data:
                        self.input.feed(data)
                    if not self.queue.empty():
                        frame = self.queue.get_nowait()
                        try:
                            await asyncio.to_thread(self.port.write, frame)
                        finally:
                            self.queue.task_done()
                    await asyncio.sleep(0.01)
            except (serial.SerialException, OSError) as error:
                self.ready = False
                self.controller.publish(f"{self.source} unavailable: {error}", level="warning")
            finally:
                self.ready = False
                self.clear()
                self.input.buffer.clear()
                self.input.discarding = False
                if self.port:
                    self.port.close()
                    self.port = None
            await asyncio.sleep(3)

    async def close(self):
        if self.task:
            self.task.cancel()
            await asyncio.gather(self.task, return_exceptions=True)
        self.ready = False
