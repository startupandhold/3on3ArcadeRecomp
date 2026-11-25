# NHL 3 on 3 dedicated server

One small Python process (standard library only) that does everything online
play needs:

- **Lobby** (TCP 10901, 30380, 42127): sign-in, quick match, created sessions
  and game membership, replacing EA's servers.
- **Tunnel** (UDP 3670): each game registers and gets a virtual address
  (`10.66.x.y`). All traffic between players goes through the server, so
  nobody has to forward ports, and players behind any home router can play.
- **Lockstep relay host**: in team play (3–8 consoles) the server combines
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
  The host starts the match once everyone is ready. A game can have up to 8
  consoles and 8 players (guests count; 3 skaters and a goalie per side).
  After it starts, new searchers wait for, or open, the next game.
- **Create Session** (the online menu's first item, in place of Play a Friend)
  opens a game with you as host, waiting for players. It shows up in
  **Online Team Play → Custom Match** for everyone with the same roster pack,
  and Online Team Play quick match searches join it too, unless it was
  created as a private session. After that it plays like any team play game:
  everyone lands on Select Sides and the host starts the match. A ranked
  session takes one opponent.

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
   `online_server` in `src/nhl3on3_net.cpp` and in both launchers
   (`launcher/nhl3on3_launcher.ps1` on Windows, `launcher/nhl3on3_launcher.cpp`
   on Linux).

## Updating and operating

- Redeploy after editing anything in `server/`: `fly deploy --ha=false`.
  A deploy restarts the server, and all its state is in memory:
  - **Ranked 1 vs 1** games only use the tunnel, which re-registers on its
    own (with the same virtual address) once the new server is up, so they
    usually survive if the gap is short.
  - **Team play** relay matches are hosted by the server and end when it
    restarts. So on SIGTERM (`kill_signal` in `fly.toml`) the server
    *drains*: it stops taking searches, new sessions, joins and new relay
    matches, keeps relaying for up to 240 s until the running matches end,
    then exits (`kill_timeout = 250`). During that time, and until the new
    machine is up, nobody can start a game. Deploy when nobody is playing,
    or expect matches longer than 4 minutes to be cut off.
  - Lobby searches in progress are lost.
- Live logs: `fly logs`. A status line (players, games, tunnel peers, relay
  matches, dead slots, desyncs, rejected registrations, datagrams forwarded)
  is logged whenever it changes (ignoring the forwarded count), at most once
  a minute.
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
- Protocol v2 (`PROTOCOL_V2.md`) adds optional trailing fields, so v1 games
  still work: a protocol version, a per-address token, keepalive acks with a
  reported ping, and state hashes. Quick match only pairs games with the same
  roster pack and protocol version, and holds back two players whose pings
  add up to more than 250 ms until one has searched for 20 s.
- Disconnects in team play: a console that sends nothing for 8 s (or never
  sends in the first 30 s) is dropped from the relay match. The others keep
  playing with its players getting no input, and the log says
  `match X: slot N (address) dropped after Ns`. A match no longer depends on
  the lobby once it runs, so a dropped lobby connection doesn't end it. A
  state hash mismatch between consoles is logged once per match (`desync at
  frame N`).
- A lobby connection whose tunnel address times out (120 s of silence) is
  closed. A game may only claim a tunnel address registered from the same
  public IP as its lobby connection (on Fly.io the `proxy_proto` handler in
  `fly.toml` passes that IP to the server).
- There are no accounts: anyone with the address can play, like the original
  quick match. Limits: 600 packets/s per tunnel address; new tunnel
  registrations 50/s in total and 5/s per IP; 16 lobby connections per IP;
  5 chat lines per 2 s per player; a lobby connection that stops reading
  (1 MB unsent) is closed. Relay inputs over 252 bytes and messages over
  1200 bytes are dropped.
