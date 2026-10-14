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

### Matchmaking

There is nothing to configure:

- **Ranked** quick match pairs two players: a 1 vs 1.
- **Online Team Play** (the renamed unranked quick match) opens a game as soon
  as two players search, and both land on Select Sides. Anyone else who
  searches while that game is open joins it and appears on Select Sides too.
  The host starts the match once everyone is ready. A game can have up to 6
  consoles and 6 players (guests count, 3 per side). After it starts, new
  searchers wait for, or open, the next game.

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

   From your PC, run the self-test against the live server. It signs in four
   fake players, tunnels a packet, makes a ranked 1 vs 1, and builds a team
   play game that a third player joins. Then it runs relay frames and checks
   that a started game takes no one new:

   ```powershell
   python selftest.py nhl3on3-yourname.fly.dev
   ```

   It should end with `PASS`. Run it while nobody is playing, because its fake
   players would land in real players' open games.

6. **Point the game at it.** The game uses `nhl3on3-turtle.fly.dev` unless
   told otherwise. For your own server, set **Server** in the launcher's Online
   tab to `nhl3on3-yourname.fly.dev` (or the IPv4 from `fly ips list`), keep
   **Connect through server** checked, and enable **Online play**. To make
   it the default for everyone you hand the game to, change the default of
   `online_server` in both `src/nhl3on3_net.cpp` and the launcher
   (`launcher/nhl3on3_launcher.ps1`).

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
python nhl3on3_server.py --public-ip <address players use>
```

With Docker:

```
docker build -t nhl3on3-server server
docker run -d --restart unless-stopped -p 10901:10901 -p 30380:30380 -p 42127:42127 \
  -p 3670:3670/udp -e PUBLIC_IP=<address> nhl3on3-server
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
