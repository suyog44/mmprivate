# mTHP Autopilot v3.1

## What changed from v2.x / v3.0

**v3.0 (vs v2.x):**
- Dropped BPF. Classifier is a pure `/proc/*/maps` + `process_madvise()` daemon.
- `mthp_bestfit.ko` remained kretprobe-based.

**v3.1 (this release):**
- Userspace now does two-way hinting: `MADV_HUGEPAGE` for large anon VMAs,
  `MADV_NOHUGEPAGE` for small ones (to prevent speculative promotion).
- Kernel module switched from kretprobe on `__thp_vma_allowable_orders`
  to the `android_vh_thp_vma_allowable_orders` vendor hook.
  No kallsyms workaround, no LLVM-LTO symbol rename hunt, no `maxactive`
  tuning. Clean module unload via `tracepoint_synchronize_unregister()`.
- New `thp_setup.sh` + `--setup` flag writes a memory-first sysfs policy.

## Files

| File                | Role                                          |
|---------------------|-----------------------------------------------|
| `mthp_classifier.c` | Userspace daemon (v3.1)                       |
| `Makefile`          | Userspace build (native + NDK arm64)          |
| `mthp_bestfit.c`    | Kernel module (v3.0, vendor-hook based)       |
| `Makefile.kmod`     | Kernel module Kbuild                          |
| `Kconfig`           | CONFIG_MTHP_BESTFIT                           |
| `thp_setup.sh`      | Standalone one-shot THP sysfs setup           |

## Architecture

```
 userspace                                       kernel
 ─────────────────────                          ─────────────────────

 mthp_classifier ──process_madvise──► VM_HUGEPAGE on VMA
   │                                  VM_NOHUGEPAGE on VMA
   │                                            │
   ▼                                            ▼
 /proc/*/maps                    __thp_vma_allowable_orders()
                                             │
                              (reads VM_HUGEPAGE via sysfs mask)
                                             │
                  android_vh_thp_vma_allowable_orders  ◄── mthp_bestfit.ko
                                             │              caps orders by
                                             ▼              VMA size
                                 folio allocation at chosen order
```

- **Userspace decides which VMAs deserve huge pages.** Hints via
  `MADV_HUGEPAGE` (large anon) and `MADV_NOHUGEPAGE` (small anon).
- **Kernel enforces the size policy.** The vendor hook fires every time
  `thp_vma_allowable_orders` runs and masks the returned bitmap to
  `(1 << (best_order + 1)) - 1` based on VMA size and `min_pages`.
- **mthp_autopilot.ko (optional)** exposes buddy/frag telemetry at
  `/sys/kernel/mthp_autopilot/system_info_bin`, which the daemon reads
  to tune khugepaged's `scan_sleep_millisecs` + `pages_to_scan`.

## Why no BPF

On Android GKI 6.12 with clang-LTO, `do_anonymous_page` is renamed to
`do_anonymous_page.llvm.<hash>`. libbpf's `SEC("kprobe/...")` attach does
exact symbol matching and silently fails. Tracepoints and BTF paths have
their own pitfalls (not a named BTF type, not in `/sys/kernel/tracing`,
static key never enabled, etc.).

Since mTHP promotion is a bias for *future* faults (via `VM_HUGEPAGE`),
not a per-fault decision, userspace doesn't need real-time event capture.
A 1-second poll over `/proc/*/maps` catches every long-lived VMA, which
is exactly the set worth hinting.

## Why the vendor hook instead of kretprobe

`android_vh_thp_vma_allowable_orders` was added to ACK in android15-6.6
(Aug 2024) and cherry-picked to android15-6.12 / android16-6.12. Signature:

```c
trace_android_vh_thp_vma_allowable_orders(struct vm_area_struct *vma,
                                          unsigned long *orders);
```

Advantages over kretprobe:

| Concern                           | kretprobe        | vendor hook      |
|-----------------------------------|------------------|------------------|
| LLVM-LTO symbol rename            | breaks attach    | compile-time OK  |
| Maxactive / nmissed               | must tune        | N/A              |
| Module unload safety              | kretprobe unreg  | tracepoint_sync  |
| Stable across kernel versions     | kallsyms-dependent | vendor ABI     |
| Overhead                          | breakpoint trap  | tracepoint call  |

Verify the hook is present on your tree:

