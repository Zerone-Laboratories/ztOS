#!/bin/bash
# The passcode backend end to end, on a test vault in /run: nothing of the
# owner's is touched (not /var/lib/zrn-passcode, not the shell, not the lock
# screen). Run as root: sudo ./test-vault.sh [BUILD_DIR]
# Test secrets are made at random into files and never printed.
set -u
B=${1:-$(dirname "$(readlink -f "$0")")}
T=/run/zrn-pc-test
UNIT=zrn-pc-test
export ZRN_PASSCODE_SOCKET=$T/sock
PC="$T/bin/zrn-passcode"
pass=0; failn=0
ok()   { echo "PASS  $*"; pass=$((pass + 1)); }
bad()  { echo "FAIL  $*"; failn=$((failn + 1)); }
want() { # want DESCRIPTION PATTERN OUTPUT
	if grep -q -- "$2" <<<"$3"; then ok "$1"; else bad "$1"; echo "$3" | sed 's/^/      /'; fi
}
ms()   { echo $(( ($(date +%s%N) - $1) / 1000000 )); }
start() {
	systemd-run --quiet --unit=$UNIT --property=Type=simple \
		"$T/bin/zrn-passcoded" --dir "$T/state" --socket "$T/sock" "$@"
	for _ in $(seq 50); do [ -S "$T/sock" ] && return; python3 -c 'import time; time.sleep(0.1)'; done
}
stopd() { systemctl stop $UNIT 2>/dev/null; systemctl reset-failed $UNIT 2>/dev/null; }

stopd; rm -rf "$T"; mkdir -p "$T/bin"; chmod 711 "$T"  # the user reaches the socket only
cp "$B/zrn-passcoded" "$B/zrn-passcode" "$T/bin/"; chmod 700 "$T/bin"
umask 077
# secrets: a 6-digit PIN, a wrong PIN, an alphanumeric passcode
python3 -c 'import secrets; print("".join(secrets.choice("0123456789") for _ in range(6)))' > $T/pin
python3 -c 'import secrets; print("".join(secrets.choice("0123456789") for _ in range(6)))' > $T/wrong
cmp -s $T/pin $T/wrong && echo 000000 > $T/wrong
python3 -c 'import secrets, string; print("".join(secrets.choice(string.ascii_letters + string.digits) for _ in range(10)))' > $T/alnum
cat $T/pin $T/alnum > $T/change.in
start

out=$($PC status); want "off by default (enabled=0, no vault)" 'enabled=0' "$out"; want "no vault yet" 'vault=0' "$out"

t=$(date +%s%N); out=$($PC set pin --recovery-out $T/rk < $T/pin); echo "      set took $(ms $t) ms"
want "set pin" '^ok' "$out"
[ -s $T/rk ] && ok "recovery key returned once" || bad "recovery key returned"
out=$($PC status); want "status after set: enabled, length 6" 'enabled=1' "$out"; want "length=6" 'length=6' "$out"
out=$($PC inspect < $T/pin); want "vault archive decrypts and checks out" 'entries=3' "$out"
want "meta in the vault" 'meta.kind=pin' "$out"; want "biometrics dir" 'entry=biometrics/' "$out"

for i in 1 2 3; do t=$(date +%s%N); out=$($PC verify < $T/pin); echo "      verify right #$i: $(ms $t) ms"; done
want "verify right" '^ok' "$out"
t=$(date +%s%N); out=$($PC verify < $T/wrong); echo "      verify wrong: $(ms $t) ms"
want "verify wrong refused" 'code=wrong' "$out"
for i in 2 3 4 5; do $PC verify < $T/wrong >/dev/null; done
out=$($PC status); want "5 wrong: no wait yet" 'locked_until=0' "$out"
out=$($PC verify < $T/wrong); want "6th wrong: 30 s wait" 'failures=6' "$out"
out=$($PC verify < $T/pin); want "right passcode refused while locked" 'code=locked' "$out"

