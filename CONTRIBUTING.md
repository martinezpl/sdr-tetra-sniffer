# Contributing

Any change to the signal pipeline must **prove that it did not alter a
recording.**

So before you change anything, make a static reference. Then make your
change, replay the same input, and compare the output byte for byte.

## 1. Capture a golden file, once

Record raw IQ from the dongle over the carriers you use. Ten minutes should be
enough to hold a few hundred voice frames:

```
rtl_sdr -f 420000000 -s 3200000 -n 1920000000 golden.cu8
```

`rtl_sdr` is not a build dependency. It comes from `rtl-sdr` on Debian and
from `librtlsdr` on Homebrew.

`-f` is your `--center`, `-s` your `--rate`, and `-n` is the sample count, so
600 s at 3.2 MS/s is 1.92e9 samples and about 3.8 GB on disk. Keep the file.
Re-capturing gives different traffic, and then no two runs can be compared.

A capture with no clear voice proves nothing. Check that the baseline below
writes WAV files that are not empty.

## 2. Build the baseline, before your change

The ETSI codec is not in git, so do not check out an old commit in place.
Copy the working tree and roll back only the source:

```
cp -a . ../tetra-analyzer-base
cd ../tetra-analyzer-base
git checkout <commit-before-your-change> -- src/
rm -rf src/build tetra-analyze
./build.sh
```

## 3. Run the baseline

**Replay with the centre, rate and carriers that the capture was made with,
not with whatever the current defaults are.** A default can move between
releases, and a carrier outside the captured span decodes nothing. The
example below matches the capture in step 1.

Pin everything else that can vary. `--start-utc` fixes the run directory
name, `--max-carriers` equal to the carrier count leaves no free slot, and
`--no-learn` stops the carrier file from changing what the next run does:

```
./tetra-analyze run --iq ~/golden.cu8 --fmt cu8 --rate 3200000 \
    --center 420000000 --tune-offset -2500 \
    --start-utc 2026-01-01T00:00:00Z \
    --max-carriers 13 --no-learn \
    --out /tmp/base \
    418562500 418762500 418962500 419162500 419362500 419562500 419762500 \
    419962500 420162500 420362500 420562500 420762500 420962500
```

## 4. Make your change, then run it again

Same command, same golden file, same flags, `--out /tmp/new`.

## 5. Compare

```
diff -r /tmp/base/2026-01-01T000000Z /tmp/new/2026-01-01T000000Z -x clock.log
```

**`calls/*.wav` and `timemap.log` must be identical.** They are the
recording. Any difference at all is a change in behaviour, and you must be
able to say why.

Two files are allowed to differ:

- **`clock.log`** holds wall-clock timestamps, so every run differs. That is
  why the command above excludes it.
- **`calls.log`** holds the same lines in a possibly different order. One
  process for each talkgroup appends to it, so two lines written at the same
  moment can land either way round. Running the *unchanged* binary twice
  reproduces this, so it is not a regression. Compare it sorted:

```
diff <(sort /tmp/base/2026-01-01T000000Z/calls.log) \
     <(sort /tmp/new/2026-01-01T000000Z/calls.log)
```

If that sorted comparison is clean and the WAV files match, your change is
recording-neutral.

### When the output is supposed to change

Say so, and show the difference. A change that adds carriers, alters the
decoder or changes a file format will differ, and that is fine. What is not
fine is finding out later. Put the numbers in the commit message: how many
`calls.log` lines before and after, which WAV files appeared or vanished, and
how many samples each one gained or lost.

## 6. Run the suite

```
src/test/run.sh                       # synthetic IQ, plus the unit tests
src/test/run.sh BASEBAND.wav HZ1 HZ2  # checks against a real capture
```

It must print `0 failure(s)`. One `SKIP` is expected: the synthetic generator
carries no clear voice, so it cannot exercise the wall-clock map.

The suite pins the faults that hide. A dead stitch process must end the run
inside 100 ms, a WAV header must agree with the size of its file, and a run
across UTC midnight must name the run before it. Add a check for whatever
your change could break in silence.

## What to be careful with

**The decoder keeps state in several places.** `tetra_mac_state`, the display
state, the crypto state and the fragment slots all carry it. One stale field
is enough to suppress playback for the rest of a run. That is why a slot that
changes frequency rebuilds the whole decoder instead of resetting fields.

**`pwrite` and `O_APPEND` do not mix.** On Linux `O_APPEND` makes `pwrite`
ignore its offset, which silently corrupts a WAV. macOS does not do this, so
the Mac cannot catch it. `WavWriter` opens with neither `O_APPEND` nor
`O_TRUNC` on purpose.

**`snprintf` returns what it would have written**, not what fit. Use
`dprintf` for a line whose length depends on a path.

**The ETSI codec needs `-fwrapv`.** Its arithmetic detects overflow by adding
and then reading the sign bit of the result, which is undefined without it.
An optimising compiler is free to delete the check.

**Signal handlers must not take `SA_RESTART`.** They have to break the
blocking read, or a dead child does not end the run.

## Style

Match the file you are editing. Comments say why, not what: the code already
says what. Prefer deleting to adding, and leave a `ponytail:` comment on a
deliberate shortcut, naming its ceiling and the upgrade path.
