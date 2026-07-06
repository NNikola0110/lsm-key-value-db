#!/bin/sh
# Chaos drills A-E (Section 9.5). Run from the repo root:
#   sh tests/drills.sh [path-to-lsmkv]
# Uses LSMKV_CRASHPOINT fault injection (see TESTING.md for the point list).
set -u
LSMKV=${1:-./build/Debug/lsmkv.exe}
[ -x "$LSMKV" ] || LSMKV=./build/lsmkv
FAILURES=0
DATA=./data

fail() { echo "DRILL FAIL: $1"; FAILURES=$((FAILURES+1)); }
pass() { echo "DRILL PASS: $1"; }
reset_store() {
  rm -rf "$DATA/wal" "$DATA/sst" "$DATA/manifest.json" "$DATA/compaction_stats.json"
  "$LSMKV" init >/dev/null 2>&1
}

export LSMKV_MEMTABLE_MAX_BYTES=150

# --- Drill A: power cut during write -----------------------------------
reset_store
i=1
while [ $i -le 20 ]; do
  if [ $((i % 5)) -eq 0 ]; then
    # every 5th write dies after append, before/after fsync (alternating)
    CP=CP_WAL_AFTER_APPEND_BEFORE_FSYNC
    [ $((i % 10)) -eq 0 ] && CP=CP_WAL_AFTER_FSYNC_BEFORE_ACK
    LSMKV_CRASHPOINT=$CP "$LSMKV" put --key "a$i" --value "val$i" >/dev/null 2>&1
  else
    "$LSMKV" put --key "a$i" --value "val$i" >/dev/null 2>&1
  fi
  i=$((i+1))
done
if "$LSMKV" wal-verify 2>/dev/null | grep -q "status=OK"; then
  # acknowledged writes (the non-crashed ones) must all be present
  if [ "$("$LSMKV" get --key a3 2>/dev/null | tail -1)" = "val3" ] &&
     [ "$("$LSMKV" get --key a19 2>/dev/null | tail -1)" = "val19" ]; then
    pass "A power-cut-during-write (recovery OK, acknowledged writes present)"
  else fail "A: acknowledged write missing after recovery"; fi
else fail "A: wal-verify not OK"; fi

# --- Drill B: flush crash (before index, and before manifest) ----------
reset_store
"$LSMKV" put --key b1 --value BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB >/dev/null 2>&1
"$LSMKV" put --key b2 --value BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB >/dev/null 2>&1
"$LSMKV" put --key b3 --value BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB >/dev/null 2>&1
LSMKV_CRASHPOINT=CP_FLUSH_AFTER_DATABLOCKS_BEFORE_INDEX "$LSMKV" flush-now >/dev/null 2>&1
S1=$("$LSMKV" stats --json 2>/dev/null | grep -o '"sst_live": [0-9]*')
if [ "$S1" = '"sst_live": 0' ]; then
  "$LSMKV" flush-now >/dev/null 2>&1
  S2=$("$LSMKV" stats --json 2>/dev/null | grep -o '"sst_live": [0-9]*')
  if [ "$S2" = '"sst_live": 1' ] && [ "$("$LSMKV" get --key b1 2>/dev/null | tail -1)" = "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB" ]; then
    pass "B flush-crash (no half-published table; retry succeeded)"
  else fail "B: retry flush did not publish exactly one table"; fi
else fail "B: crashed flush published a table ($S1)"; fi

# --- Drill C: compaction crash after outputs, before manifest ----------
reset_store
for k in c1 c2 c3; do "$LSMKV" put --key $k --value CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC >/dev/null 2>&1; done
"$LSMKV" flush-now >/dev/null 2>&1
for k in c4 c5 c6; do "$LSMKV" put --key $k --value CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC >/dev/null 2>&1; done
"$LSMKV" flush-now >/dev/null 2>&1
LSMKV_CRASHPOINT=CP_COMPACTION_AFTER_OUTPUTS_BEFORE_MANIFEST \
  LSMKV_SIZE_TIERED_FAN_IN=2 "$LSMKV" compaction-run >/dev/null 2>&1
S=$("$LSMKV" stats --json 2>/dev/null | grep -o '"sst_live": [0-9]*')
if [ "$S" = '"sst_live": 2' ]; then     # inputs still live, orphan pruned on startup
  if LSMKV_SIZE_TIERED_FAN_IN=2 "$LSMKV" compaction-run 2>/dev/null | grep -q "compact job="; then
    pass "C compaction-crash (inputs live; orphan pruned; re-run OK)"
  else fail "C: compaction re-run failed"; fi
else fail "C: table set changed after crashed compaction ($S)"; fi

# --- Drill D: corrupted WAL tail ----------------------------------------
reset_store
"$LSMKV" put --key d1 --value DD >/dev/null 2>&1
SEG=$(ls "$DATA"/wal/*.wal | head -1)
printf 'GARBAGE_TAIL_BYTES' >> "$SEG"
if "$LSMKV" wal-verify 2>/dev/null | grep -q "truncated=1"; then
  if [ "$("$LSMKV" get --key d1 2>/dev/null | tail -1)" = "DD" ]; then
    pass "D corrupted-tail (truncated, data intact)"
  else fail "D: data lost after tail truncation"; fi
else fail "D: tail corruption not detected"; fi

# --- Drill E: flush target unavailable (disk-failure stand-in) ---------
# An unwritable sst_dir (illegal path chars) makes the flush attempt fail
# with IOFailure; the data must stay covered by the WAL, the manifest must
# not change, and a retry with a healthy config must succeed.
reset_store
"$LSMKV" put --key e1 --value EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE >/dev/null 2>&1
"$LSMKV" put --key e2 --value EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE >/dev/null 2>&1
"$LSMKV" put --key e3 --value EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE >/dev/null 2>&1
if LSMKV_SST_DIR="$DATA/bad<dir>|name" "$LSMKV" flush-now >/dev/null 2>&1; then
  fail "E: flush to unwritable dir did not fail"
else
  M=$("$LSMKV" stats --json 2>/dev/null | grep -o '"sst_live": [0-9]*')
  if [ "$M" = '"sst_live": 0' ]; then   # no manifest change on failed flush
    "$LSMKV" flush-now >/dev/null 2>&1  # immutable rebuilt from WAL -> succeeds
    if [ "$("$LSMKV" get --key e1 2>/dev/null | tail -1)" = "EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE" ]; then
      pass "E disk-failure flush (graceful failure, data retained, retry OK)"
    else fail "E: data lost after failed flush"; fi
  else fail "E: failed flush changed the manifest"; fi
fi

unset LSMKV_MEMTABLE_MAX_BYTES
echo "----"
if [ $FAILURES -eq 0 ]; then echo "ALL DRILLS PASSED"; else echo "$FAILURES DRILL(S) FAILED"; fi
exit $FAILURES
