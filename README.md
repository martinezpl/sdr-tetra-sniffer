# sdr-tetra-sniffer

- [Prerequisites](#prerequisites)
  - [SDR receiver](#sdr-receiver)
  - [Host](#host)
  - [Headroom](#headroom)
  - [Packages](#packages)
  - [Active TETRA network in range](#active-tetra-network-in-range)
- [Installation](#installation)
  - [Third party dependencies](#third-party-dependencies)
- [Usage](#usage)
  - [Finding carriers](#finding-carriers)
  - [The carrier pool](#the-carrier-pool)
  - [Core options](#core-options)
  - [Output](#output)
  - [UI](#ui)
  - [Run it as a service](#run-it-as-a-service)
- [Architecture](#architecture)
  - [Parent — the radio consumer](#parent--the-radio-consumer)
  - [Carrier child, one for each slot — demodulation](#carrier-child-one-for-each-slot--demodulation)
  - [The carrier pool — why a sweep only needs the control carriers](#the-carrier-pool--why-a-sweep-only-needs-the-control-carriers)
  - [The shared allocation table](#the-shared-allocation-table)
  - [Stitch — the demultiplexer](#stitch--the-demultiplexer)
  - [Talkgroup worker, one for each talkgroup — speech and files](#talkgroup-worker-one-for-each-talkgroup--speech-and-files)
  - [Why processes and not threads](#why-processes-and-not-threads)
  - [Layout](#layout)
  - [Tests](#tests)
- [To be optimized](#to-be-optimized)

## Prerequisites

### SDR receiver 
The sniffer was developed against an RTL-SDR Blog V4 with an R828D tuner.
It opens the receiver through SoapySDR, so any device with a Soapy module
works. You'll need the module for your receiver, auto-detect will discover the connected
device and tell you what to install.
USRP is not auto-detected. Without `soapysdr-module-uhd` it stays `no SDR found`.

For a receiver with no module, the IQ feed can be piped in instead:

```
<your sdr tool writing IQ to stdout> \
  | ./tetra-sniff run --iq - --carriers ...
```

Give `--fmt`, `--rate` and `--center` to match what the tool produces.

### Host 
Any POSIX system with a C++17 compiler. Windows is not supported,
because the program is a process tree built on `fork`, pipes, POSIX file
locks and shared memory.

| Platform | State |
| --- | --- |
| Debian 13 (trixie) aarch64, Raspberry Pi 5 | verified, the reference target |
| macOS arm64 (Darwin 25) | verified |
| Other Linux, x86-64 or ARM | expected to work, not tested |
| FreeBSD and the other BSDs | expected to work, not tested |
| Windows | not supported |

### Headroom 
A Raspberry Pi 5 runs 15 carriers at 3.2 MS/s in about one of
its four cores, at 92 MB for the whole tree.

### Packages

```
sudo apt install build-essential cmake git curl unzip libvolk-dev libsoapysdr-dev  # Debian, Raspberry Pi OS
brew install cmake volk soapysdr                                                              # macOS
```

Soapy modules:

| Receiver | Debian | Homebrew |
| --- | --- | --- |
| RTL-SDR | `soapysdr-module-rtlsdr` | `soapyrtlsdr` |
| HackRF | `soapysdr-module-hackrf` | `soapyhackrf` |
| Airspy | `soapysdr-module-airspy` | `soapyairspy` (Pothos tap if brew-core does not have it) |
| bladeRF | `soapysdr-module-bladerf` | not in brew-core |
| LimeSDR | `soapysdr-module-lms7` | `limesuite` |
| USRP | `soapysdr-module-uhd` | not in brew-core |
| Pluto | `soapysdr-module-plutosdr` | not in brew-core |
| SDRplay | `soapysdr-module-sdrplay` | not in brew-core |

CMake 3.16 or later is needed. `curl`, `unzip` and `patch` must be on the
`PATH` for the codec step of the build. That step on macOS also needs
`md5sum`; install `coreutils` if `/sbin/md5sum` is absent.

### Active TETRA network in range
`sweep` runs a scan for active control carriers across the TETRA spectrum, and the traffic carriers are then learned from the grants that the
control carriers broadcast. See "Find your carriers" below.

## Installation

```
git clone --recurse-submodules git@github.com:martinezpl/sdr-tetra-sniffer.git
cd sdr-tetra-sniffer
./build.sh
./tetra-sniff help
```

`build.sh` checks out the submodules, fetches the speech codec, and builds
`./tetra-sniff` at the top of the repository.

### Third party dependencies

- **The ETSI ACELP speech codec.** ETSI source cannot be redistributed, 
  so `build.sh` downloads it from ETSI and applies the patches that make it build on a 64-bit host. 
  This needs a network connection one time.
- **The SDR++ core headers**, as the submodule `third_party/sdrpp`, pinned to
  one commit. Only the headers are used. Nothing of SDR++ is compiled, linked
  or installed. To build against a copy that you already have, pass
  `./build.sh -DSDRPP_CORE_ROOT=<sdrpp>/core`, and the submodule is then not
  fetched at all.

## Usage

```
./tetra-sniff help    # every option, with its default
./tetra-sniff sweep   # find the control channels and active carriers
./tetra-sniff run --carriers <control carriers>   # record
```

`run` exposes every setting as a flag. Ctrl-C, SIGTERM, or the end of the
input stops a run and closes the files cleanly. Logs go to stdout. A live
run prints `radio rtlsdr 0`, or the Soapy driver key of the device that
opened.

```
nohup ./tetra-sniff run --carriers 419162500,419562500 \
  > tetra-sniff.log 2>&1 < /dev/null &
```

### Finding carriers

`sweep` measures every channel of the raster, then puts a demodulator on
each peak. It prints a ready `run` command line:

```
$ ./tetra-sniff sweep
Found Rafael Micro R828D tuner
RTL-SDR Blog V4 Detected
tetra-sniff: the receiver takes 3.200 MS/s, so a span is 3.170 MHz
tetra-sniff: sweep 380.000-430.000 MHz, 12.5 kHz raster, 3.2 MS/s, 17 span(s)
tetra-sniff: span 1 at 381.585 MHz, 254 channels
tetra-sniff: span 2 at 384.596 MHz, 241 channels
[...]
tetra-sniff: 41 peak(s) at or above 6.0 dB over the noise floor
tetra-sniff: 7 span(s) to decode
tetra-sniff: span 1 of 7, decode 2 candidate(s) at 385.280 MHz for 8 s
[...]
tetra-sniff: span 7 of 7, decode 3 candidate(s) at 427.240 MHz for 8 s

  channel       SNR dB  role                     cell main carrier  error Hz
  385002500        7.0  no lock, not TETRA          -                  -
  385300000        21.8  TETRA traffic carrier    385300000          -143
  389750000        22.0  TETRA control carrier    389750000          -217
  [...]
  

20 TETRA carrier(s), of which 7 are a control carrier.

The receiver is off by about -0.43 ppm, from 9 carrier(s) above the noise that
held lock, spread -1.03 to +0.20 ppm. Each run below carries the
offset in Hz at its own centre, because the error scales with it.
That spread is wide for one oscillator. A carrier of its own may
sit off frequency, or a weak one may not have settled. A longer
--dwell tightens it.

Those carriers do not fit one span at 3.2 MS/s, so they need 2 runs,
one for each span. A receiver hears one span at a time, so running them
at once needs one receiver for each, named with --device.

  # span 1 of 2, 9 carrier(s), 4 control

  ./tetra-sniff run --center [...] \
      --tune-offset -166 \
      --rate 3200000 \
      --carriers [...]

  # span 2 of 2, 9 carrier(s), 3 control

  ./tetra-sniff run --center [...] \
      --tune-offset -180 \
      --rate 3200000 \
      --carriers [...]

2 carrier(s) sit too far from any control carrier to share a span with
one, so nothing would grant them: [...]
They are traffic carriers of a cell whose control carrier the sweep did
not find. Widen --band or raise --dwell to look for it.

A power scan sees a carrier only while it transmits, so an idle traffic
carrier is missing from that list. "run" reports each grant that names a
carrier the list does not hold, so watch its log and add what it names.
```

With `--carriers` and no `--center`, the receiver tunes to the midpoint of
your list.

- **Every control carrier is mandatory.** Every channel grant is broadcast on
  one, and a grant is how the program learns that a call is starting, on which
  carrier, for which talkgroup, and whether it is in the clear.
- **Traffic carriers are optional.** A free slot of the pool tunes to each one
  the moment a grant names it, so the list fills itself. Naming a traffic
  carrier you already know is a convenience: a slot is on it before its first
  call starts, so the head of that call is not lost to the time a retune takes.

`sweep` tells the two apart: a carrier whose system information names itself
as the main carrier of its cell is a control carrier.

### The carrier pool

`--carriers` seeds a pool of `--max-carriers` slots, 15 by default. Each
spare slot waits. When a control carrier grants a clear call on a carrier
that no slot follows, the parent gives a free slot that frequency and keeps
it there. A slot is never taken back: a carrier the network used once it will
use again, and a retune costs the head of a call.

So a sweep only has to find the control carriers. The rest fills itself:

```
tetra-sniff: slot 5 takes 420362500 Hz, granted to GSSI 1002 by 419562500 Hz
```

The learned carriers go to `DIR/carriers` beside the recordings, and the next
run reads them back, so a restart at the UTC day boundary does not learn them
all over again. `--no-learn` turns that file off.

### Core options

| Flag | Default | What it does |
| --- | --- | --- |
| `--carriers LIST` | **required** | Downlink carriers in Hz, separated by commas. Must hold every control carrier |
| `--center HZ` | midpoint of `--carriers` | Dongle centre frequency. Give one only to leave room on one side for a carrier not yet known |
| `--rate HZ` | 3200000 | Dongle sample rate, and so the width of the span. |
| `--gain DB` | auto | Tuner gain, or `auto` |
| `--tune-offset HZ` | 0 | Correction for the frequency error of your receiver. `sweep` measures it |
| `--out DIR` | `recordings` | Directory for recordings |
| `--max-carriers N` | 15 | Size of the carrier pool. Spare slots learn carriers from grants |
| `--per-carrier` | off | Also write one WAV and one log for each carrier. Costs much more CPU, and it fixes the carrier list |
| `--iq FILE` | — | Replay a capture instead of opening the dongle |

### Output

Each run makes one directory, `recordings/<start_utc>/`:

| File | Content |
| --- | --- |
| `calls/<GSSI>.wav` | The speech of one talkgroup. 8 kHz mono s16, silence removed |
| `calls.log` | One line for each decoded voice frame, and one for each call that could not be recorded |
| `timemap.log` | Anchors that tie a position in a WAV to a capture sample |
| `clock.log` | UTC against the sample counter, one line each second |

`clock.log` is the health record of a run. Its `queue` and `dropped` columns
must stay at zero; anything else means the host cannot keep up with the
dongle.

Because silence is removed, a position in a WAV is not a wall-clock time on
its own. `src/timemap.py` converts between the two with the anchors of
that run:

```
src/timemap.py RUNDIR wav 1001 1048576      # byte offset -> UTC
src/timemap.py RUNDIR utc 1001 2026-09-12T15:20:00Z   # UTC -> byte offset
```

### UI

`src/ui.py` serves one page that shows what a run is doing: health, the
band it covers, every talkgroup it has recorded with its audio, the carriers
it follows, and every call it could not take.

```
$ python3 src/ui.py --in recordings --port 8080
tetra-sniff ui: reading recordings, reachable on every network
  http://10.42.0.1:8080
  http://192.168.1.201:8080
  http://127.0.0.1:8080
```

It prints every address it answers on. `--bind` narrows that to one, and
there is no authentication, so that choice is the only access control.

It reads only the files that a run writes, so it can be started, stopped or edited while a capture is going on,
and it never touches the recorder. Python 3 alone, no dependencies.


### Run it as a service

The program sends `READY=1` and `WATCHDOG=1` to `$NOTIFY_SOCKET`, so
`Type=notify` and `WatchdogSec` work in a systemd unit with no wrapper.

Give `TasksMax` room for the whole tree: `--max-gssi` talkgroup writers, 256
by default, plus one process for each carrier, plus the parent and the stitch
process. A `fork()` above `TasksMax` throws in the stitch process, which ends the run and logs
`tetra-sniff: stitch exited 134`.

## Architecture

```
dongle ────── IQ──▶ PARENT
                       │  N pipes
                       ▼
                  CARRIER CHILD ×N  ◀──▶ shared allocation table
                       │  one voice pipe, coded bits
                       ▼
                     STITCH
                       │  one pipe for each talkgroup
                       ▼
              TALKGROUP WORKER ×N ──▶ calls/<GSSI>.wav
```

### Parent — the radio consumer
It owns the receiver. A reader thread takes blocks from it into a bounded queue; 
an overflow drops the newest block and counts it in `clock.log`. The main loop converts each block
to complex float and runs one channelizer for each carrier. A channelizer
shifts its carrier down to baseband and lowers the sample rate, so a child
works on one narrow stream instead of the whole span. The result goes down
that carrier's pipe. The parent also writes `clock.log`
and answers the systemd watchdog. It is the only process that touches the
full-rate stream, so it costs far more than any other.

### Carrier child, one for each slot — demodulation
It reads its stream and runs π/4-DQPSK demodulation, symbol extraction, bit unpacking and
the TETRA burst decoder. It sends raw coded voice frames to the stitch
process, and it publishes and reads channel grants in the shared table. By
default it never decodes speech; `--per-carrier` is what turns that on.

A child reads its own slot in the shared table once for each block. When the
frequency there changes, it throws the whole decoder away and builds a new
one. That is deliberate: the decoder keeps state in the display state, the
crypto state and the fragment slots, and one stale field is enough to
suppress playback for the rest of the run, so a rebuild is the only reset
that cannot miss one.

The parent feeds every slot, free or not, so all children count the same
samples. Grants are stamped with that count, so a slot filled an hour into a
run still reads them on the same timebase.

### The carrier pool — why a sweep only needs the control carriers
A free slot holds no frequency. When a child on a control carrier decodes a grant
for a clear call on a carrier that no slot follows, it writes the frequency
into a small queue in the shared table. Once for each input block the parent
empties that queue, gives each frequency a free slot, and moves that slot's
channelizer onto it. A slot
is never taken back, so each carrier costs one lock time ever — about 0.2 to
1.7 s, which is the head of that first call.

The span is the hard limit. A grant for a carrier the receiver cannot reach
is recorded in `calls.log` as `OUTSIDE` and never enters the pool. A grant
naming a frequency far outside the span is a decode error rather than a
carrier, so it is recorded as `BADFREQ` and kept out on the same test.

### The shared allocation table
A control carrier announces that a talkgroup has been granted a traffic
carrier, a slot and a clear channel. The child on that traffic carrier never
hears the announcement, and its own encryption field is ambiguous. So the
child on the control carrier writes the grant into shared memory, and the
child on the traffic carrier reads it back. That is how a clear call gets a
real talkgroup and subscriber identity instead of an anonymous one. The table
is anonymous shared memory, mapped before any fork, with POSIX file locks
around each access.

### Stitch — the demultiplexer
Every carrier writes voice frames into its one pipe. 
It writes the headers of `calls.log` and `timemap.log`, then keys
on the talkgroup: the first frame of a new talkgroup forks a worker and keeps
a pipe to it, and every later frame goes there. `--max-gssi` caps the number
of workers, because a corrupt talkgroup field on the air would otherwise fork
without limit.

### Talkgroup worker, one for each talkgroup — speech and files
It owns one WAV file and appends to the shared logs. It holds each frame about a second,
so late duplicates can arrive: the same talkgroup is often heard on several
carriers at once, and the worker keeps the carrier that gives the most frames
and drops the rest. Then it runs the ETSI speech decoder, which gives 480
samples for each frame. A gap longer than a second gets a separator and a
codec reset; a short gap gets concealment frames. Each of those moves the WAV
by something other than 480 samples, so each one writes an anchor to
`timemap.log`. Those anchors are what make the map from a WAV position to UTC
exact rather than an estimate.

### Why processes and not threads

The ETSI speech decoder holds its state in globals, so two calls cannot share
one copy of it. The decoder must be isolated per call to record multiple simultaneously.

Any child that dies raises `SIGCHLD`, the
parent stops, and the whole tree ends so a supervisor can start a fresh one.
A tree that stays half alive looks healthy while it records nothing, which is
the worst outcome for something left running unattended.

### Layout

- `src/` — the program: the parent, the carrier children, the stitch
  process, the wall-clock map and the tests.
- `sdrpp-tetra-demodulator/` — a submodule that gives the TETRA channel
  decoder and the DSP blocks. It's my fork of
  [cropinghigh/sdrpp-tetra-demodulator](https://github.com/cropinghigh/sdrpp-tetra-demodulator).
- `third_party/sdrpp/` — a submodule that gives the
  [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus) core headers.

### Tests

```
src/test/run.sh                       # synthetic IQ, plus the unit tests
src/test/run.sh BASEBAND.wav HZ1 HZ2  # checks against a real capture
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for how to ensure no regressions.

## To be optimized

**The parent is the only serial stage.** It is the one process that touches
the full-rate stream, and its channelizer loop runs on one thread. Each
carrier costs about 1.6% of one core there, against about 1.5% in its own
child, which the other cores absorb. At 13 carriers the parent takes about
41% of a core; at 25 it takes about 40%, and past that it is the wall. A
thread pool over the channelizer, or an FFT channelizer in place of one VFO
for each carrier, is the fix. It was measured:
`volk-config-info --machine` gives `neonv8_orc` on a Pi 5, so the NEON
kernels already carry the present load.

**A free pool slot costs as much as a busy one.** The parent runs the
channelizer for every slot, assigned or not, so that all children count the
same samples and a slot filled late still reads the grants on the same
timebase. A spare slot therefore burns about 1.6% of a core to produce
nothing. Putting the sample counter in the shared table instead would let the
parent skip a free slot completely.

**A retune loses the head of the first call.** A demodulator needs 0.2 to
1.7 s to lock, and a weak carrier has been seen to need 4 s and even 115 s.
The grant arrives with the voice, so those seconds come out of the call. A
ring of a few seconds of IQ for each slot, replayed into the child after the
retune, would recover most of it.

**calls.log line order is not deterministic.** One process for each talkgroup
appends to it, so two lines written at the same moment can land in either
order. Running the *unchanged* binary twice over the same capture reproduces
the same transposition, so this is inherent and not a regression. Every line
carries its own `capture_sample`, so sort before comparing. Letting the
stitch process write that file, instead of each worker, would fix it.

**Automatic gain makes SNR incomparable across spans.** The sweep takes the
noise floor of each span alone to work around it. A fixed `--gain` gives
numbers that can be compared directly, and the report does not yet say so.

**Signal handlers carry no `SA_RESTART`, on purpose.** They have to break the
blocking read so a dead child ends the run at once. The cost is that a signal
landing inside a line-buffered flush can cut that line, but only when stdout
is a socket, which means under systemd. A regular file never blocks, so a
redirect to a file is safe.

**Raw replay assumes a little-endian host.** The WAV writer and the WAV
header parser are explicit about byte order, but `--fmt cs16` and `--fmt
cf32` read raw input in host order. `cu8` and `cs8` are byte-wise and safe.

**The ETSI codec fetch script is fragile.** It has no `set -e`, it uses the
GNU-only `md5sum` and `stat -c%s`, and on a host with no `md5sum` its
integrity test silently reports success and skips the retry over HTTP. It
belongs to the submodule, not here, so `build.sh` checks for a patched file
afterwards instead of trusting the exit status.
