#!/usr/bin/env python3
"""Relay server for the 802.15.4 <-> WiFi tunnel bridge.

Pairs DK clients by tunnel_id and forwards length-prefixed framed
messages between paired peers. Wire format is documented in README.md.
"""
from __future__ import annotations

import argparse
import asyncio
import logging
import struct
from dataclasses import dataclass

LOG = logging.getLogger("relay")

# Header format (little-endian, 12 bytes):
#   uint32 magic; uint8 version; uint8 msg_type; uint16 seq;
#   int8  rssi;   uint8 lqi;     uint8 len;      uint8 _pad;
HDR_FMT = "<IBBHbBBB"
HDR_LEN = struct.calcsize(HDR_FMT)
assert HDR_LEN == 12

MAGIC = 0x34353142          # b"B154" interpreted little-endian
VERSION = 1
MSG_HELLO = 1
MSG_FRAME = 2

HELLO_PAYLOAD_LEN = 16
MAX_FRAME_LEN = 127
MAX_CLIENTS_PER_TUNNEL = 2  # one-line change to enable N>2 broadcast topology


@dataclass
class Client:
    tunnel_id: str
    reader: asyncio.StreamReader
    writer: asyncio.StreamWriter
    peer: str

    async def send(self, hdr: bytes, payload: bytes) -> None:
        self.writer.write(hdr + payload)
        await self.writer.drain()


# Module-level state. asyncio.Lock() in 3.10+ binds lazily so creation
# outside a running loop is safe.
tunnels: dict[str, list[Client]] = {}
tunnels_lock = asyncio.Lock()


async def handle_client(reader: asyncio.StreamReader,
                        writer: asyncio.StreamWriter) -> None:
    addr = writer.get_extra_info("peername")
    peer = f"{addr[0]}:{addr[1]}" if addr else "?"
    client: Client | None = None

    try:
        # --- HELLO ---
        hdr = await reader.readexactly(HDR_LEN)
        magic, version, msg_type, _seq, _rssi, _lqi, length, _pad = struct.unpack(HDR_FMT, hdr)

        if (magic != MAGIC or version != VERSION
                or msg_type != MSG_HELLO or length != HELLO_PAYLOAD_LEN):
            LOG.warning("[%s] invalid HELLO (magic=%#x ver=%d type=%d len=%d) -- closing",
                        peer, magic, version, msg_type, length)
            return

        payload = await reader.readexactly(length)
        tunnel_id = payload.rstrip(b"\x00").decode("ascii", errors="replace")
        if not tunnel_id:
            LOG.warning("[%s] empty tunnel_id -- closing", peer)
            return

        client = Client(tunnel_id=tunnel_id, reader=reader, writer=writer, peer=peer)

        # --- Register / pair ---
        async with tunnels_lock:
            members = tunnels.setdefault(tunnel_id, [])
            if len(members) >= MAX_CLIENTS_PER_TUNNEL:
                LOG.warning("[%s] tunnel '%s' full (%d) -- rejecting",
                            peer, tunnel_id, len(members))
                client = None  # skip removal in finally
                return
            members.append(client)
            count = len(members)

        LOG.info("[%s] connected to tunnel '%s' (%d/%d)",
                 peer, tunnel_id, count, MAX_CLIENTS_PER_TUNNEL)
        if count == MAX_CLIENTS_PER_TUNNEL:
            LOG.info("tunnel '%s' paired", tunnel_id)

        # --- Forward loop ---
        while True:
            hdr = await reader.readexactly(HDR_LEN)
            magic, version, msg_type, _seq, _rssi, _lqi, length, _pad = struct.unpack(HDR_FMT, hdr)

            if magic != MAGIC or version != VERSION:
                LOG.warning("[%s] bad header on tunnel '%s' -- closing",
                            peer, tunnel_id)
                return
            if length > MAX_FRAME_LEN:
                LOG.warning("[%s] oversized payload len=%d on tunnel '%s' -- closing",
                            peer, length, tunnel_id)
                return

            body = await reader.readexactly(length) if length else b""

            if msg_type != MSG_FRAME:
                LOG.warning("[%s] unexpected msg_type=%d on tunnel '%s' -- dropping",
                            peer, msg_type, tunnel_id)
                continue

            # Snapshot peer list under the lock; forward outside it so a
            # slow peer can't block tunnel registration globally.
            async with tunnels_lock:
                others = [c for c in tunnels.get(tunnel_id, []) if c is not client]

            for other in others:
                try:
                    await other.send(hdr, body)
                except (ConnectionError, OSError) as e:
                    LOG.info("[%s -> %s] forward error: %s", peer, other.peer, e)
                    # Don't close `other` here; its own handler will clean up.

    except asyncio.IncompleteReadError:
        pass  # peer closed cleanly mid-message
    except Exception:
        LOG.exception("[%s] handler crashed", peer)
    finally:
        if client is not None:
            async with tunnels_lock:
                members = tunnels.get(client.tunnel_id, [])
                if client in members:
                    members.remove(client)
                if not members:
                    tunnels.pop(client.tunnel_id, None)
            LOG.info("[%s] disconnected from tunnel '%s'", peer, client.tunnel_id)
        try:
            writer.close()
            await writer.wait_closed()
        except Exception:
            pass


async def main() -> None:
    parser = argparse.ArgumentParser(
        description="802.15.4 <-> WiFi tunnel relay server")
    parser.add_argument("--host", default="0.0.0.0",
                        help="bind address (default 0.0.0.0)")
    parser.add_argument("--port", type=int, default=47000,
                        help="bind port (default 47000)")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="enable DEBUG logging")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )
    if(args.verbose):
        LOG.debug("Verbose logging enabled")

    server = await asyncio.start_server(handle_client, host=args.host, port=args.port)
    addrs = ", ".join(str(s.getsockname()) for s in server.sockets)
    LOG.info("relay listening on %s", addrs)

    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
