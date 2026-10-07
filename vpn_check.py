#!/usr/bin/env python3
"""
Check the connection you are on right now, VPN or not.

Reports, in order:
  1. Whether a Proton / WireGuard / TAP adapter is up.
  2. Public IPv4 and IPv6 exit, with city, country and network owner.
  3. DNS servers per interface, flagging DNS that bypasses the tunnel.
  4. Ping latency and loss to a few well-known hosts.
  5. Download and upload throughput, using Vultr's public test files for the
     exit country (Cloudflare's speed test rejects VPN exits) and Cloudflare's
     upload endpoint, which does accept them.

Save a run with --json, then pass that file to --compare on a later run to
see the difference, for example disconnected vs connected.

No third-party packages. Python 3.8+ on Windows.

Usage:
  python vpn_check.py
  python vpn_check.py --json before.json          # save results
  python vpn_check.py --compare before.json       # show deltas against a saved run
  python vpn_check.py --no-speed                  # skip throughput tests
  python vpn_check.py --download-url https://host/100MB.bin
"""
import argparse
import json
import os
import re
import subprocess
import sys
import threading
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime

UA = {"User-Agent": "vpn-check/1.0"}
PING_HOSTS = ["1.1.1.1", "8.8.8.8", "www.google.com", "www.youtube.com", "steamcommunity.com"]

# Vultr public test files, keyed by exit country. Each is a 100 MB file.
VULTR = {
    "JP": "hnd-jp", "KR": "icn-kr", "SG": "sgp", "IN": "bom-in", "AU": "syd-au",
    "US": "lax-ca-us", "CA": "yto-ca", "MX": "mex-mx", "BR": "sao-br",
    "GB": "lon-gb", "DE": "fra-de", "FR": "cdg-fr", "NL": "ams-nl", "PL": "waw-pl",
    "ES": "mad-es", "SE": "sto-se", "ZA": "jnb-za", "IL": "tlv-il",
}
DEFAULT_LOC = "lax-ca-us"
UPLOAD_URL = "https://speed.cloudflare.com/__up"
UPLOAD_BYTES = 25_000_000
PARALLEL = 4


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def ps(cmd, timeout=20):
    try:
        out = subprocess.run(["powershell", "-NoProfile", "-Command", cmd],
                             capture_output=True, text=True, timeout=timeout)
        return out.stdout
    except Exception:
        return ""


def get_json(url, timeout=10):
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


def mbps(nbytes, seconds):
    return nbytes * 8 / 1e6 / seconds if seconds > 0 else 0.0


def section(title):
    print("\n=== %s ===" % title)


# ---------------------------------------------------------------------------
# 1. Adapter
# ---------------------------------------------------------------------------
def vpn_adapters():
    out = ps("Get-NetAdapter | Where-Object { $_.Name -like '*Proton*' -or "
             "$_.InterfaceDescription -match 'WireGuard|TAP|Wintun|OpenVPN' } | "
             "ForEach-Object { \"$($_.Name)`t$($_.Status)`t$($_.InterfaceDescription)\" }")
    rows = []
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) == 3:
            rows.append({"name": parts[0], "status": parts[1], "desc": parts[2]})
    return rows


# ---------------------------------------------------------------------------
# 2. Public exit
# ---------------------------------------------------------------------------
def public_ip(family):
    host = "api.ipify.org" if family == 4 else "api6.ipify.org"
    try:
        ip = get_json("https://%s?format=json" % host)["ip"]
    except Exception:
        return None
    info = {"ip": ip}
    try:
        geo = get_json("https://ipinfo.io/%s/json" % ip)
        info.update({k: geo.get(k, "") for k in ("city", "region", "country", "org")})
    except Exception:
        pass
    return info


# ---------------------------------------------------------------------------
# 3. DNS
# ---------------------------------------------------------------------------
def dns_servers():
    out = ps("Get-DnsClientServerAddress -AddressFamily IPv4 | Where-Object ServerAddresses | "
             "ForEach-Object { \"$($_.InterfaceAlias)`t$($_.ServerAddresses -join ',')\" }")
    rows = []
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) == 2:
            rows.append({"iface": parts[0], "servers": parts[1].split(",")})
    return rows


