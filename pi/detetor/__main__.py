"""Arranque do hub: python3 -m detetor"""
import asyncio
import json
import logging

from . import config
from .ble import BleLink
from .hub import Hub
from .rf import RfScanner
from .serial_io import SerialWatcher
from .store import Store


async def main():
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s: %(message)s")
    cfg = config.load()
    store = Store(cfg.db_path)
    loop = asyncio.get_running_loop()

    link: BleLink | None = None

    def send(msg: dict):
        if link:
            link.send(json.dumps(msg, ensure_ascii=False, separators=(",", ":")))

    hub = Hub(cfg, store, send)
    store.add("system", "hub", {"msg": "arranque", "time_ok": hub.time_ok}, time_ok=hub.time_ok)

    def on_command(raw: bytes):
        for reply in hub.on_command(raw):
            send(reply)

    link = BleLink(cfg.ble_name, lambda raw: loop.call_soon_threadsafe(on_command, raw))
    SerialWatcher(lambda line, port: loop.call_soon_threadsafe(hub.on_esp_line, line, port)).start()

    tasks = [asyncio.create_task(link.run())]
    if cfg.rf_enabled:
        tasks.append(asyncio.create_task(RfScanner(cfg, hub.on_rf, hub.on_rf_status).run()))

    async def periodic_status():
        while True:
            await asyncio.sleep(30)
            send(hub.status())
    tasks.append(asyncio.create_task(periodic_status()))
    await asyncio.gather(*tasks)


if __name__ == "__main__":
    asyncio.run(main())
