#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# mTHP Autopilot A/B test. v3.1 architecture.
#
# CHANGES from the prior version:
#   (1) enabled = madvise (was: always). Under `always`, the kernel promotes
#       every eligible anon VMA regardless of userspace hints, so the daemon's
#       MADV_HUGEPAGE becomes a no-op and the test only measures bestfit
#       order-capping. Under `madvise`, the daemon is the sole promoter,
#       which is what the v3.1 design assumes.
#   (2) defrag = defer+madvise (was: only set once, ok). Kept.
#   (3) Baseline explicitly disables daemon AND bestfit module. Previous
#       script kept the daemon running in baseline, confusing A/B.
#   (4) Added optional llama-cli workload hook (see run_llama_ab.sh for the
#       full three-phase version).

set -u

DURATION=${DURATION:-60}
MIN_KB=${MIN_KB:-128}
MIN_PAGES=${MIN_PAGES:-2}
WORKLOAD=${WORKLOAD:-llama}   # llama | monkey | none

DAEMON=/data/local/tmp/mthp_classifier
BESTFIT_KO=/data/local/tmp/mthp_bestfit.ko

OUTDIR=/data/local/tmp/mthp_ab_$(date +%Y%m%d_%H%M%S)
mkdir -p "$OUTDIR"
LOG="$OUTDIR/report.txt"

THP_BASE=/sys/kernel/mm/transparent_hugepage
KHUGE_BASE=${THP_BASE}/khugepaged
BESTFIT_ENABLED=/sys/kernel/debug/mthp_bestfit/enabled
BESTFIT_STATS=/sys/kernel/debug/mthp_bestfit/stats
BESTFIT_LIVE=/sys/kernel/debug/mthp_bestfit/live

log() { echo "$*" | tee -a "$LOG"; }
hdr() { log; log "=== $* ==="; }
die() { echo "ERROR: $*" >&2; exit 1; }

[ "$(id -u)" = "0" ] || die "must run as root"
[ -f "$BESTFIT_KO" ] || die "bestfit module missing"
[ -x "$DAEMON" ]     || die "daemon missing"

# ---------------- snapshot ----------------

snap_raw() {
	{
		echo "# $(date)"
		for d in ${THP_BASE}/hugepages-*kB/stats; do
			[ -d "$d" ] || continue
			size=$(basename "$(dirname "$d")")
			a=$(cat "$d/anon_fault_alloc" 2>/dev/null || echo 0)
			f=$(cat "$d/anon_fault_fallback" 2>/dev/null || echo 0)
			s=$(cat "$d/split" 2>/dev/null || echo 0)
			sd=$(cat "$d/split_deferred" 2>/dev/null || echo 0)
			echo "$size alloc=$a fallback=$f split=$s split_deferred=$sd"
		done
		echo "# bestfit"
		cat "$BESTFIT_LIVE" 2>/dev/null || echo "(not loaded)"
		echo "# meminfo"
		grep -E 'MemAvailable|AnonPages|AnonHugePages' /proc/meminfo
	} > "$1"
}

get_field() {
	awk -v size="$2" -v field="$3" '
		$1==size { for(i=2;i<=NF;i++){split($i,a,"="); if(a[1]==field) {print a[2]; exit}} }
	' "$1"
}

# ---------------- workload ----------------

start_workload() {
	case "$WORKLOAD" in
	llama)
		[ -x /data/local/tmp/llama/llama-cli ] || { log "llama-cli not installed; skipping"; return; }
		[ -f /data/local/tmp/llama/model-q4.gguf ] || { log "model missing; skipping"; return; }
		/data/local/tmp/llama/llama-cli \
			-m /data/local/tmp/llama/model-q4.gguf \
			-p "Describe the ARM64 memory model in depth." \
			-n 256 -c 2048 -t 4 \
			> "$OUTDIR/llama_$1.log" 2>&1 &
		echo $! > "$OUTDIR/workload.pid"
		;;
	monkey)
		am start -n com.android.chrome/com.google.android.apps.chrome.Main >/dev/null 2>&1
		sleep 3
		monkey -p com.android.chrome --throttle 100 -v 99999 \
			> "$OUTDIR/monkey_$1.log" 2>&1 &
		echo $! > "$OUTDIR/workload.pid"
		;;
	none) : ;;
	esac
}

stop_workload() {
	[ -f "$OUTDIR/workload.pid" ] && {
		kill "$(cat "$OUTDIR/workload.pid")" 2>/dev/null || true
		rm -f "$OUTDIR/workload.pid"
	}
	case "$WORKLOAD" in
	monkey) am force-stop com.android.chrome >/dev/null 2>&1 ;;
	esac
	sleep 2
}

# ---------------- setup ----------------

hdr "Phase 0: setup (memory-first policy)"

pkill -f mthp_classifier 2>/dev/null || true
rmmod mthp_bestfit 2>/dev/null || true
sleep 1
insmod "$BESTFIT_KO" || die "bestfit insmod failed"

