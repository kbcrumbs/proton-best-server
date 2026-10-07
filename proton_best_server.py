#!/usr/bin/env python3
"""
Find the lowest-latency Proton VPN server for a country (default: Japan).

Reads the server list that the Proton VPN Windows client caches on disk
(%LOCALAPPDATA%/Proton/Proton VPN/Storage/Servers.*.bin, a protobuf file),
pings every unique entry node for the chosen exit country, and prints them
ranked by round-trip time, together with the server load Proton reported at
the client's last refresh.

Run it with the VPN DISCONNECTED. While the tunnel is up, every ping goes out
through the current VPN exit, so the numbers describe that exit's path, not
yours. No third-party packages needed; Python 3.8+ on Windows.

Usage:
  python proton_best_server.py                 # Japan, Plus servers, no Secure Core
  python proton_best_server.py --country SG    # another exit country
  python proton_best_server.py --include-free --include-secure-core
  python proton_best_server.py --pings 10 --top 15
  python proton_best_server.py --force         # test even while the VPN is connected
"""
import argparse
import glob
import os
import re
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime

# Proton feature bits (from the public Proton VPN API)
F_SECURE_CORE = 1
F_TOR = 2
F_P2P = 4
F_STREAMING = 8
F_IPV6 = 16
TIER_NAMES = {0: "Free", 1: "Basic", 2: "Plus"}


# ---------------------------------------------------------------------------
# Minimal protobuf wire-format reader (no .proto needed)
# ---------------------------------------------------------------------------
def _varint(buf, i):
    result = shift = 0
    while True:
        byte = buf[i]
        i += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return result, i


def _fields(buf):
    """Yield (field_number, wire_type, value) for one protobuf message."""
    i, n = 0, len(buf)
    while i < n:
        key, i = _varint(buf, i)
        field, wtype = key >> 3, key & 7
        if wtype == 0:
            val, i = _varint(buf, i)
        elif wtype == 1:
            val, i = buf[i:i + 8], i + 8
        elif wtype == 2:
            length, i = _varint(buf, i)
            val, i = buf[i:i + length], i + length
        elif wtype == 5:
            val, i = buf[i:i + 4], i + 4
        else:
            raise ValueError("unsupported wire type %d at offset %d" % (wtype, i))
        yield field, wtype, val


def _s(v):
    return v.decode("utf-8", "replace") if isinstance(v, (bytes, bytearray)) else v


# ---------------------------------------------------------------------------
# Cache file
# ---------------------------------------------------------------------------
def find_cache():
    base = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Proton", "Proton VPN", "Storage")
    hits = glob.glob(os.path.join(base, "Servers.*.bin"))
    if not hits:
        sys.exit("No Proton VPN server cache found under %s\n"
                 "Open the Proton VPN app once so it downloads the server list, then retry." % base)
    return max(hits, key=os.path.getmtime)


def load_servers(path):
    """Return a list of logical servers from the client's protobuf cache.

    Field numbers observed in the Windows client v5.1.x cache:
      top-level 3 = LogicalServer
        2 name, 3 city, 5 entry country, 6 exit country,
        13 tier, 14 features bitmask, 15 load %, 16 score (float32),
        17 physical node (repeated): 2 entry IP, 4 domain, 6 status
    """
    with open(path, "rb") as fh:
        data = fh.read()
    servers = []
    for field, wtype, val in _fields(data):
        if field != 3 or wtype != 2:
            continue
        srv = {"nodes": []}
        for f, wt, v in _fields(val):
            if f == 2:
                srv["name"] = _s(v)
            elif f == 3:
                srv["city"] = _s(v)
            elif f == 5:
                srv["entry"] = _s(v)
            elif f == 6:
                srv["exit"] = _s(v)
            elif f == 13:
                srv["tier"] = v
            elif f == 14:
                srv["features"] = v
            elif f == 15:
                srv["load"] = v
            elif f == 16 and wt == 5:
                srv["score"] = struct.unpack("<f", v)[0]
            elif f == 17 and wt == 2:
                node = {}
                for nf, nwt, nv in _fields(v):
                    if nf == 2:
                        node["ip"] = _s(nv)
                    elif nf == 4:
                        node["domain"] = _s(nv)
                    elif nf == 6:
                        node["status"] = nv
                if node.get("ip"):
                    srv["nodes"].append(node)
        if srv.get("name"):
            servers.append(srv)
    return servers


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------
def vpn_adapter_up():
    """Return the name of an active Proton/WireGuard/TAP adapter, or ''."""
    cmd = ("(Get-NetAdapter | Where-Object { $_.Status -eq 'Up' -and "
           "($_.Name -like '*Proton*' -or $_.InterfaceDescription -like '*WireGuard*' "
           "-or $_.InterfaceDescription -like '*TAP-Proton*') }).Name")
    try:
        out = subprocess.run(["powershell", "-NoProfile", "-Command", cmd],
                             capture_output=True, text=True, timeout=20)
        return out.stdout.strip()
    except Exception:
        return ""


