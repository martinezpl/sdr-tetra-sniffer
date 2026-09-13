#!/usr/bin/env python3
"""A read-only web overview of a tetra-sniff run.

It parses the files that a run writes and serves one page. It never writes to
the run directory and it never talks to the recorder, so it can be started,
stopped or edited while a capture is going on.

usage:
  ui.py [--in DIR] [--bind ADDR] [--port N]

  --in     the parent of the run directories, which is the same directory that
           "run" was given as --out. Default "recordings".
  --bind   the address to listen on. Default 0.0.0.0, which is every network.
           Give the address of one interface to limit it to that network.
  --port   default 8080.

There is no authentication. Anyone who can reach the address can hear the
recordings, so bind it to a network you trust.
"""

import argparse
import calendar
import html
import json
import os
import re
import socket
import struct
import time
from bisect import bisect_right
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

VFO_RATE = 36000.0  # capture samples for each second, see demod_chain.h
FRAME = 480  # WAV samples for each calls.log line, see stitch.cpp
HDR = 44  # bytes of WAV header, see recorder.cpp
RATE = 8000.0  # WAV sample rate
MISSED = ("OUTSIDE", "BADFREQ", "NOSLOT", "UNTUNED")


def newest_run(out):
    """The run directory that holds the most recent capture."""
    try:
        runs = [
            d
            for d in os.scandir(out)
            if d.is_dir() and re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{6}Z", d.name)
        ]
    except OSError:
        return None
    return max(runs, key=lambda d: d.name).path if runs else None


_DAY = {}


def epoch(ts):
    """2026-09-12T22:15:49.076Z as epoch seconds. The field is fixed width, so
    slice it. time.strptime does a locale lookup for every row, and on a log of
    one row each second it was most of the cost of loading the page."""
    day = _DAY.get(ts[:10])
    if day is None:
        day = calendar.timegm((int(ts[0:4]), int(ts[5:7]), int(ts[8:10]), 0, 0, 0, 0, 1, 0))
        _DAY[ts[:10]] = day
    return (day + int(ts[11:13]) * 3600 + int(ts[14:16]) * 60 + int(ts[17:19])
            + (float(ts[19:-1]) if len(ts) > 20 else 0.0))


def tail(path, off, ino):
    """The complete lines added since off, the new offset, the inode, and
    whether the read had to start over. A run appends to these files for hours,
    so re-reading them whole on every refresh is the other half of the cost. A
    changed inode or a file that shrank means start again."""
    reset = False
    try:
        st = os.stat(path)
    except OSError:
        return [], off, ino, reset
    if st.st_ino != ino or st.st_size < off:
        off, reset = 0, ino is not None
    if st.st_size == off:
        return [], off, st.st_ino, reset
    try:
        with open(path, "rb") as f:
            f.seek(off)
            buf = f.read(st.st_size - off)
    except OSError:
        return [], off, st.st_ino, reset
    # The recorder may be part way through a line. Stop at the last newline.
    cut = buf.rfind(b"\n")
    if cut < 0:
        return [], off, st.st_ino, reset
    return (buf[:cut + 1].decode("utf-8", "replace").splitlines(),
            off + cut + 1, st.st_ino, reset)


def parse_clock(path, st):
    """The header of clock.log, its rows as (sample, epoch) for UTC, and the
    two numbers that only the whole file holds: the highest queue the run ever
    reached, and how many times the wall clock stepped. A page that reads the
    last row alone would show a queue of 0 through a spike that dropped data.
    st carries what earlier calls already read, so only new rows are parsed."""
    lines, st["clock_off"], st["clock_ino"], reset = tail(
        path, st["clock_off"], st["clock_ino"])
    if reset:
        st.update(meta={}, rows=[], keys=[], last=None, peak=0, steps=0)
    meta, rows, keys = st["meta"], st["rows"], st["keys"]
    last, peak, steps = st["last"], st["peak"], st["steps"]
    for line in lines:
        if line.startswith("#"):
            for k, v in re.findall(r"(\w+)=(\S+)", line):
                meta[k] = v
            continue
        c = line.split()
        if len(c) < 5:
            continue
        try:
            when = epoch(c[0])
        except (ValueError, IndexError):
            continue
        rows.append((int(c[1]), when))
        keys.append(int(c[1]))
        peak = max(peak, int(c[2]))
        steps += c[4] == "STEP"
        last = c
    st["last"], st["peak"], st["steps"] = last, peak, steps
    return meta, rows, keys, last, peak, steps



def sample_to_utc(rows, keys, sample):
    """A capture sample as a UTC epoch, interpolated inside one clock.log row.
    keys is the sample column of rows, built once: rebuilding it here cost a
    pass over the whole log for every talkgroup and every missed call."""
    if not rows:
        return None
    i = bisect_right(keys, sample) - 1
    if i < 0:
        return rows[0][1] + (sample - rows[0][0]) / VFO_RATE
    if i + 1 < len(rows):
        s0, t0 = rows[i]
        s1, t1 = rows[i + 1]
        if s1 > s0:
            return t0 + (t1 - t0) * (sample - s0) / (s1 - s0)
    s0, t0 = rows[i]
    return t0 + (sample - s0) / VFO_RATE


