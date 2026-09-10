#!/usr/bin/env bash
# Run a benchmark binary N times, stashing each run's fixed-name output CSV
# under a separate, numbered filename so results aren't overwritten. Also
# captures each run's "DROP_STATS received=<n> dropped=<n>" line (emitted by
# both socket_bench and rx_bench) into <output_dir>/drop_stats.csv.
#
# Usage:
#   run_repeated.sh <num_runs> <csv_produced_by_binary> <output_dir> -- <command...>
#
# Example:
#   ./run_repeated.sh 10 socket_bench_batch_latencies.csv runs/socket_batch \
#       -- sudo ./socket_bench enp7s0 5000 100000 batch
#
#   ./run_repeated.sh 10 rx_bench_latencies.csv runs/rx_bench \
#       -- sudo ./rx_bench -l 0,1 -n 4 -a 0000:07:00.0 -- 5000 100000

set -euo pipefail

if [[ $# -lt 4 ]]; then
    echo "Usage: $0 <num_runs> <csv_produced_by_binary> <output_dir> -- <command...>" >&2
    exit 1
fi

num_runs=$1
csv_name=$2
outdir=$3
shift 3

if [[ "${1:-}" != "--" ]]; then
    echo "error: expected '--' before the command" >&2
    exit 1
fi
shift

mkdir -p "$outdir"
summary="$outdir/summary.log"
: > "$summary"

drop_stats_csv="$outdir/drop_stats.csv"
echo "run,received,dropped" > "$drop_stats_csv"

for i in $(seq -w 1 "$num_runs"); do
    echo "=== run $i/$num_runs ===" | tee -a "$summary"

    set +e
    output=$("$@" 2>&1)
    cmd_status=$?
    set -e
    if [[ $cmd_status -ne 0 ]]; then
        echo "error: command exited with status $cmd_status on run $i:" >&2
        printf '%s\n' "$output" >&2
        exit 1
    fi
    printf '%s\n' "$output" | tee -a "$summary"

    if [[ ! -f "$csv_name" ]]; then
        echo "error: expected $csv_name after run $i, but it wasn't produced" >&2
        exit 1
    fi
    command mv -f "$csv_name" "$outdir/run${i}_${csv_name}"

    if grep -q "DROP_STATS" <<< "$output"; then
        drop_line=$(grep "DROP_STATS" <<< "$output" | tail -1)
        received=$(grep -oP 'received=\K[0-9]+' <<< "$drop_line")
        dropped=$(grep -oP 'dropped=\K[0-9]+' <<< "$drop_line")
        echo "$i,$received,$dropped" >> "$drop_stats_csv"
    else
        echo "warning: no DROP_STATS line found in run $i output" >&2
    fi
done

echo "Done. $num_runs run CSVs in $outdir/ (drop stats in $drop_stats_csv)"
