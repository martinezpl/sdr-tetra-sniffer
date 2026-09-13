#!/usr/bin/env python3
"""Map a position in a per-GSSI WAV file to UTC, and UTC back to a position.

The recorder writes three files in each run directory:

  calls.log    one line for each decoded voice frame, 480 WAV samples each
  timemap.log  an anchor wherever the WAV moves by more than those 480
  clock.log    UTC against the VFO sample counter, one line each second

Between two anchors the position is exact arithmetic, not an interpolation:
480 samples for each calls.log line of that GSSI. A separator and a
concealment run write samples but no calls.log line, so each one carries an
anchor. Only the last step, a sample count to UTC, interpolates, and only
inside one second of clock.log.

usage:
  timemap.py RUNDIR wav GSSI BYTE_OFFSET     a byte offset in the WAV -> UTC
  timemap.py RUNDIR utc GSSI ISO_UTC         a UTC time -> byte offset
  timemap.py --selftest
"""

import bisect
import calendar
import os
import sys
import time

FRAME = 480       # WAV samples for each calls.log line. See stitch.cpp emit_frame.
HDR = 44          # bytes of WAV header before the samples. See recorder.cpp.
RATE = 8000.0     # WAV sample rate
MAX_GAP = 2.0     # The parent writes clock.log once a second. A larger gap is a stall.


def parse_iso(s):
    """Read 2026-09-11T04:07:03.412Z as seconds since the epoch."""
    s = s.rstrip("Z")
    whole, _, frac = s.partition(".")
    t = calendar.timegm(time.strptime(whole, "%Y-%m-%dT%H:%M:%S"))
    return t + (float("0." + frac) if frac else 0.0)


def iso(t):
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(t)) + ".%03dZ" % (t % 1 * 1000)


def fields(path):
    """Give the tokens of each data line. A comment line starts with #."""
    with open(path) as f:
        for line in f:
            if not line.startswith("#"):
                tok = line.split()
                if tok:
                    yield tok


def frames(rundir, gssi):
    """Every frame of one GSSI, as (wav_start, wav_end, capture_sample).

    An anchor holds the WAV position after its own frame, so the frame always
    occupies the last 480 samples before it. A gap in front of wav_start is a
    call separator or a concealment run, which carries no calls.log line.
    """
    anchors = {}
    for t in fields(os.path.join(rundir, "timemap.log")):
        if len(t) >= 5 and t[2] == gssi:
            anchors[(t[0], t[1])] = int(t[3])   # (capture_sample, tdma_frame) -> wav
    out, pos = [], 0
    for t in fields(os.path.join(rundir, "calls.log")):
        if len(t) < 4 or t[3] != gssi:
            continue
        end = anchors.get((t[0], t[1]), pos + FRAME)
        out.append((end - FRAME, end, int(t[0])))
        pos = end
    return out


def clock(rundir):
    """Every clock.log row, as (utc, vfo_sample, stepped)."""
    rows = []
    for t in fields(os.path.join(rundir, "clock.log")):
        if len(t) >= 5:
            rows.append((parse_iso(t[0]), int(t[1]), t[4] == "STEP"))
    return rows


def sample_to_utc(rows, sample):
    """Give one time, or an interval when the counter did not advance.

    A stall shows as clock.log rows that gain no samples. The honest answer for
    a sample inside such a gap is the whole interval, never a single time.
    """
    if not rows:
        return ("none",)
    caps = [r[1] for r in rows]
    lo = bisect.bisect_left(caps, sample)
    hi = bisect.bisect_right(caps, sample) - 1
    if lo <= hi:
        # The counter held this value across rows lo..hi. A stall makes lo < hi.
        if lo == hi:
            return ("at", rows[lo][0])
        return ("between", rows[lo][0], rows[hi][0])
    if lo == 0:
        return ("before", rows[0][0])
    if lo == len(rows):
        return ("after", rows[-1][0])
    a, b = rows[lo - 1], rows[lo]
    if b[2] or b[0] - a[0] > MAX_GAP:
        # The clock stepped, or rows are absent. An interval is the honest answer.
        return ("between", a[0], b[0])
    return ("at", a[0] + (b[0] - a[0]) * (sample - a[1]) / (b[1] - a[1]))


def utc_to_sample(rows, want):
    """The VFO sample count at one UTC time, or None outside the log."""
    for a, b in zip(rows, rows[1:]):
        if a[0] <= want <= b[0]:
            if b[0] == a[0] or b[1] == a[1]:
                return a[1]        # the counter stalled and read this value
            return a[1] + (b[1] - a[1]) * (want - a[0]) / (b[0] - a[0])
    return None


def render(answer):
    if answer[0] == "at":
        return iso(answer[1])
    if answer[0] == "between":
        return "between %s and %s" % (iso(answer[1]), iso(answer[2]))
    if answer[0] == "none":
        return "no clock.log rows"
    return "%s %s" % (answer[0], iso(answer[1]))