def iso(epoch):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(epoch)) if epoch else None


def cpu_temp():
    """The CPU temperature, because a host that throttles starts dropping."""
    try:
        with open("/sys/class/thermal/thermal_zone0/temp") as f:
            return round(int(f.read().strip()) / 1000.0, 1)
    except (OSError, ValueError):
        return None


def wav_samples(path):
    try:
        return max(0, (os.path.getsize(path) - HDR) // 2)
    except OSError:
        return 0


# What each run directory has been read up to, so a refresh parses only what
# the recorder appended since the last one.
_STATE = {}


def scan(out):
    """Everything the page shows, from the files of the newest run."""
    run = newest_run(out)
    if not run:
        return {"error": "no run directory under %s" % out}
    st = _STATE.get(run)
    if st is None:
        # Only the newest run is ever shown, so one record is enough.
        _STATE.clear()
        st = {"calls_off": 0, "calls_ino": None, "clock_off": 0, "clock_ino": None,
              "meta": {}, "rows": [], "keys": [], "last": None, "peak": 0, "steps": 0,
              "talkgroups": {}, "carriers": {}, "controls": {}, "missed": [],
              "missed_total": 0}
        _STATE[run] = st

    meta, rows, keys, last, peak_queue, steps = parse_clock(os.path.join(run, "clock.log"), st)
    center = float(meta.get("center", 0)) or None
    iq_rate = float(meta.get("iq_rate", 0)) or None

    lines, st["calls_off"], st["calls_ino"], reset = tail(
        os.path.join(run, "calls.log"), st["calls_off"], st["calls_ino"])
    if reset:
        st.update(talkgroups={}, carriers={}, controls={}, missed=[], missed_total=0)
    talkgroups, carriers = st["talkgroups"], st["carriers"]
    controls, missed = st["controls"], st["missed"]
    # Each line of calls.log is one voice frame, or one call that was not taken.
    for line in lines:
        if line.startswith("#"):
            continue
        c = line.split()
        if len(c) < 9:
            continue
        sample, issi, gssi, hz, ctl, tn, status = (
            int(c[0]),
            c[2],
            c[3],
            c[4],
            c[5],
            c[6],
            c[8],
        )
        if status in MISSED:
            st["missed_total"] += 1
            missed.append(
                {
                    "sample": sample,
                    "gssi": gssi,
                    "issi": issi,
                    "hz": int(hz),
                    "control_hz": int(ctl),
                    "status": status,
                }
            )
            # The page shows the last 200, so older ones need not be kept.
            if len(missed) > 400:
                del missed[:200]
            continue
        t = talkgroups.setdefault(
            gssi,
            {
                "gssi": gssi,
                "frames": 0,
                "first": sample,
                "last": sample,
                "carriers": {},
                "issis": {},
            },
        )
        t["frames"] += 1
        t["last"] = sample
        t["carriers"][hz] = t["carriers"].get(hz, 0) + 1
        if issi != "-":
            t["issis"][issi] = t["issis"].get(issi, 0) + 1
        carriers[hz] = carriers.get(hz, 0) + 1
        if ctl != "-":
            controls[ctl] = controls.get(ctl, 0) + 1

    out_tg = []
    for base in talkgroups.values():
        t = dict(base, carriers=dict(base["carriers"]), issis=dict(base["issis"]))
        out_tg.append(t)
        wav = os.path.join(run, "calls", t["gssi"] + ".wav")
        t["seconds"] = wav_samples(wav) / RATE
        t["first_utc"] = iso(sample_to_utc(rows, keys, t["first"]))
        t["last_utc"] = iso(sample_to_utc(rows, keys, t["last"]))
        t["carriers"] = sorted(
            ({"hz": int(h), "frames": n} for h, n in t["carriers"].items()),
            key=lambda x: -x["frames"],
        )
        t["issis"] = sorted(
            ({"issi": i, "frames": n} for i, n in t["issis"].items()),
            key=lambda x: -x["frames"],
        )[:4]
        t["has_audio"] = os.path.exists(wav)
    talkgroups = {t["gssi"]: t for t in out_tg}

    for m in missed:
        m["utc"] = iso(sample_to_utc(rows, keys, m["sample"]))

    # The pool, as the run was given it and as grants filled it.
    followed = sorted(int(h) for h in carriers)
    # DIR/carriers holds every carrier a slot follows. A line marked "learned"
    # is one that a grant revealed; the rest came from the command line.
    pool, learned = [], []
    try:
        with open(os.path.join(out, "carriers")) as f:
            for line in f:
                if line.startswith("#") or not line.strip():
                    continue
                part = line.split()
                try:
                    hz = int(part[0])
                except ValueError:
                    continue
                pool.append(hz)
                if len(part) > 1 and part[1] == "learned":
                    learned.append(hz)
    except OSError:
        pass

    half = iq_rate / 2 if iq_rate else None
    span = {
        "center": center,
        "low": center - half if half else None,
        "high": center + half if half else None,
        # Past 80% of Nyquist the dongle rolls off and a carrier loses SNR.
        "good_low": center - 0.8 * half if half else None,
        "good_high": center + 0.8 * half if half else None,
    }

    # Every frequency worth drawing in the band, in span or out of it.
    marks = {}
    for h, n in carriers.items():
        marks[int(h)] = {"hz": int(h), "frames": n, "role": "carrier", "gssis": []}
    for h in controls:
        marks.setdefault(
            int(h), {"hz": int(h), "frames": 0, "role": "carrier", "gssis": []}
        )["role"] = "control"
    for m in missed:
        mk = marks.setdefault(
            m["hz"], {"hz": m["hz"], "frames": 0, "role": m["status"], "gssis": []}
        )
        if mk["role"] in MISSED and m["gssi"] not in mk["gssis"]:
            mk["gssis"].append(m["gssi"])
    for mk in marks.values():
        mk["in_span"] = bool(half and abs(mk["hz"] - center) + 15e3 < half)

    health = {}
    if last:
        health = {
            "utc": last[0],
            "samples": int(last[1]),
            "queue": int(last[2]),
            "dropped": int(last[3]),
            "event": last[4],
            "peak_queue": peak_queue,
            "steps": steps,
            "temp_c": cpu_temp(),
            "seconds": int(last[1]) / VFO_RATE,
        }

    return {
        "run": os.path.basename(run),
        "start_utc": meta.get("start_utc"),
        "iq_rate": iq_rate,
        "span": span,
        "health": health,
        # Sorted by GSSI, not by activity: a row that moves under the cursor is
        # both annoying and, for a row holding an <audio>, a stopped playback.
        "talkgroups": sorted(talkgroups.values(), key=lambda t: int(t["gssi"])),
        "carriers": followed,
        "pool": pool,
        "learned": learned,
        "controls": sorted(int(h) for h in controls),
        "marks": sorted(marks.values(), key=lambda m: m["hz"]),
        "missed": missed[-200:],
        "missed_total": st["missed_total"],
        "generated": iso(time.time()),
    }


def local_addresses():
    """Every IPv4 address this host answers on, so the start line prints a URL
    that can be pasted. The name of the host is no good: on Debian it resolves
    to 127.0.1.1. A connected UDP socket finds only the address of the default
    route, which misses a second network such as an access point."""
    found = []
    try:
        import fcntl

        SIOCGIFADDR = 0x8915  # Linux
        for _, name in socket.if_nameindex():
            sk = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                packed = fcntl.ioctl(
                    sk.fileno(), SIOCGIFADDR, struct.pack("256s", name.encode()[:15])
                )
                found.append(socket.inet_ntoa(packed[20:24]))
            except OSError:
                pass  # an interface with no IPv4 address of its own
            finally:
                sk.close()
    except (ImportError, AttributeError, OSError):
        pass
    if not found:
        # Not Linux. One address beats none: the one that would carry a packet
        # out. Connecting a UDP socket sends nothing, so the host is never used.
        sk = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sk.connect(("192.0.2.1", 9))  # TEST-NET-1, which is never routed
            found.append(sk.getsockname()[0])
        except OSError:
            pass
        finally:
            sk.close()
    # Loopback last: it is a true answer but it is not the one to paste.
    return sorted(set(found), key=lambda x: (x.startswith("127."), x))


class Handler(BaseHTTPRequestHandler):
    recordings = "recordings"
    server_version = "tetra-sniff-ui"

    # A browser abandons a request whenever it seeks in the audio, pauses it,
    # or leaves the page, and the next write to that socket fails. That is
    # ordinary traffic for a media element, not a fault, so it must not put a
    # traceback in the log of a program somebody is watching.
    GONE = (BrokenPipeError, ConnectionResetError, ConnectionAbortedError)

    def handle_one_request(self):
        try:
            super().handle_one_request()
        except self.GONE:
            self.close_connection = True

    def log_message(self, fmt, *args):
        pass  # one line for each request is noise, not a log

    def do_GET(self):
        if self.path.split("?")[0] == "/api/overview":
            body = json.dumps(scan(self.recordings)).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
        elif self.path.startswith("/audio/"):
            self.send_audio()
        elif self.path.split("?")[0] in ("/", "/index.html"):
            body = PAGE.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_error(404)

    def send_audio(self):
        """A WAV of one talkgroup, with Range so a browser can seek in it."""
        name = os.path.basename(self.path[len("/audio/") :].split("?")[0])
        if not re.fullmatch(r"\d+\.wav", name):
            return self.send_error(404)
        run = newest_run(self.recordings)
        path = os.path.join(run, "calls", name) if run else None
        if not path or not os.path.exists(path):
            return self.send_error(404)
        size = os.path.getsize(path)
        start, end = 0, size - 1
        rng = self.headers.get("Range")
        if rng:
            m = re.fullmatch(r"bytes=(\d*)-(\d*)", rng.strip())
            if m:
                if m.group(1):
                    start = min(int(m.group(1)), size - 1)
                    if m.group(2):
                        end = min(int(m.group(2)), size - 1)
                elif m.group(2):
                    start = max(0, size - int(m.group(2)))
        length = max(0, end - start + 1)
        self.send_response(206 if rng else 200)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(length))
        if rng:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        self.end_headers()
        with open(path, "rb") as f:
            f.seek(start)
            left = length
            while left > 0:
                chunk = f.read(min(65536, left))
                if not chunk:
                    break
                self.wfile.write(chunk)
                left -= len(chunk)