# ---------------------------------------------------------------------------
# 4. Ping
# ---------------------------------------------------------------------------
_PING_STATS = re.compile(r"Minimum = (\d+)ms, Maximum = (\d+)ms, Average = (\d+)ms")
_PING_LOSS = re.compile(r"Sent = (\d+), Received = (\d+)")


def ping(host, count=10, timeout_ms=2000):
    res = {"host": host, "loss": 100, "min": None, "avg": None, "max": None}
    try:
        out = subprocess.run(["ping", "-n", str(count), "-w", str(timeout_ms), host],
                             capture_output=True, text=True,
                             timeout=count * (timeout_ms / 1000 + 1) + 5).stdout
    except subprocess.TimeoutExpired:
        return res
    m = _PING_LOSS.search(out)
    if m:
        sent, recv = int(m.group(1)), int(m.group(2))
        res["loss"] = round(100 * (sent - recv) / sent) if sent else 100
    m = _PING_STATS.search(out)
    if m:
        res.update(min=int(m.group(1)), max=int(m.group(2)), avg=int(m.group(3)))
    return res


# ---------------------------------------------------------------------------
# 5. Throughput
# ---------------------------------------------------------------------------
def download(url, timeout=120):
    """Return (bytes, seconds, http_status). Streams to /dev/null, never to disk."""
    req = urllib.request.Request(url, headers=UA)
    t0 = time.perf_counter()
    n = 0
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            status = r.status
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                n += len(chunk)
    except urllib.error.HTTPError as e:
        return 0, time.perf_counter() - t0, e.code
    except Exception:
        return 0, time.perf_counter() - t0, 0
    return n, time.perf_counter() - t0, status


def download_parallel(url, streams):
    total = [0]
    lock = threading.Lock()

    def one():
        n, _, _ = download(url)
        with lock:
            total[0] += n

    t0 = time.perf_counter()
    with ThreadPoolExecutor(max_workers=streams) as pool:
        for _ in range(streams):
            pool.submit(one)
    return total[0], time.perf_counter() - t0


