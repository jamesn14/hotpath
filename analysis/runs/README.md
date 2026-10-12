# Benchmark runs

Each folder is one test configuration. Every subfolder (`dpdk`, `socket_single`, `socket_batch`) holds 10 runs of
100,000 packets (5,000 warmup, so 95,000 samples per run) produced by `scripts/run_repeated.sh`: one latency CSV per
run (values in ns), `drop_stats.csv`, and `summary.log` with the full program output.

Two things change between folders:
- **Where the socket latency starts.** "After recv" means the receive thread stamped the packet after `recv()`/`recvmmsg()`
  returned, so kernel receive work is not included. "Kernel timestamp" means the packet is stamped by the kernel when it
  enters the network stack (`SO_TIMESTAMPNS`), so the window includes the socket queue, thread wakeup, copy and syscall
  return. DPDK always starts when `rte_eth_rx_burst` returns.
- **Pktgen offered load.** "Overload" is Pktgen at full rate (100%), which the socket path can't keep up with.

The kernel timestamp only applies to the socket path. DPDK's measurement is the same in every folder, so
`refactor_check/dpdk` (full rate) is the DPDK counterpart for `kernel_ts_overload`, recorded in an earlier session.

| Folder | Code | Socket start point | Pktgen load | Contents |
|---|---|---|---|---|
| `baseline` | Pre-refactor | After recv | Overload | Original README data and plot |
| `refactor_check` | CRTP library | After recv | Overload | Same test after the refactor, to check for regressions |
| `kernel_ts_overload` | CRTP library | Kernel timestamp | Overload | Socket only (DPDK counterpart: `refactor_check/dpdk`) |
| `kernel_ts_1pct` | CRTP library | Kernel timestamp | 1% rate, burst 32 | Socket only |
| `kernel_ts_0.1pct_burst1` | CRTP library | Kernel timestamp | 0.1% rate, burst 1 | DPDK and socket, same traffic |
| `old_single_runs` | Pre-refactor | After recv | Overload | Single early runs from before `run_repeated.sh`, not used in the README |

## Pooled results (all 10 runs combined)

| Folder | Path | p50 | p99 | p99.9 | Dropped |
|---|---|---|---|---|---|
| `baseline` | DPDK | 140 ns | 290 ns | 5.05 µs | 0% |
| | Socket single | 780 ns | 1.18 µs | 29.9 µs | 44.9% |
| | Socket batch | 870 ns | 1.34 µs | 35.0 µs | 38.9% |
| `refactor_check` | DPDK | 140 ns | 290 ns | 4.43 µs | 0% |
| | Socket single | 770 ns | 1.12 µs | 5.61 µs | 43.5% |
| | Socket batch | 850 ns | 1.38 µs | 11.5 µs | 34.9% |
| `kernel_ts_overload` | Socket single | 112.8 ms | 143.6 ms | 158.3 ms | 42.7% |
| | Socket batch | 69.2 ms | 95.8 ms | 100.1 ms | 36.5% |
| `kernel_ts_1pct` | Socket single | 39.8 µs | 102.0 µs | 116.6 µs | 0% |
| | Socket batch | 42.4 µs | 103.7 µs | 129.4 µs | 0% |
| `kernel_ts_0.1pct_burst1` | DPDK | 130 ns | 210 ns | 1.28 µs | 0% |
| | Socket single | 4.83 µs | 9.34 µs | 19.4 µs | 0% |
| | Socket batch | 5.18 µs | 10.0 µs | 19.9 µs | 0% |

Notes:
- `kernel_ts_overload` latencies are time spent waiting in the full 8 MB socket receive buffer (about 16k packets queued
  ahead of each one), not per-packet kernel cost.
- `kernel_ts_1pct` changed both rate and burst size compared with `kernel_ts_0.1pct_burst1`, so the drop from ~40 µs to
  ~5 µs can't be attributed to one of them alone.
- DPDK at full rate is in `refactor_check/dpdk` (0% dropped). DPDK was not run at the `kernel_ts_1pct` settings.