#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# llama-cli three-phase benchmark for mTHP Autopilot.
#
# Phase A: vanilla madvise policy. No daemon. No bestfit module.
#          Baseline: what Android does out of the box under madvise.
# Phase B: daemon running (MADV_HUGEPAGE/NOHUGEPAGE hints), no bestfit module.
#          Isolates the contribution of userspace hinting alone.
# Phase C: daemon + bestfit module. Full stack.
#          Isolates the additional contribution of kernel-side order capping.
#
# Three phases matter for the hackathon pitch: judges will ask "how much
# of your win comes from the userspace piece vs the kernel piece?"

set -u

# ---------------- CONFIG ----------------

MODEL=${MODEL:-/data/local/tmp/llama/model-q4.gguf}
LLAMA=${LLAMA:-/data/local/tmp/llama/llama-cli}
PROMPT=${PROMPT:-"Explain ARM64 contiguous PTEs and why they matter for transparent huge pages. Be thorough."}
N_PREDICT=${N_PREDICT:-256}
N_CTX=${N_CTX:-2048}
N_THREADS=${N_THREADS:-4}
REPEATS=${REPEATS:-3}

DAEMON=/data/local/tmp/mthp_classifier
BESTFIT_KO=/data/local/tmp/mthp_bestfit.ko

THP_BASE=/sys/kernel/mm/transparent_hugepage
KHUGE_BASE=${THP_BASE}/khugepaged
BESTFIT_DBG=/sys/kernel/debug/mthp_bestfit

OUTDIR=/data/local/tmp/llama_mthp_$(date +%Y%m%d_%H%M%S)
mkdir -p "$OUTDIR"
LOG="$OUTDIR/report.txt"

# ---------------- HELPERS ----------------

log() { echo "$*" | tee -a "$LOG"; }
hdr() { log; log "=== $* ==="; }
die() { echo "ERROR: $*" >&2; exit 1; }

[ "$(id -u)" = "0" ] || die "must run as root"
[ -x "$LLAMA" ]      || die "llama-cli not found at $LLAMA"
[ -f "$MODEL" ]      || die "model not found at $MODEL"
[ -x "$DAEMON" ]     || die "daemon not found at $DAEMON"
[ -f "$BESTFIT_KO" ] || die "bestfit module not found at $BESTFIT_KO"

# --- sysfs setup common to all phases: madvise policy ---
setup_madvise_policy() {
	echo madvise       > $THP_BASE/enabled
	echo defer+madvise > $THP_BASE/defrag
	for d in $THP_BASE/hugepages-*kB; do
		[ -w "$d/enabled" ] && echo madvise > "$d/enabled"
	done
	echo 1    > $KHUGE_BASE/defrag
	echo 2000 > $KHUGE_BASE/scan_sleep_millisecs
	echo 4096 > $KHUGE_BASE/pages_to_scan
	[ -w $THP_BASE/shmem_enabled ] && echo never > $THP_BASE/shmem_enabled
	echo 3 > /proc/sys/vm/drop_caches
	sleep 2
}

# --- snapshot system state at a moment ---
snap() {
	local tag=$1
	local out=$OUTDIR/snap_${tag}.txt
	{
		echo "# $tag $(date)"
		echo "## meminfo"
		grep -E 'MemTotal|MemFree|MemAvailable|AnonPages|AnonHuge|Shmem|Cached|Slab' /proc/meminfo
		echo "## thp per-size alloc/fallback/split"
		for d in $THP_BASE/hugepages-*kB/stats; do
			[ -d "$d" ] || continue
			size=$(basename "$(dirname "$d")")
			a=$(cat $d/anon_fault_alloc 2>/dev/null || echo 0)
			f=$(cat $d/anon_fault_fallback 2>/dev/null || echo 0)
			s=$(cat $d/split 2>/dev/null || echo 0)
			sd=$(cat $d/split_deferred 2>/dev/null || echo 0)
			echo "$size alloc=$a fallback=$f split=$s split_deferred=$sd"
		done
		echo "## bestfit"
		cat $BESTFIT_DBG/live 2>/dev/null || echo "(not loaded)"
		echo "## vmstat selected"
		grep -E 'thp_|compact_|nr_anon_transparent' /proc/vmstat
	} > "$out"
}

