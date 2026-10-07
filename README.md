# proton-best-server

Two small Windows tools for Proton VPN users. No login, no API key, no
third-party packages. Python 3.8+.

| Script | What it does |
|---|---|
| `proton_best_server.py` | Ranks the servers in a country by ping latency, from the Windows client's cached server list |
| `vpn_check.py` | Checks the connection you are on: exit location, IPv6 and DNS leaks, latency, download and upload |

---

# proton_best_server.py

Find the lowest-latency Proton VPN server for a country, straight from the server
list the Proton VPN Windows client already caches on disk.

## How it works

1. Reads `%LOCALAPPDATA%\Proton\Proton VPN\Storage\Servers.*.bin`, the protobuf
   server list the Windows client downloads on each refresh. A tiny wire-format
   reader is built in, so no `.proto` file or `protobuf` package is needed.
2. Filters to the exit country you ask for (default `JP`), dropping Free-tier
   servers and Secure Core routes unless you opt in.
3. Groups logical servers by physical entry IP so each node is pinged once,
   then pings them all in parallel.
4. Prints the nodes ranked by average round-trip time, alongside the load
   Proton reported at the client's last refresh, and recommends the best one.

## Usage

Run it with the VPN **disconnected**. While the tunnel is up, every ping leaves
through the current exit server, so the numbers describe that server's path, not
yours. The script checks for an active Proton/WireGuard adapter and refuses to
run unless you pass `--force`.

```
python proton_best_server.py                 # Japan, Plus servers, no Secure Core
python proton_best_server.py --country SG    # another exit country
python proton_best_server.py --include-free --include-secure-core
python proton_best_server.py --pings 10 --top 15
python proton_best_server.py --force         # measure even while connected
python proton_best_server.py --no-pause      # skip "Press Enter" (for scripting)
```

All options:

| Flag | Default | Meaning |
|---|---|---|
| `--country` | `JP` | Two-letter exit country code |
| `--include-free` | off | Also test Free-tier servers |
| `--include-secure-core` | off | Also test Secure Core routes |
| `--pings` | 5 | Pings per node |
| `--timeout` | 1500 | Per-ping timeout in ms |
| `--workers` | 8 | Parallel pings |
| `--top` | 20 | Rows to print |
| `--max-load` | 80 | Ignore nodes above this load % for the recommendation |
| `--force` | off | Run even if a VPN tunnel is up |
| `--no-pause` | off | Do not wait for Enter before exiting |

## Example output

```
Server list: C:\Users\you\AppData\Local\Proton\Proton VPN\Storage\Servers.<hash>.bin
  cached 2026-10-07 11:08 (load values are from that refresh)
Testing 9 unique JP entry nodes with 5 pings each...

  avg   min   max  loss  load tier  city     ip               servers
---------------------------------------------------------------------
   6ms    2ms   12ms    0%   22% Plus  Tokyo    138.199.21.213   JP#268  [P2P,Stream]
   6ms    2ms   12ms    0%   18% Plus  Tokyo    212.102.51.121   JP#364, JP#368, JP#383  [P2P,Stream]
   7ms    2ms   14ms    0%   13% Plus  Tokyo    138.199.21.215   JP#331  [P2P,Stream]
  15ms   11ms   22ms    0%   75% Plus  Osaka    45.14.71.6       JP#200, JP#201, JP#204, JP#205 (+8)  [P2P,Stream]

Recommended: JP#268 (Tokyo, 138.199.21.213) - 6 ms avg, 22% load
  In the Proton VPN app, search for that server name and connect to it.
```

When several nodes ping the same, pick the one with the lowest load. Load
matters more for throughput than a millisecond of latency.

## Notes

- The Proton VPN Windows app has no command-line interface for switching
  servers, so the script ranks servers but cannot connect to them. Search for
  the recommended name in the app.
- Field numbers in the protobuf cache were observed on Windows client 5.1.x. A
  future client version could change the layout; if the script reports no
  servers, that is the first thing to check.
