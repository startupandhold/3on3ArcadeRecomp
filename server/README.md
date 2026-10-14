# NHL 3 on 3 dedicated server

One small Python process (standard library only) that does everything online
play needs:

- **Lobby** (TCP 10901, 30380, 42127): sign-in, quick match and game
  membership, replacing EA's servers.
- **Tunnel** (UDP 3670): each game registers and gets a virtual address
  (`10.66.x.y`). All traffic between players goes through the server, so
  nobody has to forward ports, and players behind any home router can play.
- **Lockstep relay host**: in team play (3–6 consoles) the server combines
  every console's input into frames. No player acts as host, so none has a
  latency advantage.

The game connects through the server by default (the launcher's **Connect through
server** option, `online_tunnel = true` in `nhl3on3.toml`). If the server does not
answer the tunnel, the game falls back to direct connections.

## Hosting on Fly.io

Costs roughly $4/month: a `shared-cpu-1x` machine (about $2) plus a dedicated
IPv4 address ($2). Fly only routes UDP to apps with a dedicated IPv4, so the
address is required.

1. **Install flyctl** (PowerShell):

   ```powershell
   iwr https://fly.io/install.ps1 -useb | iex
   ```

   Then sign up or log in:

   ```powershell
   fly auth signup    # or: fly auth login
   ```

2. **Create the app.** App names are global, so pick a unique one:

   ```powershell
   cd C:\dev\nhl-3-on-3\server
   fly apps create nhl3on3-yourname
   ```

   Edit `fly.toml` and set `app = "nhl3on3-yourname"`. Set `primary_region` to
   the region nearest your players, because every packet goes through it.
   `fly platform regions` lists them, e.g. `ord` Chicago, `iad` Virginia,
   `sjc` San Jose, `yyz` Toronto, `lhr` London, `fra` Frankfurt.

3. **Allocate the dedicated IPv4** (needed for UDP):

   ```powershell
   fly ips allocate-v4
   ```

   Answer yes to the prompt about the monthly charge.

4. **Deploy one machine.** Lobby state lives in memory, so there must be
   exactly one:

   ```powershell
   fly deploy --ha=false
   fly scale count 1
   ```

5. **Check it.**

   ```powershell
   fly status
   fly ips list        # shows your dedicated v4 address
   fly logs            # should show "public address <your v4>; lobby tcp ...; tunnel udp ..."
   ```

   From your PC, run the self-test against the live server. It signs in two
   fake players, tunnels a packet between them and matches them:

   ```powershell
   python selftest.py nhl3on3-yourname.fly.dev
   ```

   It should end with `PASS`. Run it again with a 3–6 argument to also test
   the relay once you set `MATCH_SIZE` to that value (see below).

6. **Point the game at it.** In the launcher's Online tab, set **Server** to
   `nhl3on3-yourname.fly.dev` (or the IPv4 from `fly ips list`), keep
   **Connect through server** checked, and enable **Online play**. To make
   it the default for everyone you hand the game to, change the `Default` of
   `online_server` in `launcher/nhl3on3_launcher.ps1` to that name.

## Team play

Quick match pairs `MATCH_SIZE` consoles for unranked games (ranked is always
1 vs 1). For team play, set it in `fly.toml` and redeploy:

```toml
[env]
  MATCH_SIZE = "4"
```

```powershell
fly deploy --ha=false
```

Every player needs the same **Team play** console count in the launcher.

## Updating and operating

- Redeploy after editing anything in `server/`: `fly deploy --ha=false`.
  A deploy restarts the server. Games in progress keep running, since their
  traffic only needs the tunnel, which re-registers on its own within a few
  seconds. Lobby searches in progress are lost.
- Live logs: `fly logs`. A status line (players, games, tunnel peers, relay
  matches) is logged whenever it changes, at most once a minute.
- More detail: set `LOG_LEVEL = "DEBUG"` in `fly.toml` to log every lobby request.
- Stop it: `fly scale count 0`. Remove it completely: `fly apps destroy nhl3on3-yourname`
  (this also releases the IPv4).

## Running it elsewhere

Any machine with Python 3.9+ works, for example a home PC or a VPS. On a home PC
you must forward TCP 10901, 30380, 42127 and UDP 3670 to it:

```
python nhl3on3_server.py --public-ip <address players use> [--match-size 4]
```

With Docker:

```
docker build -t nhl3on3-server server
docker run -d --restart unless-stopped -p 10901:10901 -p 30380:30380 -p 42127:42127 \
  -p 3670:3670/udp -e PUBLIC_IP=<address> -e MATCH_SIZE=2 nhl3on3-server
```

`--public-ip` / `PUBLIC_IP` is the address the lobby tells games to reconnect to;
on Fly.io it is found automatically from `<app>.fly.dev`.

## Protocol notes

- Lobby: `lobby.py`. It follows the ea-nation-server emulator, and the
  frames are documented at the top of the file.
- Tunnel and relay: `tunnel.py`. The client side is in
  `src/nhl3on3_net.cpp` (tunnel) and `src/nhl3on3_lockstep_relay.cpp` (relay,
  where every console is a client when tunneled).
- There are no accounts: anyone with the address can play, like the original
  quick match. Each address is rate-limited to 600 packets/s.