stopd; start
out=$($PC status); want "lockout kept across a daemon restart (failures=6)" 'failures=6' "$out"
lu=$(grep locked_until <<<"$out" | cut -d= -f2); [ "$lu" -gt "$(date +%s)" ] && ok "still locked after restart" || bad "still locked after restart"
out=$($PC verify < $T/pin); want "locked after restart" 'code=locked' "$out"
python3 -c "import time; time.sleep(max(0, $lu - time.time()) + 1)"
out=$($PC verify < $T/pin); want "right passcode after the wait" '^ok' "$out"
out=$($PC status); want "count reset on success" 'failures=0' "$out"

out=$($PC change alnum < $T/change.in); want "change pin -> alphanumeric" '^ok' "$out"
out=$($PC verify < $T/alnum); want "new passcode verifies" '^ok' "$out"
out=$($PC verify < $T/pin); want "old passcode refused" 'code=wrong' "$out"
out=$($PC inspect < $T/alnum); want "meta updated by change" 'meta.kind=alnum' "$out"; want "archive intact after change" 'entries=3' "$out"
out=$($PC verify < $T/rk); want "recovery key is not a passcode for verify" 'code=wrong' "$out"
out=$($PC inspect < $T/rk); want "recovery key opens the vault" 'entries=3' "$out"

echo WRONG-KEY-0000 > $T/badrk
out=$($PC save-recovery < $T/badrk); want "save-recovery refuses a wrong key" 'code=wrong' "$out"
out=$($PC save-recovery < $T/rk); want "save-recovery" '^ok' "$out"
m=$(stat -c '%a %U' $T/state/recovery.key); [ "$m" = "400 root" ] && ok "recovery.key is 0400 root" || bad "recovery.key mode $m"
cmp -s $T/rk $T/state/recovery.key && ok "saved key matches" || bad "saved key matches"

out=$($PC disable < $T/alnum); want "disable" '^ok' "$out"
out=$($PC status); want "disabled, vault kept" 'enabled=0' "$out"; want "vault kept" 'vault=1' "$out"
out=$($PC set pin < $T/pin); want "set again without the old secret refused" 'code=vault-exists' "$out"
cat $T/pin $T/rk > $T/reset.in
out=$($PC set pin --unlock --recovery-out $T/rk2 < $T/reset.in); want "set again unlocked by the recovery key" '^ok' "$out"
[ ! -e $T/rk2 ] && ok "recovery key kept (none new)" || bad "a new recovery key was made"
out=$($PC inspect < $T/pin); want "data kept through disable + set" 'entries=3' "$out"

# a peer that is not root, the shell or Settings is refused
out=$(runuser -u "$(id -nu 1000)" -- python3 -c "
import socket; s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET); s.connect('$T/sock'); s.send(b'status'); print(s.recv(4096).decode())")
want "SO_PEERCRED: the user's other programs are refused" 'code=denied' "$out"

for i in $(seq 6); do $PC verify < $T/wrong >/dev/null; done
out=$($PC status); want "locked again" 'failures=6' "$out"
out=$($PC recover); want "recover over the socket" '^ok' "$out"
out=$($PC status); want "recover: off and not locked" 'enabled=0' "$out"; want "recover: count cleared" 'failures=0' "$out"
want "recover: vault kept" 'vault=1' "$out"

out=$($PC set pin --unlock --recovery-out $T/rk3 < $T/reset.in); want "on again" '^ok' "$out"
stopd
out=$($PC recover --dir $T/state); want "recover with the daemon down (offline)" 'ok (offline)' "$out"
start
out=$($PC status); want "offline recover held" 'enabled=0' "$out"
out=$($PC inspect < $T/rk); want "vault data still opens with the recovery key" 'entries=3' "$out"

echo "--- vault header (keyslots, no secrets)"
cryptsetup luksDump $T/state/vault.img | grep -E '^  [0-9]: luks2|PBKDF|Memory|Threads|Time cost|Iterations|cipher:|offset:|sector:' | sed 's/^/      /'
echo "--- journal lines of the test daemon"
journalctl -u $UNIT --no-pager -o cat | tail -n 20 | sed 's/^/      /'
stopd
rm -rf "$T"
echo "RESULT: $pass passed, $failn failed; test vault removed"
