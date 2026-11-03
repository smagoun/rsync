#!/bin/sh
# Tests --mac-types, --mac-text and --dst-window against a stock rsync daemon,
# using the Unix build (forks in .rsrc/, Finder info in .finf/).
#
# usage: classic/tests/macopts_test.sh [path/to/openrsync]

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OPENRSYNC=${1:-$HERE/../../openrsync/openrsync}
MKAD="python3 $HERE/mkad.py"
W=$(mktemp -d "${TMPDIR:-/tmp}/macopts-test.XXXXXX")
SRV=$W/srv
DST=$W/dst
PORT=$((20000 + ($$ + 77) % 10000))
FAILS=0
trap 'kill $(cat $W/rsyncd.pid 2>/dev/null) 2>/dev/null; rm -rf $W' EXIT

ok()   { echo "ok   - $1"; }
fail() { echo "FAIL - $1"; FAILS=$((FAILS + 1)); }
check() { if eval "$2"; then ok "$1"; else fail "$1"; fi; }
sync_() { "$OPENRSYNC" -a --netatalk "$@" rsync://127.0.0.1:$PORT/src/ "$DST/" ; }
tc()    { head -c 8 "$1" 2>/dev/null; }

mkdir -p "$SRV" "$DST"
printf 'int a;\nint b;\n' > "$SRV/plain.c"
printf 'notes\n' > "$SRV/README"
printf 'x\n' > "$SRV/data.bin"
printf 'hello\nworld\n' > "$SRV/withfork.c"
head -c 100 /dev/zero > $W/r
$MKAD "$SRV/._withfork.c" TEXT CWIE $W/r
cat > $W/rsyncd.conf <<EOF
pid file = $W/rsyncd.pid
port = $PORT
use chroot = no
uid = $(id -u)
gid = $(id -g)
[src]
    path = $SRV
    read only = yes
EOF
rsync --daemon --config=$W/rsyncd.conf </dev/null >/dev/null 2>&1
sleep 1

echo "# --mac-types"
sync_ --mac-types > $W/o1 2>&1 || { cat $W/o1; fail "pull"; }
check "plain .c gets TEXT/ttxt" '[ "$(tc $DST/.finf/plain.c)" = TEXTttxt ]'
check "README gets TEXT/ttxt" '[ "$(tc $DST/.finf/README)" = TEXTttxt ]'
check ".bin gets BINA/SITx" '[ "$(tc $DST/.finf/data.bin)" = BINASITx ]'
check "a file with a sidecar keeps the sidecar's type" \
    '[ "$(tc $DST/.finf/withfork.c)" = TEXTCWIE ]'
sync_ --mac-types -v > $W/o2 2>&1
check "second run: nothing transferred, no type churn" \
    '! grep -q "setting type\|clearing\|^plain.c\|^README" $W/o2'
printf '.c TEXT KAHL\n# comment\nREADME TEXT R*ch\n' > $W/types
sync_ --mac-types=$W/types -v > $W/o3 2>&1
check "types file overrides the built-in table (existing files updated)" \
    '[ "$(tc $DST/.finf/plain.c)" = TEXTKAHL ] && [ "$(tc $DST/.finf/README)" = "TEXTR*ch" ]'
check "sidecar type still wins" '[ "$(tc $DST/.finf/withfork.c)" = TEXTCWIE ]'
sync_ -v > $W/o4 2>&1
check "without --mac-types, mapped Finder info is cleared again" \
    '[ ! -e $DST/.finf/plain.c ]'

echo "# --mac-text"
rm -rf "$DST"; mkdir "$DST"
sync_ --mac-text > $W/t1 2>&1 || { cat $W/t1; fail "pull"; }
check "plain.c stored with CR line ends" \
    '[ "$(od -An -c $DST/plain.c | tr -d " \n")" = "inta;\\rintb;\\r" ]'
check "data.bin (not a text extension) untouched" \
    '[ "$(od -An -c $DST/data.bin | tr -d " \n")" = "x\\n" ]'
sync_ --mac-text -i > $W/t2 2>&1
check "second run transfers nothing" '! grep -q "^[<>]f" $W/t2'
sync_ --mac-text -i --checksum > $W/t3 2>&1
check "--checksum agrees (checksums see LF)" '! grep -q "^[<>]f" $W/t3'
python3 -c "import sys; open(sys.argv[1],'w').write('int a;\n' * 2000 + 'int z;\n')" \
    "$SRV/plain.c"
touch -d '2030-01-01' "$SRV/plain.c"
sync_ --mac-text --no-whole-file --stats > $W/t4 2>&1
check "delta update result has CR line ends and the new line" \
    '[ "$(tail -c 7 $DST/plain.c | od -An -c | tr -d " \n")" = "intz;\\r" ] &&
     [ "$(tr -cd "\n" < $DST/plain.c | wc -c)" -eq 0 ]'
check "without --mac-text the CR file is seen as different" \
    'sync_ -i --checksum > $W/t5 2>&1; grep -q "plain.c" $W/t5'

echo "# --dst-window"
rm -rf "$DST"; mkdir "$DST"
sync_ > /dev/null 2>&1
touch -d "@$(( $(stat -c %Y $SRV/README) + 3600 ))" "$DST/README"
sync_ -i --dst-window > $W/d1 2>&1
check "exactly one hour off, same size: unchanged with --dst-window" \
    '! grep -q "^[<>]f.* README" $W/d1'
touch -d "@$(( $(stat -c %Y $SRV/README) + 3600 ))" "$DST/README"
sync_ -i > $W/d2 2>&1
check "...and transferred without it" 'grep -q "^[<>]f.* README" $W/d2'
touch -d "@$(( $(stat -c %Y $SRV/README) - 3601 ))" "$DST/README"
sync_ -i --dst-window > $W/d3 2>&1
check "an hour and a second off: transferred" 'grep -q "^[<>]f.* README" $W/d3'
touch -d "@$(( $(stat -c %Y $SRV/README) - 3600 ))" "$DST/README"
sync_ -i --dst-window --no-dst-window > $W/d4 2>&1
check "--no-dst-window turns it off" 'grep -q "^[<>]f.* README" $W/d4'

if [ $FAILS -ne 0 ]; then
	echo "$FAILS test(s) failed; outputs in $W"
	trap - EXIT
	exit 1
fi
echo "all tests passed"