PAGE = r"""<!doctype html><meta charset="utf-8"><title>tetra-sniff</title>
<link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'><text y='13' font-size='13'>&#128225;</text></svg>">
<style>
:root{--bg:#12151a;--fg:#e6e9ef;--dim:#8b93a3;--line:#242a34;--ok:#4ea564;
--warn:#d2a24c;--bad:#d2645c;--ctl:#5b9dd9;--car:#4ea564}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.5 ui-monospace,
SFMono-Regular,Menlo,monospace;padding:16px;-webkit-font-smoothing:antialiased}
h1{font-size:15px;margin:0 0 2px;font-weight:600}
h2{font-size:13px;margin:22px 0 8px;color:var(--dim);font-weight:600;
text-transform:uppercase;letter-spacing:.08em}
.sub{color:var(--dim);margin-bottom:14px}
.strip{display:flex;flex-wrap:wrap;gap:10px;margin-bottom:6px}
.cell{background:#181c23;border:1px solid var(--line);border-radius:8px;
padding:8px 12px;min-width:104px;position:relative}
.cell b{display:block;font-size:17px;font-weight:600;
font-variant-numeric:tabular-nums}
.cell span{color:var(--dim);font-size:11px;text-transform:uppercase;
letter-spacing:.06em}
.ok{color:var(--ok)}.warn{color:var(--warn)}.bad{color:var(--bad)}
table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums}
th,td{text-align:left;padding:5px 10px 5px 0;border-bottom:1px solid var(--line);
white-space:nowrap}
th{color:var(--dim);font-weight:600;font-size:11px;text-transform:uppercase;
letter-spacing:.06em}
td.num,th.num{text-align:right;padding-right:16px}
.wrap{overflow-x:auto}
audio{height:28px;vertical-align:middle}
.empty{color:var(--dim);padding:8px 0}

/* The band. The hatched ground is what the dongle does not receive; the solid
   block is the captured span; the lighter block inside it is clear of the
   roll-off. */
.band{position:relative;height:136px;border:1px solid var(--line);
border-radius:8px;overflow:hidden;
background:repeating-linear-gradient(135deg,#141821,#141821 5px,#171c26 5px,#171c26 10px)}
.covered,.clear{position:absolute;top:0;bottom:30px}
.covered{background:#1b2230;box-shadow:inset 1px 0 0 #39465a,inset -1px 0 0 #39465a}
.clear{background:#232d3d}
.ctr{position:absolute;top:0;bottom:30px;border-left:1px dashed #55637a}
.zone{position:absolute;top:6px;font-size:10px;letter-spacing:.04em;color:#5d6a7d;
text-align:center;white-space:nowrap;overflow:hidden;pointer-events:none}
.hit{position:absolute;top:0;bottom:30px}
.mark{position:absolute;bottom:30px;width:2px;background:var(--car);border-radius:1px}
.mark.control{background:var(--ctl);width:3px}
.mark.miss{background:var(--bad)}
.lbl{position:absolute;font-size:10px;color:var(--dim);
transform:translateX(-50%);white-space:nowrap;font-variant-numeric:tabular-nums}
.tag{position:absolute;top:6px;font-size:10px;letter-spacing:.06em;
text-transform:uppercase;color:#6d7a8d;transform:translateX(-50%)}
.scale{display:flex;justify-content:space-between;color:var(--dim);font-size:11px;
margin:6px 2px 0;font-variant-numeric:tabular-nums}
.scale b{color:var(--fg);font-weight:600}
.legend{color:var(--dim);font-size:11px;margin-bottom:6px}
/* One outline glyph on currentColor. It marks where an explanation is; the
   whole block stays the hover target, so the glyph never takes the mouse. */
.info{position:absolute;top:6px;right:7px;color:#414c5c;line-height:0;
pointer-events:none;transition:color 150ms cubic-bezier(.2,0,0,1)}
.cell:hover .info{color:var(--dim)}
/* On a heading the text and the glyph together are the target, padded out so
   it is comfortable to hit without reaching across the row. */
.ih{cursor:help;display:inline-flex;align-items:center;gap:6px;
padding:7px 6px;margin:-7px -6px;border-radius:6px;
transition:color 150ms cubic-bezier(.2,0,0,1)}
.ih:hover{color:var(--fg)}
.ih .info{position:static}
.ih:hover .info{color:var(--dim)}
.sw{display:inline-block;width:9px;height:9px;border-radius:2px;
margin:0 4px 0 12px;vertical-align:baseline;box-shadow:inset 0 0 0 1px #39465a}
.sw:first-child{margin-left:0}
.hatch{background:repeating-linear-gradient(135deg,#141821,#141821 2px,#1b212c 2px,#1b212c 4px);
border:1px solid #2a3240}

/* The disclosure. 40px tall so it is a comfortable target in a dense page. */
details{border:1px solid var(--line);border-radius:8px;background:#181c23}
summary{list-style:none;cursor:pointer;padding:0 12px;height:40px;
display:flex;align-items:center;gap:8px;color:var(--fg);border-radius:7px}
summary::-webkit-details-marker{display:none}
summary:hover{background:#1d222b}
summary .caret{color:var(--dim);transition:rotate 150ms cubic-bezier(.2,0,0,1)}
details[open] summary .caret{rotate:90deg}
summary .count{color:var(--dim);font-variant-numeric:tabular-nums}
.panel{padding:0 12px 10px;max-height:340px;overflow:auto}
</style>
<h1>tetra-sniff</h1>
<div class="sub" id="sub">loading&hellip;</div>
<div class="strip" id="strip"></div>

<h2 id="h-band">Band covered</h2>
<div class="legend">
<span class="sw hatch"></span>not received
<span class="sw" style="background:#1b2230"></span>captured span
<span class="sw" style="background:#232d3d"></span>clear of the roll-off
<span class="sw" style="background:var(--ctl)"></span>control carrier
<span class="sw" style="background:var(--car)"></span>traffic carrier
<span class="sw" style="background:var(--bad)"></span>granted, not recorded</div>
<div class="band" id="band"></div>
<div class="scale" id="scale"></div>

<h2 id="h-tg">Talkgroups</h2>
<div class="wrap"><table id="tg"></table></div><div class="empty" id="tg-empty"></div>

<h2 id="h-car">Carriers</h2>
<div class="wrap"><table id="car"></table></div><div class="empty" id="car-empty"></div>

<h2 id="h-miss">Calls not recorded</h2>
<details id="missbox">
  <summary><span class="caret">&#9654;</span><span>Calls not recorded</span>
    <span class="count" id="misscount"></span></summary>
  <div class="panel">
    <div class="wrap"><table id="miss"></table></div><div class="empty" id="miss-empty"></div>
  </div>
</details>
<script>
const INFO = '<svg class="info" viewBox="0 0 16 16" width="13" height="13" fill="none" '
  + 'stroke="currentColor" stroke-width="1.5" aria-hidden="true">'
  + '<circle cx="8" cy="8" r="6.25"/><path d="M8 7.4v3.9" stroke-linecap="round"/>'
  + '<circle cx="8" cy="4.8" r=".55" fill="currentColor" stroke="none"/></svg>';
const head = (id, text, tip) => {
  const h = document.getElementById(id);
  if (h) h.innerHTML = '<span class="ih" title="'+esc(tip)+'">'+text+INFO+'</span>';
};
const mhz = h => (h/1e6).toFixed(4);
const dur = s => { s=Math.round(s); const h=(s/3600|0), m=(s/60|0)%60;
  return h? h+"h"+String(m).padStart(2,"0")+"m" : m? m+"m"+String(s%60).padStart(2,"0")+"s" : s+"s"; };
// The run date is in the header, so a time of day is enough in the tables.
const hhmm = s => s ? s.slice(11) : "—";
const esc = s => String(s==null?"":s).replace(/[&<>"]/g, c =>
  ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));

// Rows are keyed and updated in place. Rebuilding the table would take the
// <audio> elements out of the document, which stops whatever is playing.
function tbl(id, cols, rows, empty, keyOf){
  const el = document.getElementById(id), note = document.getElementById(id+"-empty");
  note.textContent = rows.length ? "" : empty;
  el.hidden = !rows.length; note.hidden = !!rows.length;
  if (!el.tHead) el.createTHead().innerHTML = "<tr>" +
    cols.map(c=>'<th class="'+(c.cls||"")+'">'+c.h+"</th>").join("") + "</tr>";
  const body = el.tBodies[0] || el.createTBody();
  const keep = new Set(rows.map(r=>String(keyOf(r))));
  for (const tr of [...body.children]) if (!keep.has(tr.dataset.k)) tr.remove();
  rows.forEach((r,i) => {
    const k = String(keyOf(r));
    let tr = body.querySelector('tr[data-k="'+k+'"]');
    if (!tr){
      tr = document.createElement("tr"); tr.dataset.k = k;
      for (const c of cols){
        const td = document.createElement("td");
        td.className = c.cls || ""; td.innerHTML = c.f(r); tr.appendChild(td);
      }
      body.insertBefore(tr, body.children[i] || null);
      return;
    }
    cols.forEach((c,j) => {
      if (c.once) return;             // holds an <audio>; leave it alone
      const v = c.f(r);
      if (tr.cells[j].innerHTML !== v) tr.cells[j].innerHTML = v;
    });
  });
}

// text needs about 0.62% of the width for each character at this size
const zone = (left, w, text) => w > text.length*0.62 + 1.5
  ? '<div class="zone" style="left:'+left+'%;width:'+w+'%">'+text+'</div>' : '';

function band(d){
  const el = document.getElementById("band"), sc = document.getElementById("scale");
  const s = d.span;
  if (!s || !s.center){
    el.innerHTML = '<div class="empty" style="padding:12px">no centre recorded in clock.log</div>';
    sc.textContent = ""; return;
  }
  const width = s.high - s.low, half = width/2;
  // A BADFREQ is a decode error, not a frequency, so it is not on the band at
  // all. One of them measured 1090150000 Hz, which would flatten the span to a
  // sliver if it were allowed to set the scale.
  const marks = d.marks.filter(m => m.role !== "BADFREQ");
  // Widen for anything granted outside the span, but never far enough that the
  // span itself stops being the subject of the picture.
  let need = half*1.12;
  for (const m of marks) need = Math.max(need, Math.abs(m.hz - s.center) + half*0.12);
  need = Math.min(need, half*3);
  const lo = s.center - need, hi = s.center + need;
  const x = h => Math.max(0, Math.min(100, ((h - lo)/(hi - lo))*100));
  const L = x(s.low), H = x(s.high), GL = x(s.good_low), GH = x(s.good_high);
  const tipSpan = "Captured span " + mhz(s.low) + " to " + mhz(s.high) +
    " MHz. The dongle receives all of this at once, and a carrier outside it "
    + "cannot be recorded at any setting.";
  const tipClear = "Clear of the roll-off: inside 80% of Nyquist, where a carrier "
    + "keeps its full sensitivity. " + mhz(s.good_low) + " to " + mhz(s.good_high) + " MHz.";
  const tipRoll = "Roll-off: still inside the span and still decoded, but the filter "
    + "of the dongle is falling away here, so a carrier loses SNR.";
  const tipOut = "Not received. The dongle is not listening to these frequencies, so a "
    + "grant naming one is reported but cannot be recorded.";
  el.innerHTML =
    '<div class="covered" style="left:'+L+'%;width:'+(H-L)+'%"></div>' +
    '<div class="clear" style="left:'+GL+'%;width:'+(GH-GL)+'%"></div>' +
    '<div class="ctr" style="left:'+x(s.center)+'%"></div>' +
    '<div class="tag" style="left:'+((L+H)/2)+'%">'+(width/1e6).toFixed(2)+' MHz span</div>' +
    // Naming each region in place, because a legend swatch does not say which
    // block of the picture it stands for.
    // A label is only drawn where it fits. A clipped word reads as a fault,
    // and the region still carries the whole sentence on hover.
    zone(0, L, "not received") + zone(H, 100-H, "not received") +
    zone(L, GL-L, "roll-off") + zone(GH, H-GH, "roll-off") +
    // Transparent strips carry the tooltips; the labels above ignore the mouse.
    '<div class="hit" style="left:0;width:'+L+'%" title="'+tipOut+'"></div>' +
    '<div class="hit" style="left:'+H+'%;width:'+(100-H)+'%" title="'+tipOut+'"></div>' +
    '<div class="hit" style="left:'+L+'%;width:'+(GL-L)+'%" title="'+tipRoll+'"></div>' +
    '<div class="hit" style="left:'+GH+'%;width:'+(H-GH)+'%" title="'+tipRoll+'"></div>' +
    '<div class="hit" style="left:'+GL+'%;width:'+(GH-GL)+'%" title="'+tipClear+'"></div>';
  el.title = tipSpan;
  // Two rows of labels, because carriers sit 25 kHz apart and their text does not.
  const lastAt = [-99,-99];
  for (const m of marks){
    const miss = !["carrier","control"].includes(m.role), px = x(m.hz);
    const bar = document.createElement("div");
    bar.className = "mark" + (m.role==="control"?" control":"") + (miss?" miss":"");
    bar.style.left = px+"%";
    // A control carrier decodes no voice frame of its own, so give it a height
    // that says "always on" instead of one that says "silent".
    bar.style.height = m.role==="control" ? "66px"
      : miss ? "36px" : (20 + Math.min(48, Math.log10(1+m.frames)*20))+"px";
    bar.title = m.hz + " Hz — " + (m.role==="control" ? "control carrier" :
      m.role==="carrier" ? m.frames+" frames" :
      m.role + (m.gssis.length? " — GSSI "+m.gssis.join(", ") : ""));
    el.appendChild(bar);
    const row = (px-lastAt[0] >= 5.5) ? 0 : (px-lastAt[1] >= 5.5) ? 1 : -1;
    if (row < 0) continue;            // no room; the tooltip still carries it
    lastAt[row] = px;
    const lb = document.createElement("div");
    lb.className = "lbl"; lb.style.left = px+"%"; lb.style.bottom = row? "3px":"15px";
    lb.textContent = mhz(m.hz);
    lb.style.color = miss ? "var(--bad)" : m.role==="control" ? "var(--ctl)" : "";
    el.appendChild(lb);
  }
  const out = marks.filter(m=>!m.in_span).length;
  sc.innerHTML = '<span>' + mhz(s.low) + ' MHz</span>' +
    '<span>centre <b>' + mhz(s.center) + ' MHz</b>' +
    (out? ' &nbsp; <span style="color:var(--bad)">' + out +
          ' granted outside the span</span>' : '') + '</span>' +
    '<span>' + mhz(s.high) + ' MHz</span>';
}

async function load(){
  let d; try { d = await (await fetch("/api/overview")).json(); }
  catch(e){ document.getElementById("sub").textContent = "cannot reach the server"; return; }
  if (d.error){ document.getElementById("sub").textContent = d.error; return; }
  const h = d.health || {};
  document.getElementById("sub").textContent =
    "run " + d.run + "   started " + (d.start_utc||"?") + "   " +
    (d.iq_rate? (d.iq_rate/1e6).toFixed(1)+" MS/s":"") + "   updated " + d.generated;
  const drop = h.dropped|0;
  const peak = h.peak_queue|0;
  const cells = [
    ["recording for", dur(h.seconds||0), "",
     "How long this run has been going, counted from the samples the dongle "+
     "delivered, not from the wall clock."],
    // Dropped is a running total and means IQ was lost. Queue is the warning
    // that comes first: the peak is of the whole run, the other of this second.
    ["dropped", drop, drop? "bad":"ok",
     "Blocks of radio the dongle produced that this program could not take in "+
     "time. Every one is lost audio that no later fix recovers. It is a running "+
     "total for the run and it must stay at zero."],
    ["queue now/peak", (h.queue|0)+"/"+peak, peak>40? "bad" : peak? "warn":"ok",
     "Blocks waiting to be processed: this second, and the highest this run has "+
     "reached. The queue fills before anything is dropped, so a rising peak is "+
     "the warning that arrives first. It holds one second by default, 400 "+
     "blocks, which --queue-blocks sets."],
  ];
  if (h.temp_c != null)
    cells.push(["cpu temp", h.temp_c+"\u00b0C", h.temp_c>=80? "bad" : h.temp_c>=70? "warn":"ok",
     "A host that gets hot slows itself down, and a slowed host starts dropping. "+
     "A Raspberry Pi 5 begins to throttle near 80 \u00b0C."]);
  if (h.steps) cells.push(["clock steps", h.steps, "warn",
     "Times the wall clock jumped while recording, which is usually NTP "+
     "correcting it. The map from a position in a recording to UTC re-anchors "+
     "at each one, and clock.log marks that row STEP."]);
  cells.push(
    ["talkgroups", d.talkgroups.length, "",
     "Distinct talkgroups (GSSI) with recorded speech in this run. Each one has "+
     "its own WAV file."],
    ["carriers", d.carriers.length, "",
     "Carriers that have decoded at least one voice frame in this run."],
    ["learned", d.learned.length, "",
     "Carriers discovered: a control carrier granted a call on them, "+
     "and a free slot of the pool tuned to them and stayed."],
    ["not recorded", d.missed_total, d.missed_total? "warn":"ok",
     "Clear calls this run could not take, because no slot held that carrier, or "+
     "the carrier lies outside the span. This is a coverage problem, not a "+
     "performance one: see the list at the foot of the page for the reason of "+
     "each. Dropped is the number to watch for performance."]);
  document.getElementById("strip").innerHTML = cells.map(c =>
    '<div class="cell" title="'+esc(c[3])+'">'+INFO+'<span>'+c[0]+'</span><b class="'+c[2]+
    '">'+c[1]+"</b></div>").join("");

  band(d);
  head("h-band", "Band covered",
    "What the dongle receives, drawn across frequency. The lighter block inside it is clear of the "+
    "roll-off, meaning inside 80% of Nyquist, where a carrier keeps its full "+
    "sensitivity; between that and the edge a carrier still decodes but loses "+
    "SNR. The dashed line is the centre. Blue marks a control carrier, green a "+
    "traffic carrier with its height by frames decoded, red a frequency a grant "+
    "named that no slot recorded. Hover any region or mark for its own detail.");
  head("h-tg", "Talkgroups",
    "One row for each talkgroup (GSSI) with recorded speech, and its own WAV. "+
    "Carriers lists every carrier it has been heard on, which is more than one "+
    "when the network moves a call. Radios lists the ISSIs that transmitted.");
  head("h-car", "Carriers",
    "Every frequency this run has seen, whether or not a slot could reach it. "+
    "Given means the command line named it; learned means a grant revealed it "+
    "and a free slot tuned to it. A row with in span \u201cno\u201d is a real "+
    "carrier this dongle cannot reach at the current centre.");
  head("h-miss", "Calls not recorded",
    "Clear calls that went out on air while no slot was on their carrier. This "+
    "is a coverage problem, not a performance one: nothing was dropped, the "+
    "program was simply not listening there. Each row says why.");

  tbl("tg", [
    {h:"GSSI", f:r=>esc(r.gssi)},
    {h:"audio", once:true, f:r=>r.has_audio?
      '<audio controls preload="none" src="/audio/'+esc(r.gssi)+'.wav"></audio>':"—"},
    {h:"speech", cls:"num", f:r=>dur(r.seconds)},
    {h:"frames", cls:"num", f:r=>r.frames},
    {h:"carriers", f:r=>r.carriers.map(c=>mhz(c.hz)).join(" ")},
    {h:"radios (ISSI)", f:r=>r.issis.map(i=>esc(i.issi)).join(" ")||"—"},
    {h:"first heard", f:r=>esc(hhmm(r.first_utc))},
    {h:"last heard", f:r=>esc(hhmm(r.last_utc))},
  ], d.talkgroups, "nothing recorded yet", r=>r.gssi);

  const learned = new Set(d.learned), pool = new Set(d.pool), ctl = new Set(d.controls);
  const heard = r => ["carrier","control"].includes(r.role);
  tbl("car", [
    {h:"frequency", f:r=>mhz(r.hz)+" MHz"},
    {h:"role", f:r=>ctl.has(r.hz)? '<span style="color:var(--ctl)">control</span>'
                  : heard(r)? "traffic" : "&mdash;"},
    {h:"source", f:r=>learned.has(r.hz)? "learned from a grant"
                    : (pool.has(r.hz)||heard(r))? "given"
                    : '<span class="bad">named by a grant, not followed</span>'},
    {h:"frames", cls:"num", f:r=>r.frames},
    {h:"in span", f:r=>r.in_span? '<span class="ok">yes</span>':'<span class="bad">no</span>'},
  ],
  // Everything the run has seen a frequency for, whether or not a slot could
  // reach it. BADFREQ is left out: a decode error is not a carrier.
  d.marks.filter(m=>m.role !== "BADFREQ"),
     "no carrier has decoded yet", r=>r.hz);

  document.getElementById("misscount").textContent =
    d.missed_total ? d.missed_total + (d.missed_total>d.missed.length? " (last "+d.missed.length+" shown)":"")
                   : "none";
  tbl("miss", [
    {h:"time", f:r=>esc(hhmm(r.utc))},
    {h:"GSSI", f:r=>esc(r.gssi)},
    {h:"radio", f:r=>esc(r.issi)},
    {h:"carrier", f:r=>mhz(r.hz)+" MHz"},
    {h:"granted by", f:r=>mhz(r.control_hz)+" MHz"},
    {h:"why", f:r=>({OUTSIDE:"outside the span — move --center or raise --rate",
                     BADFREQ:"the grant decoded wrong",
                     NOSLOT:"the carrier pool is full — raise --max-carriers",
                     UNTUNED:"no slot held it yet; one has taken it now"}[r.status]||r.status)},
  ], d.missed.slice().reverse(), "every granted call was recorded",
     r=>r.sample+"/"+r.hz+"/"+r.gssi);
}
load(); setInterval(load, 5000);
</script>
"""


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("--in", dest="recordings", default="recordings")
    p.add_argument("--bind", default="0.0.0.0")
    p.add_argument("--port", type=int, default=8080)
    a = p.parse_args()
    Handler.recordings = a.recordings
    srv = ThreadingHTTPServer((a.bind, a.port), Handler)
    srv.daemon_threads = True
    everywhere = a.bind in ("", "0.0.0.0")
    print(
        "tetra-sniff ui: reading %s%s"
        % (a.recordings, ", reachable on every network" if everywhere else ""),
        flush=True,
    )
    for addr in (local_addresses() if everywhere else [a.bind]) or [a.bind or "0.0.0.0"]:
        print("  http://%s:%d" % (addr, a.port), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