def upload(url, nbytes, timeout=120):
    body = os.urandom(nbytes)
    req = urllib.request.Request(url, data=body, method="POST",
                                 headers={**UA, "Content-Type": "application/octet-stream"})
    t0 = time.perf_counter()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            r.read()
            return nbytes, time.perf_counter() - t0, r.status
    except urllib.error.HTTPError as e:
        return 0, time.perf_counter() - t0, e.code
    except Exception:
        return 0, time.perf_counter() - t0, 0


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="Check the current connection: exit, leaks, latency, throughput.")
    ap.add_argument("--json", metavar="FILE", help="save results to this JSON file")
    ap.add_argument("--compare", metavar="FILE", help="print deltas against a saved --json run")
    ap.add_argument("--no-speed", action="store_true", help="skip download/upload tests")
    ap.add_argument("--download-url", help="override the download test file")
    ap.add_argument("--streams", type=int, default=PARALLEL, help="parallel download streams (default %d)" % PARALLEL)
    ap.add_argument("--pings", type=int, default=10, help="pings per host (default 10)")
    ap.add_argument("--no-pause", action="store_true", help="do not wait for Enter before exiting")
    args = ap.parse_args()
    global PAUSE
    PAUSE = not args.no_pause

    result = {"time": datetime.now().strftime("%Y-%m-%d %H:%M:%S")}

    section("VPN adapters")
    adapters = vpn_adapters()
    up = [a for a in adapters if a["status"] == "Up"]
    result["vpn_up"] = [a["name"] for a in up]
    if not adapters:
        print("  none found")
    for a in adapters:
        print("  %-24s %-12s %s" % (a["name"], a["status"], a["desc"]))
    print("  tunnel: %s" % (", ".join(result["vpn_up"]) if up else "NOT connected"))

    section("Public exit")
    v4 = public_ip(4)
    v6 = public_ip(6)
    result["ipv4"], result["ipv6"] = v4, v6
    for label, info in (("IPv4", v4), ("IPv6", v6)):
        if info:
            print("  %s  %-40s %s, %s  (%s)" % (label, info["ip"], info.get("city", "?"),
                                                 info.get("country", "?"), info.get("org", "?")))
        else:
            print("  %s  no route" % label)
    if up and v4 and v6 and v6.get("country") and v6.get("country") != v4.get("country"):
        print("  WARNING: IPv6 exits in a different country from IPv4. Likely an IPv6 leak.")
    elif up and v6 and v4 and v6.get("org") and v6.get("org") != v4.get("org"):
        print("  note: IPv6 and IPv4 exit through different networks (%s vs %s). "
              "Check that both belong to your VPN provider." % (v6.get("org"), v4.get("org")))

    section("DNS servers")
    dns = dns_servers()
    result["dns"] = dns
    for d in dns:
        print("  %-24s %s" % (d["iface"], ", ".join(d["servers"])))
    if up:
        vpn_names = {a["name"] for a in up}
        if not any(d["iface"] in vpn_names for d in dns):
            print("  WARNING: no DNS server bound to the VPN adapter. Lookups may bypass the tunnel.")

    section("Latency (%d pings each)" % args.pings)
    with ThreadPoolExecutor(max_workers=len(PING_HOSTS)) as pool:
        pings = list(pool.map(lambda h: ping(h, args.pings), PING_HOSTS))
    result["ping"] = {p["host"]: p for p in pings}
    for p in pings:
        if p["avg"] is None:
            print("  %-22s unreachable (%d%% loss)" % (p["host"], p["loss"]))
        else:
            print("  %-22s avg %4d ms   min %4d   max %4d   loss %d%%" % (
                p["host"], p["avg"], p["min"], p["max"], p["loss"]))

    if not args.no_speed:
        country = (v4 or {}).get("country", "")
        loc = VULTR.get(country, DEFAULT_LOC)
        url = args.download_url or "https://%s-ping.vultr.com/vultr.com.100MB.bin" % loc
        section("Download (%s)" % url.split("/")[2])
        n, s, code = download(url)
        if n:
            single = mbps(n, s)
            print("  single stream   %6.0f Mbps  (%.1f s)" % (single, s))
        else:
            single = 0
            print("  single stream   failed (HTTP %s)" % code)
        n, s = download_parallel(url, args.streams)
        multi = mbps(n, s) if n else 0
        if n:
            print("  %d streams       %6.0f Mbps  (%.1f s)" % (args.streams, multi, s))
        else:
            print("  %d streams       failed" % args.streams)
        result["download_mbps"] = {"single": round(single), "multi": round(multi), "url": url}

        section("Upload (speed.cloudflare.com)")
        n, s, code = upload(UPLOAD_URL, UPLOAD_BYTES)
        if n:
            up_mbps = mbps(n, s)
            print("  %d MB            %6.0f Mbps  (%.1f s)" % (UPLOAD_BYTES // 1_000_000, up_mbps, s))
        else:
            up_mbps = 0
            print("  failed (HTTP %s)" % code)
        result["upload_mbps"] = round(up_mbps)

    if args.compare:
        try:
            with open(args.compare, encoding="utf-8") as fh:
                old = json.load(fh)
        except Exception as e:
            print("\ncould not read %s: %s" % (args.compare, e))
            old = None
        if old:
            section("Compared with %s (%s)" % (args.compare, old.get("time", "?")))
            print("  %-22s %10s %10s %8s" % ("", "before", "now", "delta"))

            def row(label, a, b, unit):
                if a is None or b is None:
                    return
                print("  %-22s %7s %s %7s %s %+8d" % (label, a, unit, b, unit, b - a))

            for h in PING_HOSTS:
                row("ping " + h, old.get("ping", {}).get(h, {}).get("avg"),
                    result.get("ping", {}).get(h, {}).get("avg"), "ms")
            od, nd = old.get("download_mbps", {}), result.get("download_mbps", {})
            row("download single", od.get("single"), nd.get("single"), "Mb")
            row("download multi", od.get("multi"), nd.get("multi"), "Mb")
            row("upload", old.get("upload_mbps"), result.get("upload_mbps"), "Mb")
            oe, ne = (old.get("ipv4") or {}), (v4 or {})
            if oe.get("ip") != ne.get("ip"):
                print("  exit changed: %s (%s) -> %s (%s)" % (
                    oe.get("ip"), oe.get("city"), ne.get("ip"), ne.get("city")))

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(result, fh, indent=2)
        print("\nsaved %s" % args.json)


PAUSE = True


def _pause():
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
