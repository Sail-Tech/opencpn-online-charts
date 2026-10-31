#!/usr/bin/env python3
"""Receive NMEA/JSON over UDP or TCP and forward it to Chrome over WebSocket.

N2K must first be converted by a gateway to NMEA 0183, JSON or Signal K.
Do not expose the WebSocket to a public network: it listens only on 127.0.0.1 by default.
"""

import argparse
import asyncio
import logging
from typing import Set

import websockets

CLIENTS: Set = set()
QUEUE: asyncio.Queue = asyncio.Queue()


def enqueue_bytes(data: bytes) -> None:
    text = data.decode("utf-8", errors="replace")
    if text.strip():
        QUEUE.put_nowait(text)


class NmeaUDP(asyncio.DatagramProtocol):
    def datagram_received(self, data: bytes, addr) -> None:
        logging.debug("UDP %s: %d bytes", addr, len(data))
        enqueue_bytes(data)


async def udp_source(host: str, port: int) -> None:
    loop = asyncio.get_running_loop()
    await loop.create_datagram_endpoint(NmeaUDP, local_addr=(host, port))
    logging.info("Listening for UDP on %s:%d", host, port)
    await asyncio.Future()


async def tcp_client_source(host: str, port: int) -> None:
    while True:
        try:
            reader, writer = await asyncio.open_connection(host, port)
            logging.info("Connected to TCP server %s:%d", host, port)
            while True:
                data = await reader.read(4096)
                if not data:
                    break
                enqueue_bytes(data)
            writer.close()
            await writer.wait_closed()
        except (OSError, asyncio.IncompleteReadError) as exc:
            logging.warning("TCP unavailable: %s; retrying in 3 s", exc)
        await asyncio.sleep(3)


async def handle_tcp_device(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    peer = writer.get_extra_info("peername")
    logging.info("TCP device connected: %s", peer)
    try:
        while True:
            data = await reader.read(4096)
            if not data:
                break
            enqueue_bytes(data)
    finally:
        writer.close()
        await writer.wait_closed()
        logging.info("TCP device disconnected: %s", peer)


async def tcp_server_source(host: str, port: int) -> None:
    server = await asyncio.start_server(handle_tcp_device, host, port)
    logging.info("Accepting TCP devices on %s:%d", host, port)
    async with server:
        await server.serve_forever()


async def websocket_handler(websocket) -> None:
    CLIENTS.add(websocket)
    logging.info("Chrome connected (%d client(s))", len(CLIENTS))
    try:
        await websocket.wait_closed()
    finally:
        CLIENTS.discard(websocket)
        logging.info("Chrome disconnected (%d client(s))", len(CLIENTS))


async def broadcaster() -> None:
    while True:
        text = await QUEUE.get()
        if not CLIENTS:
            continue
        targets = list(CLIENTS)
        results = await asyncio.gather(
            *(client.send(text) for client in targets), return_exceptions=True
        )
        for client, result in zip(targets, results):
            if isinstance(result, Exception):
                CLIENTS.discard(client)


async def run(args) -> None:
    if args.mode == "udp":
        source = udp_source(args.host, args.port)
    elif args.mode == "tcp":
        source = tcp_client_source(args.host, args.port)
    else:
        source = tcp_server_source(args.host, args.port)

    async with websockets.serve(websocket_handler, args.ws_host, args.ws_port):
        logging.info("WebSocket for Chrome: ws://%s:%d", args.ws_host, args.ws_port)
        await asyncio.gather(source, broadcaster())


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("udp", "tcp", "tcp-server"))
    parser.add_argument("--host", required=True, help="Listening IP or GPS server IP")
    parser.add_argument("--port", type=int, default=10110, help="NMEA port (default: 10110)")
    parser.add_argument("--ws-host", default="127.0.0.1", help="WebSocket interface (default: localhost)")
    parser.add_argument("--ws-port", type=int, default=8765, help="WebSocket port (default: 8765)")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
