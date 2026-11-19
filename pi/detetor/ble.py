"""Servidor Bluetooth LE para a app (Nordic UART Service, o padrão para "porta série" BLE).

  RX (0002): a app escreve comandos JSON terminados em '\n'
  TX (0003): o hub notifica mensagens JSON terminadas em '\n', partidas em pedaços de 180 B
             (a app pede MTU 247; 180 B deixa folga)
Também dá para espreitar com o nRF Connect ou o "Serial Bluetooth Terminal".
"""
import asyncio
import logging

log = logging.getLogger("ble")

NUS = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
CHUNK = 180


class BleLink:
    def __init__(self, name: str, on_command):
        self.name, self.on_command = name, on_command
        self.queue: asyncio.Queue[bytes] = asyncio.Queue(maxsize=2000)
        self.server = None
        self.rx_buf = b""

    def send(self, line: str):
        data = line.encode() + b"\n"
        try:
            self.queue.put_nowait(data)
        except asyncio.QueueFull:
            pass  # app longe: o histórico fica no SQLite e é pedido depois

    async def run(self):
        from bless import (BlessServer, GATTAttributePermissions,  # noqa: import tardio
                           GATTCharacteristicProperties)
        loop = asyncio.get_running_loop()
        self.server = BlessServer(name=self.name, loop=loop)
        self.server.read_request_func = lambda ch, **kw: ch.value
        self.server.write_request_func = self._on_write
        await self.server.add_new_service(NUS)
        await self.server.add_new_characteristic(
            NUS, RX,
            GATTCharacteristicProperties.write | GATTCharacteristicProperties.write_without_response,
            None, GATTAttributePermissions.writeable)
        await self.server.add_new_characteristic(
            NUS, TX, GATTCharacteristicProperties.notify | GATTCharacteristicProperties.read,
            None, GATTAttributePermissions.readable)
        await self.server.start()
        log.info("Bluetooth a anunciar como '%s'", self.name)
        while True:
            data = await self.queue.get()
            for i in range(0, len(data), CHUNK):
                self.server.get_characteristic(TX).value = bytearray(data[i:i + CHUNK])
                self.server.update_value(NUS, TX)
                await asyncio.sleep(0.012)

    def _on_write(self, characteristic, value, **kwargs):
        self.rx_buf += bytes(value)
        while b"\n" in self.rx_buf:
            line, self.rx_buf = self.rx_buf.split(b"\n", 1)
            if line.strip():
                self.on_command(line)
        if len(self.rx_buf) > 4096:
            self.rx_buf = b""
