#!/bin/sh
# End-to-end test of openrsync --netatalk against a stock rsync daemon.
#
# usage: classic/tests/netatalk_test.sh [path/to/openrsync]
#
# The server tree mimics a netatalk 4 "ea = sys" volume (data file plus
# "._name" AppleDouble sidecar).  The client is the Unix build, whose
# backend stores forks as .rsrc/NAME and .finf/NAME (Basilisk II layout),
# exactly the code path the Classic Mac OS build uses minus the File
# Manager.  Needs: rsync (daemon), python3.

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OPENRSYNC=${1:-$HERE/../../openrsync/openrsync}
MKAD="python3 $HERE/mkad.py"
W=$(mktemp -d "${TMPDIR:-/tmp}/nt-test.XXXXXX")
SRV=$W/srv
DST=$W/dst
PORT=$((20000 + $$ % 10000))
FAILS=0
trap 'kill $(cat $W/rsyncd.pid 2>/dev/null) 2>/dev/null; rm -rf $W' EXIT

ok()   { echo "ok   - $1"; }
fail() { echo "FAIL - $1"; FAILS=$((FAILS + 1)); }
check() { if eval "$2"; then ok "$1"; else fail "$1"; fi; }
sync_() { "$OPENRSYNC" -a --netatalk "$@" rsync://127.0.0.1:$PORT/src/ "$DST/" ; }
hex()   { od -An -tx1 -v "$1" | tr -d ' \n'; }

mkdir -p "$SRV/src" "$DST"
printf 'int main(void){return 0;}\n' > "$SRV/src/main.c"
head -c 300 /dev/zero > $W/small.rsrc
$MKAD "$SRV/src/._main.c" TEXT KAHL $W/small.rsrc
: > "$SRV/App"
python3 -c "import os,sys; sys.stdout.buffer.write(os.urandom(200000))" > $W/app.rsrc
$MKAD "$SRV/._App" APPL TEST $W/app.rsrc 2100
printf 'plain\n' > "$SRV/README"
mkdir -p "$SRV/.AppleDB" && : > "$SRV/.AppleDB/cnid2.db"
touch -d '2024-01-02 03:04:05' "$SRV/App" "$SRV/src/main.c" "$SRV/README"
touch -d '2025-05-06 07:08:09' "$SRV/._App" "$SRV/src/._main.c"

cat > $W/rsyncd.conf <<EOF
pid file = $W/rsyncd.pid
port = $PORT
use chroot = no
uid = $(id -u)
gid = $(id -g)
[src]
    path = $SRV
    read only = yes
[up]
    path = $W/up
    read only = no
EOF
mkdir -p $W/up
rsync --daemon --config=$W/rsyncd.conf </dev/null >/dev/null 2>&1
sleep 1

echo "# 1. first pull"
sync_ > $W/out1 2>&1 || { cat $W/out1; fail "first pull exit status"; }
check "no ._ files stored on disk" '[ -z "$(find $DST -name "._*")" ]'
check "resource fork of App (200000 bytes)" 'cmp -s $W/app.rsrc $DST/.rsrc/App'
check "resource fork of src/main.c" 'cmp -s $W/small.rsrc $DST/src/.rsrc/main.c'
check "Finder info of App: APPL/TEST, flags 2100" \
    '[ "$(hex $DST/.finf/App | cut -c1-20)" = "4150504c544553542100" ]'
check "data fork of App is empty" '[ -f $DST/App ] && [ ! -s $DST/App ]'
check "data mtime kept" '[ "$(date -r $DST/App +%F)" = 2024-01-02 ]'
check "sidecar mtime kept separately" '[ "$(date -r $DST/.finf/App +%F)" = 2025-05-06 ]'
check ".AppleDB excluded" '[ ! -e $DST/.AppleDB ]'

echo "# 2. second run is a no-op"
sync_ -i > $W/out2 2>&1
check "nothing transferred" '! grep -q "^[<>]f" $W/out2'

echo "# 3. resource fork changed on server (same size)"
python3 -c "
import sys; f=open(sys.argv[1],'r+b'); f.seek(82+1000); f.write(b'CHANGED'); f.close()
" "$SRV/._App"
touch -d '2025-06-01 00:00:00' "$SRV/._App"
sync_ -i > $W/out3 2>&1
check "only ._App re-sent" 'grep -q "^[<>]f.* \._App" $W/out3 && ! grep -q "^[<>]f.* App$" $W/out3'
check "fork updated" '[ "$(dd if=$DST/.rsrc/App bs=1 skip=1000 count=7 2>/dev/null)" = CHANGED ]'
check "data mtime untouched" '[ "$(date -r $DST/App +%F)" = 2024-01-02 ]'

echo "# 4. data fork changed on server"
printf 'int main(void){return 1;}\n' > "$SRV/src/main.c"
sync_ -i > $W/out4 2>&1
check "main.c re-sent, its sidecar not" \
    'grep -q "^[<>]f.* src/main.c" $W/out4 && ! grep -q "_main.c" $W/out4'
check "fork of main.c survives data update" 'cmp -s $W/small.rsrc $DST/src/.rsrc/main.c'

echo "# 5. delta transfer of a large fork"
python3 -c "
import sys; f=open(sys.argv[1],'r+b'); f.seek(82+150000); f.write(b'DELTA'); f.close()
" "$SRV/._App"
touch -d '2025-07-01 00:00:00' "$SRV/._App"
"$OPENRSYNC" -a --netatalk --no-whole-file --stats \
    rsync://127.0.0.1:$PORT/src/ "$DST/" > $W/out5 2>&1
lit=$(sed -n 's/.*\(Unmatched\|[Ll]iteral\) data: *\([0-9,]*\).*/\2/p' $W/out5 | tr -d ,)
check "delta sends far less than the 200 KB fork (literal ${lit:-?})" \
    '[ -n "$lit" ] && [ "$lit" -lt 20000 ]'
check "delta result correct" \
    '[ "$(dd if=$DST/.rsrc/App bs=1 skip=150000 count=5 2>/dev/null)" = DELTA ]'

echo "# 6. sidecar removed on server"
rm "$SRV/src/._main.c"
sync_ > $W/out6 2>&1
check "local fork and Finder info cleared" \
    '[ ! -e $DST/src/.rsrc/main.c ] && [ ! -e $DST/src/.finf/main.c ]'

echo "# 7. dry run changes nothing"
rm "$SRV/README"
sync_ -n --delete > $W/out7 2>&1
check "README still present after -n" '[ -f $DST/README ]'

echo "# 8. --delete of a file with a fork"
rm "$SRV/App" "$SRV/._App"
sync_ --delete > $W/out8 2>&1
check "App and its sidecar storage deleted" \
    '[ ! -e $DST/App ] && [ ! -e $DST/.rsrc/App ] && [ ! -e $DST/.finf/App ]'
check "README deleted" '[ ! -e $DST/README ]'
check "backend folders protected" '[ -d $DST/.finf ]'

echo "# 9. sidecar without a data file"
$MKAD "$SRV/._Orphan" TEXT ttxt $W/small.rsrc
sync_ > $W/out9 2>&1
check "orphan sidecar creates an empty file with the fork" \
    '[ -f $DST/Orphan ] && cmp -s $W/small.rsrc $DST/.rsrc/Orphan'

echo "# 10. sending: forks go out as ._ sidecars"
"$OPENRSYNC" -a --netatalk "$DST/" rsync://127.0.0.1:$PORT/up/ > $W/out10 2>&1 ||
    { cat $W/out10; fail "push exit status"; }
# (By now App is deleted and main.c's fork cleared; Orphan has a fork.)
check "._Orphan sent as AppleDouble holding the fork" \
    '[ "$(hex $W/up/._Orphan | cut -c1-8)" = "00051607" ] &&
     tail -c +83 $W/up/._Orphan | cmp -s - $W/small.rsrc'
check "._Orphan carries the Finder info (TEXT/ttxt)" \
    '[ "$(dd if=$W/up/._Orphan bs=1 skip=50 count=8 2>/dev/null)" = TEXTttxt ]'
check "backend folders not sent" '[ ! -e $W/up/.rsrc ] && [ ! -e $W/up/.finf ]'
check "file without a fork has no sidecar" \
    '[ -f $W/up/src/main.c ] && [ ! -e $W/up/src/._main.c ]'

if [ $FAILS -ne 0 ]; then
	echo "$FAILS test(s) failed; outputs in $W"
	trap - EXIT
	exit 1
fi
echo "all tests passed"