- Load values are only as fresh as the client's last server-list refresh. Open
  the app once before running if the cache is stale.
- Windows only, because it shells out to `ping.exe` and PowerShell's
  `Get-NetAdapter`. Porting to Linux or macOS would mean swapping those two
  calls and the cache path.

---

# vpn_check.py

Once you are connected, this tells you whether the tunnel is doing its job and
how fast it is. It works just as well disconnected, which is how you get a
baseline to compare against.

## What it reports

1. **VPN adapters.** Every Proton, WireGuard, TAP, Wintun or OpenVPN adapter
   and whether it is up.
2. **Public exit.** Your IPv4 and IPv6 address, each with city, country and
   network owner from ipinfo.io. Warns if IPv6 exits in a different country
   from IPv4, which is the classic IPv6 leak.
3. **DNS servers** per interface. Warns if no resolver is bound to the VPN
   adapter, which means lookups may bypass the tunnel.
4. **Latency.** Ten pings each to Cloudflare DNS, Google DNS, google.com,
   youtube.com and steamcommunity.com.
5. **Throughput.** Single-stream and 4-stream download from the Vultr test
   file nearest your exit country, and a 25 MB upload to Cloudflare.
   Cloudflare's own download endpoint returns 403 to VPN exits, which is why
   Vultr is used for the download side.

## Usage

```
python vpn_check.py
python vpn_check.py --json before.json         # save this run
python vpn_check.py --compare before.json      # show deltas against a saved run
python vpn_check.py --no-speed                 # skip the throughput tests
python vpn_check.py --download-url https://host/100MB.bin
```

The typical workflow for measuring what the VPN costs you:

```
python vpn_check.py --json off.json            # with the VPN disconnected
python vpn_check.py --compare off.json         # after connecting
```

| Flag | Default | Meaning |
|---|---|---|
| `--json FILE` | off | Save results to a JSON file |
| `--compare FILE` | off | Print before/now/delta against a saved run |
| `--no-speed` | off | Skip download and upload tests |
| `--download-url` | Vultr by country | Override the download test file |
| `--streams` | 4 | Parallel download streams |
| `--pings` | 10 | Pings per host |
| `--no-pause` | off | Do not wait for Enter before exiting |

## Example output

```
=== VPN adapters ===
  ProtonVPN                Up           WireGuard Tunnel
  tunnel: ProtonVPN

=== Public exit ===
  IPv4  159.26.119.90                            Tokyo, JP  (AS208172 Proton AG)
  IPv6  2a02:6ea0:d33b:6275::38                  Tokyo, JP  (AS60068 Datacamp Limited)

=== DNS servers ===
  ProtonVPN                10.2.0.1
  Ethernet                 1.1.1.1, 8.8.8.8

=== Latency (10 pings each) ===
  1.1.1.1                avg    3 ms   min    3   max    7   loss 0%
  www.youtube.com        avg    3 ms   min    3   max    7   loss 0%
  steamcommunity.com     avg   37 ms   min   36   max   41   loss 0%

=== Download (hnd-jp-ping.vultr.com) ===
  single stream      233 Mbps  (3.6 s)
  4 streams          155 Mbps  (21.6 s)

=== Upload (speed.cloudflare.com) ===
  25 MB               461 Mbps  (0.4 s)
```

## Notes

- Proton rents capacity from hosting providers, so the IPv6 exit will often
  show a different network owner (Datacamp, M247, and so on) from the IPv4
  one. That is fine as long as both land in the same city. The script only
  warns when the countries differ.
- Public test files cap per-connection speed and some limit concurrent
  connections from one IP, so the 4-stream figure can come in lower than the
  single-stream one. Treat the better of the two as a floor for your real
  download speed, not a ceiling.
- Windows only for the same reasons as the server ranker.

## License

MIT. See [LICENSE](LICENSE).
