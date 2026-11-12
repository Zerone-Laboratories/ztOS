#!/bin/bash
# audio/test-call-path.sh: the path of a call without a call. No radio is touched.
# Runs on the phone as the user with the session's PipeWire. For each route: a
# tone through the normal playback while the driver is in call mode, measured
# at the microphones; then the driver's own numbers (call_diag). Ends with
# ordinary playback and capture, to show a call leaves nothing behind.
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
P=/sys/module/snd_zrn_abox/parameters
T=$(mktemp -d); trap 'amixer -q -c0 cset name="Voice Call" Off; rm -rf "$T"' EXIT
python3 - "$T/tone.wav" <<'PY'
import math, struct, sys, wave
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
w.writeframes(b''.join(struct.pack('<hh', *(2 * [int(9000 * math.sin(2 * math.pi * 1000 * i / 48000))])) for i in range(48000 * 3)))
PY
measure() {	# label: mic level with the tone playing
	pw-play "$T/tone.wav" & local pid=$!
	sleep 0.8
	timeout 1.5 pw-record --rate 48000 --channels 1 --format s16 --raw - > "$T/mic.raw" 2>/dev/null
	wait $pid
	python3 - "$1" "$T/mic.raw" <<'PY'
import array, math, sys
a = array.array('h'); a.frombytes(open(sys.argv[2], 'rb').read()[:144000 // 2 * 2])
print('%-10s fast=%s mic rms %6.0f peak %6d (%d samples)' % (sys.argv[1], open('/sys/module/snd_zrn_abox/parameters/fast_rebuilds').read().strip(), math.sqrt(sum(x * x for x in a) / max(len(a), 1)), max(map(abs, a), default=0), len(a)))
PY
}
measure before
for cycle in 1 2; do
	for route in Earpiece Speaker Earpiece; do
		amixer -q -c0 cset name='Voice Call' $route || echo "FAILED $route"
		sleep 0.3
		measure "$route"
		cat $P/call_diag
	done
	amixer -q -c0 cset name='Voice Call' Off
	sleep 0.3
done
cat $P/call_diag
measure after
for f in ipc_retries dsp_restarts codec_errors fast_rebuilds; do echo "$f=$(cat $P/$f)"; done