```bash
grep android_vh_thp_vma_allowable_orders include/trace/hooks/mm.h \
     drivers/android/vendor_hooks.c
```

If it's missing, the ACK patch to add it is ~30 lines (declare in
`include/trace/hooks/mm.h`, call from `thp_vma_allowable_orders()` in
`include/linux/huge_mm.h` or `mm/huge_memory.c`, export in
`drivers/android/vendor_hooks.c`).

## Why `madvise`, not `always`

With memory saving as the primary goal, `transparent_hugepage=madvise` +
`defrag=defer+madvise` is the right setting. Rationale:

1. **No speculative waste.** A sparsely-touched 4 MB mmap'd region
   under `always` can consume a 2 MB folio for 1 byte of real use. Under
   `madvise`, the kernel only promotes regions explicitly hinted by
   userspace — which is exactly what the daemon controls.
2. **No latency spikes.** `defrag=always` causes synchronous compaction
   on unrelated faults. `defer+madvise` confines sync compaction to
   regions the daemon has flagged as worth the cost.
3. **Clean A/B comparison.** Under `always`, khugepaged, the fault path,
   and your daemon all compete to promote. Under `madvise`, the daemon
   is the only promoter, so benchmarks measure what you actually built.

## Build & deploy

### Userspace (Android, NDK)

```bash
make NDK=/path/to/android-ndk deploy
# Produces mthp_classifier.arm64, pushes to /data/local/tmp/
```

### Kernel module (out-of-tree)

```bash
make -C $KERNEL_SRC M=$PWD -f Makefile.kmod modules
# Produces mthp_bestfit.ko
adb push mthp_bestfit.ko /data/local/tmp/
adb shell 'su 0 insmod /data/local/tmp/mthp_bestfit.ko'
```

### Kernel module (in-tree / Kleaf)

Drop `mthp_bestfit.c`, `Makefile.kmod`, and `Kconfig` under
`drivers/mthp/` in your kernel tree. Add a `kernel_module` Bazel target
in your `BUILD.bazel` pointing at this directory, and add
`CONFIG_MTHP_BESTFIT=m` to your defconfig fragment.

## Run

### One-shot setup at boot (init.rc)

```
service thp_setup /vendor/bin/thp_setup.sh
    class core
    oneshot
    user root
```

### Daemon with setup

```bash
# First time, apply sysfs + run daemon:
adb shell 'su 0 /data/local/tmp/mthp_classifier --setup -v'

# Typical steady-state:
adb shell 'su 0 /data/local/tmp/mthp_classifier'

# A/B baseline:
adb shell 'su 0 /data/local/tmp/mthp_classifier --dry-run'
```

### Tune knobs

```bash
# Change min_pages threshold (lower = more aggressive)
echo 2 > /proc/sys/vm/mthp_bestfit/min_pages

# Disable contPTE bias (let kernel pick natural order)
echo 0 > /proc/sys/vm/mthp_bestfit/contpte_bias

# Disable the whole filter (A/B baseline without rmmod)
echo 0 > /sys/kernel/debug/mthp_bestfit/enabled
```

## Verify it's working

```bash
# Was the hook called?
cat /sys/kernel/debug/mthp_bestfit/stats | head -20
# decisions > 0 means the vendor hook is firing.

# What did it pick?
cat /sys/kernel/debug/mthp_bestfit/live
# Machine-parseable: order_N N-decisions

# Did MADV_HUGEPAGE stick?
grep -A1 AnonHugePages /proc/<PID>/smaps | head
# Or:
cat /proc/meminfo | grep -iE 'huge|anon'

# mTHP per-order accounting
grep anon_fault /sys/kernel/mm/transparent_hugepage/hugepages-*/stats/*
```

## Kernel config required

```
CONFIG_TRANSPARENT_HUGEPAGE=y
CONFIG_TRANSPARENT_HUGEPAGE_MADVISE=y     # default to madvise
CONFIG_ANDROID_VENDOR_HOOKS=y             # for android_vh_*
CONFIG_MTHP_BESTFIT=m                     # the order-capping module
CONFIG_DEBUG_FS=y                         # for /sys/kernel/debug knobs
```

And at runtime (or via `thp_setup.sh`):

```
echo madvise > /sys/kernel/mm/transparent_hugepage/enabled
echo defer+madvise > /sys/kernel/mm/transparent_hugepage/defrag
```
