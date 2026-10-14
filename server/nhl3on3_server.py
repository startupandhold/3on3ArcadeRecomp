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
import socket

from lobby import Lobby
from tunnel import Tunnel

LOBBY_TCP_PORTS = (10901, 30380, 42127)
TUNNEL_UDP_PORT = 3670

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
    lobby = Lobby(ip)
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
            status.pop("datagrams_forwarded")
            if status != last:
                log.info("status %s", status)
                last = status

    await asyncio.gather(tunnel.run(), report())


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