# Memory-first: madvise, not always.
echo madvise       > ${THP_BASE}/enabled
echo defer+madvise > ${THP_BASE}/defrag
for d in ${THP_BASE}/hugepages-*kB; do
	[ -w "$d/enabled" ] && echo madvise > "$d/enabled"
done
echo 1 > ${KHUGE_BASE}/defrag
echo "$MIN_PAGES" > /proc/sys/vm/mthp_bestfit/min_pages 2>/dev/null || true

# ---------------- baseline: daemon runs but bestfit disabled ----------------

hdr "Phase 1: baseline (daemon on, bestfit capping off)"

echo 0 > "$BESTFIT_ENABLED"
echo reset > "$BESTFIT_STATS" 2>/dev/null || true
echo 3 > /proc/sys/vm/drop_caches
sleep 2

snap_raw "$OUTDIR/baseline_start.txt"

"$DAEMON" --interval-ms 1000 --min-kb "$MIN_KB" --report-sec 0 \
	> "$OUTDIR/daemon_baseline.log" 2>&1 &
echo $! > "$OUTDIR/daemon.pid"
sleep 3

start_workload baseline
sleep "$DURATION"
stop_workload

kill "$(cat "$OUTDIR/daemon.pid")" 2>/dev/null || true
rm -f "$OUTDIR/daemon.pid"

snap_raw "$OUTDIR/baseline_end.txt"

# ---------------- autopilot: daemon + bestfit ----------------

hdr "Phase 2: autopilot (daemon on, bestfit capping on)"

echo 1 > "$BESTFIT_ENABLED"
echo reset > "$BESTFIT_STATS"
echo 3 > /proc/sys/vm/drop_caches
sleep 2

snap_raw "$OUTDIR/autopilot_start.txt"

"$DAEMON" --interval-ms 1000 --min-kb "$MIN_KB" --report-sec 0 \
	> "$OUTDIR/daemon_autopilot.log" 2>&1 &
echo $! > "$OUTDIR/daemon.pid"
sleep 3

start_workload autopilot
sleep "$DURATION"
stop_workload

kill "$(cat "$OUTDIR/daemon.pid")" 2>/dev/null || true
rm -f "$OUTDIR/daemon.pid"

snap_raw "$OUTDIR/autopilot_end.txt"

# ---------------- results ----------------

hdr "Phase 3: results"

delta() {
	local size=$1 field=$2 start=$3 end=$4 s e
	s=$(get_field "$start" "$size" "$field"); s=${s:-0}
	e=$(get_field "$end"   "$size" "$field"); e=${e:-0}
	echo $((e - s))
}

TOTAL_BA=0; TOTAL_AA=0; TOTAL_BS=0; TOTAL_AS=0
log "size                 base_alloc base_split  ap_alloc ap_split"
for size in hugepages-16kB hugepages-32kB hugepages-64kB hugepages-128kB \
            hugepages-256kB hugepages-512kB hugepages-1024kB hugepages-2048kB; do
	ba=$(delta "$size" alloc          "$OUTDIR/baseline_start.txt"  "$OUTDIR/baseline_end.txt")
	bs=$(delta "$size" split_deferred "$OUTDIR/baseline_start.txt"  "$OUTDIR/baseline_end.txt")
	aa=$(delta "$size" alloc          "$OUTDIR/autopilot_start.txt" "$OUTDIR/autopilot_end.txt")
	as=$(delta "$size" split_deferred "$OUTDIR/autopilot_start.txt" "$OUTDIR/autopilot_end.txt")
	TOTAL_BA=$((TOTAL_BA + ba)); TOTAL_BS=$((TOTAL_BS + bs))
	TOTAL_AA=$((TOTAL_AA + aa)); TOTAL_AS=$((TOTAL_AS + as))
	printf "  %-20s %10d %10d  %8d %8d\n" "$size" "$ba" "$bs" "$aa" "$as" | tee -a "$LOG"
done

BASE_RATE=$((TOTAL_BS * 100 / (TOTAL_BA + 1)))
AP_RATE=$((TOTAL_AS * 100 / (TOTAL_AA + 1)))
IMPROVEMENT=$((BASE_RATE - AP_RATE))

log ""
log "totals: BA=$TOTAL_BA AA=$TOTAL_AA BS=$TOTAL_BS AS=$TOTAL_AS"
log "baseline split_deferred rate:  $BASE_RATE%"
log "autopilot split_deferred rate: $AP_RATE%"

if [ "$IMPROVEMENT" -gt 0 ]; then
	log "VERDICT: PASS (split rate reduced by $IMPROVEMENT percentage points)"
elif [ "$IMPROVEMENT" -eq 0 ]; then
	log "VERDICT: NEUTRAL"
else
	abs=$((-IMPROVEMENT))
	log "VERDICT: REGRESSION (split rate increased by $abs percentage points)"
fi

log ""
log "bestfit final stats:"
cat $BESTFIT_STATS >> "$LOG" 2>/dev/null

tar -czf "${OUTDIR}.tar.gz" -C "$(dirname "$OUTDIR")" "$(basename "$OUTDIR")"
log "Archive: ${OUTDIR}.tar.gz"