# --- capture per-process memory of running llama-cli ---
snap_proc() {
	local tag=$1 pid=$2
	local out=$OUTDIR/proc_${tag}.txt
	{
		echo "# $tag pid=$pid $(date)"
		cat /proc/$pid/status 2>/dev/null | grep -E 'VmRSS|VmHWM|RssAnon|RssFile|VmPeak|VmSwap'
		echo "## smaps_rollup"
		cat /proc/$pid/smaps_rollup 2>/dev/null | grep -E 'Rss|Pss|AnonHugePages|ShmemPmdMapped'
		echo "## top-5 largest anon VMAs"
		awk '$6=="" && $2~/w/ {sz=strtonum("0x"$1)*0; split($1,a,"-"); \
			sz=strtonum("0x"a[2])-strtonum("0x"a[1]); print sz, $0}' \
			/proc/$pid/maps 2>/dev/null | sort -rn | head -5
	} > "$out"
}

run_llama() {
	local tag=$1 iter=$2
	local llama_log=$OUTDIR/llama_${tag}_${iter}.log

	"$LLAMA" -m "$MODEL" -p "$PROMPT" \
		-n "$N_PREDICT" -c "$N_CTX" -t "$N_THREADS" \
		--no-mmap \
		> "$llama_log" 2>&1 &
	local pid=$!
	echo "$pid" > "$OUTDIR/llama.pid"

	# wait for model to load (first 4 GB mmap happens during load)
	sleep 15
	if kill -0 "$pid" 2>/dev/null; then
		snap_proc "${tag}_${iter}_mid" "$pid"
	fi

	wait "$pid" 2>/dev/null
	rm -f "$OUTDIR/llama.pid"

	# extract tokens/sec from llama.cpp output
	local tps
	tps=$(grep -oE 'eval time.*[0-9.]+ tokens per second' "$llama_log" | \
	      grep -oE '[0-9.]+' | tail -1)
	[ -z "$tps" ] && tps=0
	echo "$tps"
}

# ---------------- PHASE IMPLEMENTATIONS ----------------

phase_A_vanilla() {
	hdr "Phase A: vanilla madvise (no daemon, no bestfit)"
	pkill -f mthp_classifier 2>/dev/null || true
	rmmod mthp_bestfit 2>/dev/null || true
	setup_madvise_policy
	snap A_start

	local total=0
	for i in $(seq 1 "$REPEATS"); do
		log "  iter $i..."
		tps=$(run_llama A "$i")
		log "  tokens/sec: $tps"
		total=$(awk "BEGIN{print $total + $tps}")
	done
	snap A_end
	A_TPS=$(awk "BEGIN{printf \"%.2f\", $total / $REPEATS}")
	log "Phase A mean tokens/sec: $A_TPS"
}

phase_B_daemon_only() {
	hdr "Phase B: daemon only (no bestfit module)"
	rmmod mthp_bestfit 2>/dev/null || true
	setup_madvise_policy

	"$DAEMON" --interval-ms 1000 --min-kb 128 --no-khuge \
		> "$OUTDIR/daemon_B.log" 2>&1 &
	echo $! > "$OUTDIR/daemon.pid"
	sleep 3
	snap B_start

	local total=0
	for i in $(seq 1 "$REPEATS"); do
		log "  iter $i..."
		tps=$(run_llama B "$i")
		log "  tokens/sec: $tps"
		total=$(awk "BEGIN{print $total + $tps}")
	done
	snap B_end
	kill "$(cat "$OUTDIR/daemon.pid")" 2>/dev/null || true
	rm -f "$OUTDIR/daemon.pid"
	B_TPS=$(awk "BEGIN{printf \"%.2f\", $total / $REPEATS}")
	log "Phase B mean tokens/sec: $B_TPS"
}

