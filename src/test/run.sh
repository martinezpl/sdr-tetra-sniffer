#!/bin/bash
# usage: src/test/run.sh                       synthetic gen_iq case
#        src/test/run.sh BASEBAND.wav HZ1 HZ2  real-RF acceptance checks
set -u
cd "$(dirname "$0")/../.."
BUILD=src/build
REC=./tetra-analyze
[ -d $BUILD ] || { echo "build first: ./build.sh"; exit 2; }
cmake --build $BUILD >/dev/null || exit 2
TMP=$(mktemp -d /tmp/tetra-analyze-test.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
fails=0
ctest --test-dir $BUILD --output-on-failure && echo "PASS unit tests" \
	|| { echo "FAIL unit tests"; fails=$((fails + 1)); }
pass() { echo "PASS $*"; }

# The page carries its JavaScript inside a Python string, so nothing compiles
# it and a broken edit reaches the browser as a blank page. One syntax check
# costs nothing and catches exactly that.
check_ui() {
	python3 - "$1" <<'EOF'
import re, sys
s = open("src/ui.py").read()
m = re.search(r"<script>(.*?)</script>", s, re.S)
if not m:
    print("no <script> block in src/ui.py"); sys.exit(1)
open(sys.argv[1], "w").write(m.group(1))
EOF
}
fail() { echo "FAIL $*"; fails=$((fails + 1)); }
run_rec() { "$REC" run "$@" 2>&1 | grep -v -e '^Ranking' -e '^\[Resamp\]' >&2; return "${PIPESTATUS[0]}"; }

python3 -c "import ast; ast.parse(open('src/ui.py').read())" 2>/dev/null \
	&& pass "ui.py parses" || fail "ui.py parses"
check_ui "$TMP/page.js"
if command -v node >/dev/null; then
	node --check "$TMP/page.js" 2>/dev/null && pass "ui.py page javascript parses" \
		|| { fail "ui.py page javascript"; node --check "$TMP/page.js" 2>&1 | head -4; }
else
	echo "SKIP ui javascript: node is not installed"
fi

# The last anchor plus 480 per later calls.log line must equal the real WAV size.
timemap_check() {
	python3 - "$1" <<'EOF'
import os, sys
d = sys.argv[1]
a = {}
for ln in open(d + '/timemap.log'):
    if not ln.startswith('#'):
        c = ln.split(); a[c[2]] = (c[0], c[1], int(c[3]))
n = dict.fromkeys(a)
for ln in open(d + '/calls.log'):
    if ln.startswith('#'): continue
    c = ln.split(); g = c[3]
    if g not in a: continue
    if n[g] is not None: n[g] += 1
    if (c[0], c[1]) == a[g][:2]: n[g] = 0
bad = 0
for g in sorted(a):
    want = (os.path.getsize(d + '/calls/' + g + '.wav') - 44) // 2
    got = a[g][2] + 480 * n[g]
    if got != want: print('BAD %s %d != %d' % (g, got, want)); bad += 1
print('info timemap: %d gssi checked, %d bad' % (len(a), bad))
# No clear voice means nothing to check. Do not report that as a pass.
sys.exit(2 if not a else 1 if bad else 0)
EOF
}

check_wavs() {
	python3 - "$@" <<'EOF'
import os, struct, sys, wave
bad = 0
for p in sys.argv[1:]:
    size = os.path.getsize(p)
    with open(p, 'rb') as f:
        h = f.read(44)
    riff, data = struct.unpack('<I', h[4:8])[0], struct.unpack('<I', h[40:44])[0]
    with wave.open(p) as w:
        ok = (w.getframerate(), w.getnchannels(), w.getsampwidth()) == (8000, 1, 2) and w.getnframes() * 2 == data
    ok = ok and riff == 36 + data and size == 44 + data
    print(('ok ' if ok else 'BAD ') + p + ' samples=' + str(data // 2))
    bad += not ok
sys.exit(1 if bad else 0)
EOF
}

if [ $# -eq 0 ]; then
	RATE=288000; CENTER=419340000
	ON1=419240000; ON2=419415000; OFF=419365000
	$BUILD/gen_iq --rate $RATE --seconds 6 --out "$TMP/iq.cf32" $((ON1 - CENTER)) $((ON2 - CENTER)) || exit 2

	run_rec --iq "$TMP/iq.cf32" --fmt cf32 --rate $RATE --center $CENTER --per-carrier --out "$TMP/a" $ON1 $ON2 $OFF
	[ $? -eq 0 ] && pass "exit 0 on EOF" || fail "exit status on EOF"
	if grep -qE " (LOCK|UNLOCK) " "$TMP"/a/*/*.log; then fail "LOCK/UNLOCK still logged"; else pass "no LOCK/UNLOCK"; fi
	for hz in $ON1 $ON2 $OFF; do
		[ -s "$TMP"/a/*/$hz.log ] && pass "log for $hz" || fail "missing log for $hz"
	done

	(cat "$TMP/iq.cf32"; sleep 4) | "$REC" run --iq - --fmt cf32 --rate $RATE --center $CENTER --per-carrier --out "$TMP/b" $ON1 $ON2 >"$TMP/b.err" 2>&1 &
	pid=$!
	sleep 1.5
	kill -INT "$pid"
	wait "$pid"
	[ $? -eq 0 ] && pass "exit 0 on SIGINT" || { fail "exit status on SIGINT"; grep -v -e '^Ranking' -e '^\[Resamp\]' "$TMP/b.err"; }
	[ -s "$TMP"/b/*/$ON1.log ] && pass "log before SIGINT" || fail "no log before SIGINT"
	(cat "$TMP/iq.cf32"; sleep 4) | "$REC" run --iq - --fmt cf32 --rate $RATE --center $CENTER --out "$TMP/c" $ON1 $ON2 >"$TMP/c.err" 2>&1 &
	pid=$!
	sleep 1.5
	kill -INT "$pid"
	wait "$pid"
	[ $? -eq 0 ] && [ -s "$TMP"/c/*/calls.log ] && pass "calls-only SIGINT flush" || fail "calls-only SIGINT flush"
	timemap_check "$TMP"/a/*/; rc=$?
	case $rc in
	0) pass "timemap anchors reproduce every WAV size" ;;
	2) echo "SKIP timemap: the synthetic case carries no clear voice (run with a real baseband WAV)" ;;
	*) fail "timemap anchors" ;;
	esac
	# A dead stitch must end the run at once, not at the end of the input. Without
	# this the tree stays alive and records nothing for the whole remaining input.
	(cat "$TMP/iq.cf32"; sleep 10) | "$REC" run --iq - --fmt cf32 --rate $RATE --center $CENTER --out "$TMP/d" $ON1 $ON2 >"$TMP/d.err" 2>&1 &
	pid=$!
	sleep 1.5
	kill -9 $(pgrep -P "$pid" | head -1)   # the stitch process forks first
	waited=0
	while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt 30 ]; do sleep 0.1; waited=$((waited + 1)); done
	wait "$pid"; rc=$?
	[ "$waited" -lt 30 ] && [ "$rc" -ne 0 ] && pass "a dead stitch ends the run in ${waited}00 ms" \
		|| fail "a dead stitch ends the run (waited ${waited}00 ms, exit $rc)"
	# The day rotation restarts across midnight: a run started on one UTC date
	# is cut, and the new run starts on the next date. The new clock.log must
	# still name the earlier run, so the cut reads as a known gap, not silence.
	run_rec --iq "$TMP/iq.cf32" --fmt cf32 --rate $RATE --center $CENTER \
		--start-utc 2026-09-10T23:59:00Z --out "$TMP/e" $ON1 $ON2 >/dev/null
	run_rec --iq "$TMP/iq.cf32" --fmt cf32 --rate $RATE --center $CENTER \
		--start-utc 2026-09-11T00:00:00Z --out "$TMP/e" $ON1 $ON2 >/dev/null
	if ! grep -q PREV "$TMP"/e/2026-09-10T235900Z/clock.log &&
	   grep -q "PREV run=2026-09-10T235900Z" "$TMP"/e/2026-09-11T000000Z/clock.log; then
		pass "a run across midnight names the run from before it"
	else
		fail "PREV line across a midnight restart"
	fi
	check_wavs "$TMP"/a/*/*.wav "$TMP"/b/*/*.wav && pass "WAV headers consistent" || fail "WAV headers"
else
	WAV=$1; HZ1=$2; HZ2=$3
	run_rec --iq "$WAV" --per-carrier --out "$TMP/both" "$HZ1" "$HZ2" || fail "exit status (both)"
	run_rec --iq "$WAV" --per-carrier --out "$TMP/alone" "$HZ1" || fail "exit status (alone)"
	B=$(echo "$TMP"/both/*); A=$(echo "$TMP"/alone/*)
	check_wavs "$B"/*.wav "$A"/*.wav && pass "WAV headers consistent" || fail "WAV headers"

	cat "$B"/*.log | awk '!/^#/ && ($2 == 16777215 || $3 == 16777215) { bad++ } END { exit bad > 0 }' && pass "no dummy SSI" || fail "dummy SSI 16777215 logged"
	cat "$B"/*.log | awk '!/^#/ && $4 == "PLAY" && $8 != 0 { bad++ } END { exit bad > 0 }' && pass "every PLAY has encr 0" || fail "PLAY with encr != 0"
	for hz in $HZ1 $HZ2; do
		n=$(grep -c " PLAY " "$B/$hz.log")
		echo "info $hz: PLAY lines $n"
	done

	cmp -s "$B/$HZ1.wav" "$A/$HZ1.wav" && pass "codec isolation: $HZ1.wav identical alone vs alongside $HZ2" || fail "$HZ1.wav differs alone vs alongside"
	cmp -s "$B/$HZ1.log" "$A/$HZ1.log" && pass "$HZ1.log identical alone vs alongside" || fail "$HZ1.log differs alone vs alongside"

	awk -v h1="$HZ1" -v h2="$HZ2" '
		!/^#/ && $4 == "PLAY" { open[$5, $3] = $12 }
		!/^#/ && $4 == "END" && (($5, $3) in open) { n = ++cnt[$5]; s[$5, n] = open[$5, $3]; e[$5, n] = $12; gssi[$5, n] = $3; delete open[$5, $3] }
		END {
			for (i = 1; i <= cnt[h1]; i++) for (j = 1; j <= cnt[h2]; j++)
				if (gssi[h1, i] == gssi[h2, j] && s[h1, i] < e[h2, j] && s[h2, j] < e[h1, i]) {
					printf "info GSSI %s plays on both Hz at iq_s %.3f..%.3f / %.3f..%.3f\n", gssi[h1, i], s[h1, i], e[h1, i], s[h2, j], e[h2, j]; found = 1
				}
			exit !found
		}' "$B/$HZ1.log" "$B/$HZ2.log" && pass "same GSSI PLAY on both Hz at the same time" || echo "NOT OBSERVED: same GSSI on both Hz at the same time"

	awk -v h1="$HZ1" -v h2="$HZ2" '
		!/^#/ && $4 == "REPLACE" && $10 == h1 + h2 - $5 { rep[++nr] = $12; rgssi[nr] = $3; rto[nr] = $10 }
		!/^#/ && $4 == "PLAY" { np++; pt[np] = $12; pgssi[np] = $3; phz[np] = $5 }
		END {
			for (i = 1; i <= nr; i++) for (j = 1; j <= np; j++)
				if (phz[j] == rto[i] && pgssi[j] == rgssi[i] && pt[j] >= rep[i] && pt[j] <= rep[i] + 2) {
					printf "info REPLACE GSSI %s -> %s at iq_s %.3f, PLAY there at %.3f\n", rgssi[i], rto[i], rep[i], pt[j]; found = 1
				}
			exit !found
		}' "$B/$HZ1.log" "$B/$HZ2.log" && pass "REPLACE followed by PLAY on the target Hz" || echo "NOT OBSERVED: REPLACE to the sibling Hz followed by PLAY there"
	echo "recordings kept in $TMP"; trap - EXIT
fi
echo "$fails failure(s)"
exit $((fails > 0))
