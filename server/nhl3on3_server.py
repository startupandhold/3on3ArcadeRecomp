#!/usr/bin/env python3
"""NHL 3 on 3 dedicated server: lobby (TCP), peer tunnel and lockstep relay
host (UDP). Standard library only; Python 3.9+.

    python nhl3on3_server.py [--public-ip A.B.C.D]

Environment (overrides defaults, used by the Docker image): PUBLIC_IP,
LOG_LEVEL, and on Fly.io FLY_APP_NAME (set by the platform).
"""

import argparse
import asyncio
import logging
import os
import signal
import socket

from lobby import Lobby
from tunnel import Tunnel

LOBBY_TCP_PORTS = (10901, 30380, 42127)
TUNNEL_UDP_PORT = 3670
# On SIGTERM/SIGINT relay matches get this long to finish (fly.toml's
# kill_timeout must be longer).
DRAIN_TIMEOUT = 240

log = logging.getLogger("server")


def detect_lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 53))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def public_ip(arg):
    """The IPv4 address players reach the server on; the lobby hands it out as
    the address of its own lobby service."""
    if arg or os.environ.get("PUBLIC_IP"):
        return arg or os.environ["PUBLIC_IP"]
    app = os.environ.get("FLY_APP_NAME")
    if app:
        try:
            return socket.getaddrinfo(f"{app}.fly.dev", None, socket.AF_INET)[0][4][0]
        except OSError:
            log.error("could not resolve %s.fly.dev; set PUBLIC_IP "
                      "(fly ips allocate-v4 first)", app)
    return detect_lan_ip()


def udp_bind_address(bind):
    # Fly.io only routes UDP to sockets bound to fly-global-services.
    if os.environ.get("FLY_APP_NAME"):
        try:
            return socket.gethostbyname("fly-global-services")
        except OSError:
            log.error("could not resolve fly-global-services; UDP will not work")
    return bind


async def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--public-ip", default="")
    ap.add_argument("--log-level", default=os.environ.get("LOG_LEVEL", "INFO"))
    args = ap.parse_args()
    logging.basicConfig(level=args.log_level.upper(),
                        format="%(asctime)s %(name)s: %(message)s", datefmt="%H:%M:%S")

    ip = public_ip(args.public_ip)
    # Fly's proxy names each lobby connection's real address (proxy_proto).
    lobby = Lobby(ip, trust_proxy=bool(os.environ.get("FLY_APP_NAME")))
    tunnel = Tunnel(lobby)
    loop = asyncio.get_running_loop()

    servers = []
    for port in LOBBY_TCP_PORTS:
        servers.append(await asyncio.start_server(lobby.handle, args.bind, port))
    udp_bind = udp_bind_address(args.bind)
    await loop.create_datagram_endpoint(lambda: tunnel, local_addr=(udp_bind, TUNNEL_UDP_PORT))
    log.info("public address %s; lobby tcp %s; tunnel udp %s:%d", ip,
             ",".join(map(str, LOBBY_TCP_PORTS)), udp_bind, TUNNEL_UDP_PORT)

    async def report():
        last = None
        while True:
            await asyncio.sleep(60)
            status = {**lobby.status(), **tunnel.status()}
            key = {k: v for k, v in status.items() if k != "datagrams_forwarded"}
            if key != last:
                log.info("status %s", status)
                last = key

    stop = asyncio.Event()
    for sig in (signal.SIGTERM, signal.SIGINT):
        try:
            loop.add_signal_handler(sig, stop.set)
        except (NotImplementedError, RuntimeError, AttributeError):
            pass  # Windows: Ctrl+C still raises KeyboardInterrupt
    tasks = [asyncio.ensure_future(t) for t in (tunnel.run(), lobby.run(), report())]
    stopping = asyncio.ensure_future(stop.wait())
    done, _ = await asyncio.wait(tasks + [stopping], return_when=asyncio.FIRST_COMPLETED)
    for task in done:
        if task is not stopping:
            task.result()  # a housekeeping loop failed: raise it
    # Relay matches can't survive a restart, so let them finish first.
    lobby.draining = True
    deadline = loop.time() + DRAIN_TIMEOUT
    log.info("shutting down: no new games; %d relay matches to finish (at most %ds)",
             len(tunnel.matches), DRAIN_TIMEOUT)
    while tunnel.matches and loop.time() < deadline:
        await asyncio.sleep(1)
    log.info("shutting down now (%d relay matches left)", len(tunnel.matches))
    for server in servers:
        server.close()
    for task in tasks:
        task.cancel()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