def do_wav(rundir, gssi, byte):
    sample = (byte - HDR) // 2
    if sample < 0:
        return "byte %d sits inside the 44-byte WAV header" % byte
    rows = clock(rundir)
    for start, end, capture in frames(rundir, gssi):
        if sample < start:
            return "silence before the frame at %s (WAV %.3f s, no calls.log line here)" % (
                render(sample_to_utc(rows, capture)), sample / RATE)
        if sample < end:
            return "%s (WAV %.3f s, capture_sample %d)" % (
                render(sample_to_utc(rows, capture)), sample / RATE, capture)
    return "byte %d is past the end of the GSSI %s recording" % (byte, gssi)


def do_utc(rundir, gssi, text):
    want = utc_to_sample(clock(rundir), parse_iso(text))
    if want is None:
        return "%s lies outside the span that clock.log covers" % text
    for start, end, capture in frames(rundir, gssi):
        if capture >= want:
            return "byte %d (WAV %.3f s, capture_sample %d)" % (
                HDR + start * 2, start / RATE, capture)
    return "GSSI %s records nothing at or after %s" % (gssi, text)


def selftest():
    import tempfile
    d = tempfile.mkdtemp()
    # Two calls for GSSI 777. Frame 3 follows a separator, frame 4 follows a
    # 2-frame concealment run. A second GSSI proves the filter works.
    open(os.path.join(d, "calls.log"), "w").write(
        "# capture_sample tdma_frame ISSI GSSI carrier_hz control_hz TN usage status\n"
        "36000 1 1 777 418962500 419162500 1 17 FRAME\n"
        "72000 2 1 777 418962500 419162500 1 17 FRAME\n"
        "252000 9 1 777 418962500 419162500 1 17 FRAME\n"
        "288000 12 1 777 418962500 419162500 1 17 FRAME\n"
        "324000 13 1 888 418962500 419162500 1 17 FRAME\n")
    open(os.path.join(d, "timemap.log"), "w").write(
        "# capture_sample tdma_frame GSSI wav_sample event\n"
        "36000 1 777 480 START\n"
        "252000 9 777 5440 SEP\n"
        "288000 12 777 6880 FILL\n")
    open(os.path.join(d, "clock.log"), "w").write(
        "# tetra-sniff run=x start_utc=y iq_rate=3200000 vfo_rate=36000\n"
        "# utc vfo_sample queue dropped event\n"
        "2026-09-11T00:00:01.000Z 36000 0 0 OK\n"
        "2026-09-11T00:00:02.000Z 72000 0 0 OK\n"
        "2026-09-11T00:00:03.000Z 108000 0 0 OK\n"
        "2026-09-11T00:00:04.000Z 108000 0 0 OK\n"    # the dongle delivers nothing
        "2026-09-11T00:00:05.000Z 108000 0 0 OK\n"    # and still nothing
        "2026-09-11T00:00:06.000Z 144000 0 0 OK\n"
        "2026-09-11T00:00:07.000Z 180000 0 0 OK\n"
        "2026-09-11T00:00:08.000Z 216000 0 0 OK\n"
        "2026-09-11T00:00:09.000Z 252000 0 0 OK\n"
        "2026-09-11T00:00:10.000Z 288000 0 0 OK\n")

    # Rule 1: the anchors place the separator and the concealment run exactly.
    f = frames(d, "777")
    assert f == [(0, 480, 36000), (480, 960, 72000),
                 (4960, 5440, 252000), (6400, 6880, 288000)], f
    # Rule 2: the last anchor is the file size, so the accounting is complete.
    assert f[-1][1] == 6880
    # The other GSSI is not mixed in.
    assert frames(d, "888") == [(0, 480, 324000)]

    rows = clock(d)
    # A sample inside a normal second gives one time.
    answer = sample_to_utc(rows, 54000)
    assert answer[0] == "at" and abs(answer[1] - parse_iso("2026-09-11T00:00:01.500Z")) < 1e-6
    # The stalled sample count is an interval, never a false-precise point.
    answer = sample_to_utc(rows, 108000)
    assert answer[0] == "between", answer
    assert answer[1] == parse_iso("2026-09-11T00:00:03Z"), answer
    assert answer[2] == parse_iso("2026-09-11T00:00:05Z"), answer

    # A round trip returns the same frame, within one sample.
    byte = HDR + 480 * 2
    out = do_wav(d, "777", byte)
    assert "2026-09-11T00:00:02" in out, out
    assert ("byte %d " % byte) in do_utc(d, "777", "2026-09-11T00:00:02.000Z")

    # Inserted audio is named as silence, not as a frame.
    assert "silence" in do_wav(d, "777", HDR + 3000 * 2)
    assert "past the end" in do_wav(d, "777", HDR + 99999 * 2)
    print("selftest: every check passes")


def main(argv):
    if len(argv) == 2 and argv[1] == "--selftest":
        selftest()
        return 0
    if len(argv) != 5 or argv[2] not in ("wav", "utc"):
        sys.stderr.write(__doc__)
        return 2
    run, mode, gssi, value = argv[1:5]
    print(do_wav(run, gssi, int(value)) if mode == "wav" else do_utc(run, gssi, value))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