phase_C_full_stack() {
	hdr "Phase C: daemon + bestfit module (full stack)"
	insmod "$BESTFIT_KO" || die "bestfit insmod failed"
	setup_madvise_policy
	echo reset > $BESTFIT_DBG/stats 2>/dev/null || true

	"$DAEMON" --interval-ms 1000 --min-kb 128 \
		> "$OUTDIR/daemon_C.log" 2>&1 &
	echo $! > "$OUTDIR/daemon.pid"
	sleep 3
	snap C_start

	local total=0
	for i in $(seq 1 "$REPEATS"); do
		log "  iter $i..."
		tps=$(run_llama C "$i")
		log "  tokens/sec: $tps"
		total=$(awk "BEGIN{print $total + $tps}")
	done
	snap C_end
	kill "$(cat "$OUTDIR/daemon.pid")" 2>/dev/null || true
	rm -f "$OUTDIR/daemon.pid"
	C_TPS=$(awk "BEGIN{printf \"%.2f\", $total / $REPEATS}")
	log "Phase C mean tokens/sec: $C_TPS"

	log "--- bestfit final stats ---"
	cat $BESTFIT_DBG/stats >> "$LOG" 2>/dev/null
}

# ---------------- ANALYSIS ----------------

analyze() {
	hdr "Analysis"

	delta_meminfo() {
		local key=$1 start=$2 end=$3
		local s e
		s=$(grep "^$key:" "$OUTDIR/snap_${start}.txt" | awk '{print $2}')
		e=$(grep "^$key:" "$OUTDIR/snap_${end}.txt"   | awk '{print $2}')
		s=${s:-0}; e=${e:-0}
		echo $((e - s))
	}

	delta_thp() {
		local phase=$1 size=$2 field=$3
		awk -v size="$size" -v field="$field" '
			$1==size { for(i=2;i<=NF;i++){split($i,a,"="); if(a[1]==field) {print a[2]; exit}} }
		' "$OUTDIR/snap_${phase}_end.txt"
	}

	for p in A B C; do
		log "  Phase $p:"
		log "    AnonHugePages delta (kB): $(delta_meminfo AnonHugePages ${p}_start ${p}_end)"
		log "    AnonPages     delta (kB): $(delta_meminfo AnonPages     ${p}_start ${p}_end)"
		log "    MemAvailable  delta (kB): $(delta_meminfo MemAvailable  ${p}_start ${p}_end)"
		for size in hugepages-64kB hugepages-128kB hugepages-256kB hugepages-2048kB; do
			a=$(delta_thp $p "$size" alloc)
			s=$(delta_thp $p "$size" split_deferred)
			log "    $size: alloc=$a split_deferred=$s"
		done
	done

	log ""
	log "Tokens/sec:"
	log "  A (vanilla):   $A_TPS"
	log "  B (daemon):    $B_TPS"
	log "  C (full):      $C_TPS"

	improvement_ba=$(awk "BEGIN{printf \"%.1f\", ($B_TPS - $A_TPS) / $A_TPS * 100}")
	improvement_ca=$(awk "BEGIN{printf \"%.1f\", ($C_TPS - $A_TPS) / $A_TPS * 100}")
	improvement_cb=$(awk "BEGIN{printf \"%.1f\", ($C_TPS - $B_TPS) / $B_TPS * 100}")

	log "  B/A = +${improvement_ba}%  (daemon alone)"
	log "  C/A = +${improvement_ca}%  (full stack)"
	log "  C/B = +${improvement_cb}%  (bestfit contribution)"
}

# ---------------- MAIN ----------------

phase_A_vanilla
phase_B_daemon_only
phase_C_full_stack
analyze

hdr "Done"
log "Output: $OUTDIR"
tar -czf "${OUTDIR}.tar.gz" -C "$(dirname "$OUTDIR")" "$(basename "$OUTDIR")"
log "Archive: ${OUTDIR}.tar.gz"
