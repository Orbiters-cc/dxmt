#!/bin/bash
# The GPU times of encoders under ThreadSanitizer and without it, then what both wrote: every encoder of every recorded command
# buffer either has times of its own, inside its command buffer's GPU time, or is counted as having none; none has two.
# Usage: run-samples-test.sh <otl.py of Orbiters-cc/voile-lkd>
set -eu
W=$(cd "$(dirname "$0")" && pwd)
D=$(mktemp -d "${TMPDIR:-/tmp}/samples.XXXXXX"); D=$(cd "$D" && pwd -P); trap 'rm -rf "$D"' EXIT
build() { clang -arch arm64 -std=gnu11 -O1 -g -Wall -Wno-unused-function -Wno-unguarded-availability-new "$@" "$W/samples_test.m" -framework Foundation -framework Metal; }
build -fsanitize=thread -o "$D/tsan"
build -o "$D/plain"
for m in tsan plain; do
    mkdir "$D/$m.d"
    TSAN_OPTIONS=halt_on_error=0:exitcode=66 OBELISK_TIMELINE="$D/$m.d" OBELISK_TIMELINE_PASSES=1 "$D/$m" 2> "$D/$m.log" || { grep -a -A25 "WARNING: ThreadSanitizer" "$D/$m.log" | head -60; echo "$m FAILED"; exit 1; }
    echo "$m ran, $(grep -a -c 'WARNING: ThreadSanitizer' "$D/$m.log" || true) reports of ThreadSanitizer"
done
python3 -I - "$1" "$D/tsan.d" "$D/plain.d" <<'PY'
import collections, glob, importlib.util, sys
spec = importlib.util.spec_from_file_location("otl", sys.argv[1]); otl = importlib.util.module_from_spec(spec); spec.loader.exec_module(otl)
for d in sys.argv[2:]:
    f = otl.File(glob.glob(d + "/otl-dxmt-*.bin")[0])
    assert f.lost == 0 and f.end["reason"] == "exit", f.says()
    s = otl.Session.__new__(otl.Session); s.dxmt = {f.pid: f}; s.clocks = {}
    s.chunk_done = {f.pid: {(r.v[5], r.v[0]): r for r in f.get("D_DONE")}}
    encoders, kind = collections.Counter(), {}
    for k in ("D_PASS", "D_COMPUTE", "D_BLIT"):
        for r in f.get(k): encoders[r.v[0]] += 1; kind[(r.v[0], r.a)] = k
    times, why, seen = collections.Counter(), collections.Counter(), set()
    for r in f.get("D_PASS_TIME"):
        key, i = r.v[0], r.a & 0xffff
        assert (key, i) not in seen, "an encoder has times twice"
        seen.add((key, i))
        assert kind[(key, i)] == {0: "D_PASS", 1: "D_COMPUTE", 2: "D_BLIT"}[r.a >> 16], "times of another encoder"
        times[key] += 1
        t, w = s.times(f, key >> 48, key & otl.SERIAL, r)
        if not t: why[w] += 1
    skipped, reasons = collections.Counter(), collections.Counter()
    for r in f.get("D_PASS_SKIP"): skipped[r.v[0]] += r.v[1]; reasons[otl.SKIP[r.a]] += r.v[1]
    for key, n in encoders.items():
        assert times[key] + skipped[key] == n, (hex(key), n, times[key], skipped[key])
    queues = f.get("D_QUEUE")
    assert len([r for r in queues if r.a]) == len([r for r in queues if not r.a]) == 24
    assert not set(why) & {"outside its command buffer's GPU time", "ends before it begins"}, why
    print(f"{sum(encoders.values())} encoders of {len(encoders)} command buffers of 24 queues: {sum(times.values())} with times, not told {dict(why)}, none {dict(reasons)}")
    assert reasons["too many encoders"] and reasons["its command buffer failed"] and sum(times.values()) > 1000, reasons
PY
