# Benchmarking on native Ubuntu

The next steps (the V-cache replay, the order-book work, later core isolation) need a native Linux install on the benchmark machine. WSL2 is not enough, for three reasons:

- **Pinning picks virtual CPUs.** WSL2 is a Hyper-V VM, and Hyper-V moves its virtual CPUs across physical cores, so `--core=2` says nothing about which CCD runs the code.
- **No performance counters.** Without them, `perf stat` and `perf c2c` can't confirm the cache-miss explanations in [order_book.md](order_book.md) and [ring_buffer_v2.md](ring_buffer_v2.md).
- **Nested paging.** Every TLB miss in a VM walks two sets of page tables. That inflates exactly the cost the order-book work goes after.

The real-day figures so far (131 ns p50 per update) come from WSL2. Measure a new baseline natively before comparing anything against them.

## Install

- **OS:** Ubuntu Server 26.04 LTS, or 24.04 with the HWE kernel. Leave the desktop out, since fewer background processes means a quieter tail. Install it on its own drive or partition so that Windows stays bootable.
- **Packages:**
  ```bash
  sudo apt install build-essential cmake ninja-build clang git linux-tools-common linux-tools-generic
  git clone https://github.com/veetihietasalo/MiniHFT && cd MiniHFT
  ```
- **The NASDAQ sample day:** copy `12302019.NASDAQ_ITCH50.gz` over from Windows, or download it again from emi.nasdaq.com. To copy it, mount the Windows partition read-only: `sudo mount -o ro /dev/<partition> /mnt/win`. Keep the file gzipped, because the runner decompresses it on the fly.
- **Claude Code (optional):** install it with `curl -fsSL https://claude.ai/install.sh | bash`, then run `claude remote-control` in the repo folder. The session appears in the Claude Code app and can build, run and compare results on this machine without you at the keyboard. It sits idle while a benchmark runs.

## Run

```bash
bench/run_ccd_matrix.sh --dry-run          # topology, pairs and warnings; runs nothing
cmake --preset gcc-release && cmake --build --preset gcc-release
sudo sysctl -w kernel.sched_rt_runtime_us=-1
sudo bench/run_ccd_matrix.sh --no-build --itch="$HOME/12302019.NASDAQ_ITCH50.gz"
sudo sysctl -w kernel.sched_rt_runtime_us=950000
```

- **Why build first, then use sudo:** the build runs as you, so `build/` doesn't end up owned by root. `--high-priority` (SCHED_FIFO) needs root.
- **What it runs:** the same matrix as the Windows script, then the full day once on each CCD. Expect 10–15 minutes.
- **Output:** the log goes to `build/bench-results/`.

### Settings that change the numbers

The log header records all of these.

- **RT throttling.** By default the kernel can stop a SCHED_FIFO thread for up to 50 ms in every second, and a spinning benchmark thread runs into that. A test run in a container with throttling on showed a p99 of 40 ms and a max of 49 ms, consistent with that. That's why the commands above switch throttling off around the run and then restore it. No benchmark thread spins at that priority for more than about 2 s, and the replay runs at normal priority.
- **CPU frequency.** For steady clocks, run `sudo cpupower frequency-set -g performance`.
- **Virtualization.** If the header says anything other than `virtualization: none`, the CCD comparison isn't valid.
- **Later, for the tail work:** isolate the benchmark cores (`isolcpus=`, `nohz_full=` and `rcu_nocbs=` on the kernel command line), and move interrupts off them. Leave this out for the first runs, so the new baseline is comparable with the Windows numbers.
