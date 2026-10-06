import argparse
import asyncio
import logging

from aiohttp import web

from .config import load_config
from .core import Controller
from .server import create_app
from .transport_manager import TransportManager


def main():
    parser = argparse.ArgumentParser(description="Raspberry Pi transport and dispenser controller")
    parser.add_argument("--config", help="JSON configuration file")
    parser.add_argument("--simulate", action="store_true", help="force simulation (never drive GPIO)")
    arguments = parser.parse_args()
    config = load_config(arguments.config)
    if arguments.simulate:
        config["simulate"] = True
    logging.basicConfig(level=logging.INFO)
    controller = Controller(config)
    app = create_app(controller)

    async def transports(app):
        manager = TransportManager(controller)
        try:
            try:
                await manager.start()
                if config["hardware_profile"] == "dispenser":
                    await controller.geo.open_udp()
                if controller.radio.configured and (config["enable_os_control"] or config["simulate"]):
                    await controller.radio.apply()
            except Exception as error:
                controller.ble_error = str(error)
                controller.publish("transport/radio startup: " + str(error), level="error")
            await controller.features.start()
            yield
        finally:
            controller.stop_all("transport shutdown", abort_test=False)
            await manager.close()

    app.cleanup_ctx.append(transports)
    print(f"Pi controller {'SIMULATION' if config['simulate'] else 'GPIO LIVE'} at http://{config['host']}:{config['port']}")
    try:
        web.run_app(app, host=config["host"], port=config["port"], access_log=None, shutdown_timeout=3)
    finally:
        controller.features.close_hardware()
        controller.dispenser.close()


if __name__ == "__main__":
    main()