# ---------------------------------------------------------------------------
# Ping
# ---------------------------------------------------------------------------
_PING_STATS = re.compile(r"Minimum = (\d+)ms, Maximum = (\d+)ms, Average = (\d+)ms")
_PING_LOSS = re.compile(r"Sent = (\d+), Received = (\d+)")


def ping(ip, count, timeout_ms):
    empty = {"ip": ip, "loss": 100, "min": None, "avg": None, "max": None}
    try:
        out = subprocess.run(["ping", "-n", str(count), "-w", str(timeout_ms), ip],
                             capture_output=True, text=True,
                             timeout=(count * (timeout_ms / 1000 + 1)) + 5).stdout
    except subprocess.TimeoutExpired:
        return empty
    loss = 100
    m = _PING_LOSS.search(out)
    if m:
        sent, recv = int(m.group(1)), int(m.group(2))
        loss = round(100 * (sent - recv) / sent) if sent else 100
    m = _PING_STATS.search(out)
    if m:
        return {"ip": ip, "loss": loss, "min": int(m.group(1)),
                "avg": int(m.group(3)), "max": int(m.group(2))}
    empty["loss"] = loss
    return empty


def _ms(v):
    return "%4dms" % v if v is not None else "   --"


def _flags(e):
    f = e["features"]
    out = []
    if f & F_SECURE_CORE:
        out.append("SC via " + e["entry"])
    if f & F_P2P:
        out.append("P2P")
    if f & F_STREAMING:
        out.append("Stream")
    if f & F_TOR:
        out.append("Tor")
    return ",".join(out)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="Rank Proton VPN servers in a country by ping latency.")
    ap.add_argument("--country", default="JP", help="exit country code (default JP)")
    ap.add_argument("--include-free", action="store_true", help="also test Free-tier servers")
    ap.add_argument("--include-secure-core", action="store_true", help="also test Secure Core routes")
    ap.add_argument("--pings", type=int, default=5, help="pings per node (default 5)")
    ap.add_argument("--timeout", type=int, default=1500, help="per-ping timeout in ms (default 1500)")
    ap.add_argument("--workers", type=int, default=8, help="parallel pings (default 8)")
    ap.add_argument("--top", type=int, default=20, help="rows to print (default 20)")
    ap.add_argument("--max-load", type=int, default=80,
                    help="ignore nodes above this load percent for the recommendation (default 80)")
    ap.add_argument("--force", action="store_true", help="run even if a VPN tunnel is up")
    ap.add_argument("--no-pause", action="store_true",
                    help="do not wait for Enter before exiting (for use from scripts)")
    args = ap.parse_args()
    global PAUSE
    PAUSE = not args.no_pause
    country = args.country.upper()

    up = vpn_adapter_up()
    if up and not args.force:
        sys.exit("VPN adapter '%s' is connected. Pings would travel through the tunnel and "
                 "measure the current exit's path, not yours.\n"
                 "Disconnect Proton VPN and run again (or pass --force to measure anyway)." % up)
    if up:
        print("WARNING: VPN adapter '%s' is up; results reflect the tunnel path, not yours.\n" % up)

    cache = find_cache()
    refreshed = datetime.fromtimestamp(os.path.getmtime(cache)).strftime("%Y-%m-%d %H:%M")
    servers = load_servers(cache)

    # Filter, then group logical servers by physical entry IP so each node is pinged once.
    by_ip = {}
    for s in servers:
        if s.get("exit") != country:
            continue
        feats = s.get("features", 0)
        if feats & F_SECURE_CORE and not args.include_secure_core:
            continue
        if s.get("tier", 2) == 0 and not args.include_free:
            continue
        for n in s["nodes"]:
            if n.get("status", 1) != 1:
                continue
            e = by_ip.setdefault(n["ip"], {
                "ip": n["ip"], "domain": n.get("domain", ""), "names": set(),
                "city": s.get("city", ""), "entry": s.get("entry", ""),
                "tier": s.get("tier"), "features": feats, "loads": [], "scores": []})
            e["names"].add(s["name"])
            if "load" in s:
                e["loads"].append(s["load"])
            if "score" in s:
                e["scores"].append(s["score"])

    if not by_ip:
        sys.exit("No %s servers matched. Try --include-free or --include-secure-core, "
                 "or check the country code." % country)

    print("Server list: %s\n  cached %s (load values are from that refresh)" % (cache, refreshed))
    print("Testing %d unique %s entry nodes with %d pings each...\n" % (len(by_ip), country, args.pings))

    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        results = list(pool.map(lambda ip: ping(ip, args.pings, args.timeout), by_ip))

    rows = []
    for r in results:
        e = by_ip[r["ip"]]
        e.update(r)
        e["load"] = round(sum(e["loads"]) / len(e["loads"])) if e["loads"] else None
        e["score"] = min(e["scores"]) if e["scores"] else None
        rows.append(e)

    rows.sort(key=lambda e: (e["avg"] is None, e["avg"] if e["avg"] is not None else 0, e["loss"]))

    hdr = "%5s %5s %5s %5s %5s %-5s %-8s %-16s %s" % (
        "avg", "min", "max", "loss", "load", "tier", "city", "ip", "servers")
    print(hdr)
    print("-" * len(hdr))
    for e in rows[:args.top]:
        names = sorted(e["names"], key=lambda n: (len(n), n))
        shown = ", ".join(names[:4]) + (" (+%d)" % (len(names) - 4) if len(names) > 4 else "")
        load = ("%d%%" % e["load"]) if e["load"] is not None else "--"
        fl = _flags(e)
        print("%s %s %s %4d%% %5s %-5s %-8s %-16s %s%s" % (
            _ms(e["avg"]), _ms(e["min"]), _ms(e["max"]), e["loss"], load,
            TIER_NAMES.get(e["tier"], "?"), e["city"], e["ip"], shown,
            ("  [%s]" % fl) if fl else ""))

    good = [e for e in rows if e["avg"] is not None and e["loss"] == 0
            and (e["load"] is None or e["load"] <= args.max_load)]
    if good:
        best = good[0]
        names = sorted(best["names"], key=lambda n: (len(n), n))
        print("\nRecommended: %s (%s, %s) - %d ms avg, %s%% load" % (
            names[0], best["city"], best["ip"], best["avg"], best["load"]))
        if len(names) > 1:
            print("  Same physical node, so any of these is equivalent: " + ", ".join(names))
        print("  In the Proton VPN app, search for that server name and connect to it.")
    else:
        print("\nNo node answered all pings under the load limit; try --max-load 100 or more --pings.")


PAUSE = True


def _pause():
    # Keep the console window open when the script was started by double-click.
    if PAUSE and sys.stdin and sys.stdin.isatty():
        try:
            input("Press Enter to close...")
        except (EOFError, KeyboardInterrupt):
            pass


if __name__ == "__main__":
    try:
        main()
    except SystemExit as e:
        if e.code not in (None, 0):
            print(e.code if isinstance(e.code, str) else "Exited with code %s" % e.code)
    except Exception:
        import traceback
        traceback.print_exc()
    finally:
        _pause()
