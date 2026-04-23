#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Memory-first THP configuration for Android.
# Run once per boot (init.rc / boot-completed trigger).

set -e

THP=/sys/kernel/mm/transparent_hugepage

if [ ! -d "$THP" ]; then
	echo "THP not available on this kernel" >&2
	exit 1
fi

# System-wide: only promote where userspace has hinted.
echo madvise > "$THP/enabled"

# Defrag: async compaction for unhinted regions, sync only on MADV_HUGEPAGE.
# Never use 'always' on a memory-constrained device -- causes jank.
echo defer+madvise > "$THP/defrag"

# Per-mTHP-size: madvise for every supported size.
for d in "$THP"/hugepages-*kB; do
	[ -e "$d/enabled" ] && echo madvise > "$d/enabled"
done

# khugepaged: enabled, let the daemon tune scan rate.
echo 1 > "$THP/khugepaged/defrag"
echo 2000 > "$THP/khugepaged/scan_sleep_millisecs"
echo 4096 > "$THP/khugepaged/pages_to_scan"

# shmem: off by default on memory-constrained targets.
[ -e "$THP/shmem_enabled" ] && echo never > "$THP/shmem_enabled"

echo "THP configured for memory-first (madvise / defer+madvise)"
echo "current:"
echo "  enabled: $(cat "$THP/enabled")"
echo "  defrag:  $(cat "$THP/defrag")"
for d in "$THP"/hugepages-*kB; do
	[ -e "$d/enabled" ] || continue
	echo "  $(basename "$d"): $(cat "$d/enabled")"
done
