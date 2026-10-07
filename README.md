# proton-best-server

Find the lowest-latency Proton VPN server for a country, straight from the server
list the Proton VPN Windows client already caches on disk.

No login, no API key, no third-party packages. Python 3.8+ on Windows.

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

## License

MIT. See [LICENSE](LICENSE).
