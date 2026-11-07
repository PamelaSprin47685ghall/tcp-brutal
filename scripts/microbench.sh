#!/bin/sh
# microbench.sh - the host microbenchmark feedback loop.
#
#   scripts/microbench.sh run [--quick|--full] [--tag NAME]
#   scripts/microbench.sh baseline [--full] [--name NAME]
#   scripts/microbench.sh compare [BASE [NEW]] [--threshold PCT]
#   scripts/microbench.sh remote [options]      # real hosts, see benches/short.sh
#
# run       builds host/bench_micro and writes
#           benches/results/<UTC stamp>[-<tag>]/host.csv (+ host.log).
#           --quick (default) targets <= 2 s, --full <= 10 s.
# baseline  runs, then copies host.csv to benches/baseline/host.csv (or
#           benches/baseline/<name>.csv). One fixed path is the reference
#           every later compare uses.
# compare   joins two CSVs by name and prints the delta per benchmark:
#
#             name,base_ns,new_ns,delta_ns_pct,base_mb_s,new_mb_s,
#             delta_mb_pct,checksum,verdict
#
#           verdict is REGRESS when ns/op rose by more than the threshold
#           (default 10%), IMPROVE when it fell by more than that, NEW or
#           MISSING when a row exists on one side only, else stable.
#           The report is also saved next to NEW as compare-<stamp>.txt.
#
# There are no retries: a failed build or run exits non-zero and leaves the
# partial output for inspection. Noise rules live in benches/MICROBENCH.md.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
results=$root/benches/results
baseline_dir=$root/benches/baseline
bench=$root/host/bench_micro

die()
{
    echo "microbench.sh: $*" >&2
    exit 2
}

usage()
{
    sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
}

build()
{
    make -C "$root" --no-print-directory host-bench-micro >/dev/null
}

stamp_utc()
{
    date -u +%Y%m%dT%H%M%SZ
}

latest_dir()
{
    ls -1dt "$results"/*/ 2>/dev/null | head -1
}

cmd_run()
{
    mode=--quick
    tag=

    while [ $# -gt 0 ]; do
        case $1 in
        --quick) mode=--quick; shift ;;
        --full) mode=--full; shift ;;
        --tag) tag=$2; shift 2 ;;
        -h | --help) usage; exit 0 ;;
        *) die "run: unknown option $1" ;;
        esac
    done

    build
    stamp=$(stamp_utc)
    if [ -n "$tag" ]; then
        dir=$results/$stamp-$tag
    else
        dir=$results/$stamp
    fi
    mkdir -p "$dir"
    if [ -n "${MICROBENCH_CPU:-}" ] && command -v taskset >/dev/null 2>&1; then
        runner="taskset -c $MICROBENCH_CPU"
    else
        runner=
    fi
    if ! $runner "$bench" "$mode" >"$dir/host.csv" 2>"$dir/host.log"; then
        echo "microbench.sh: benchmark failed; partial output in $dir" >&2
        exit 1
    fi
    echo "$dir"
    echo "  host.csv: $(wc -l <"$dir/host.csv") lines, mode $mode" >&2
}

cmd_baseline()
{
    mode=--quick
    name=host

    while [ $# -gt 0 ]; do
        case $1 in
        --quick) mode=--quick; shift ;;
        --full) mode=--full; shift ;;
        --name) name=$2; shift 2 ;;
        -h | --help) usage; exit 0 ;;
        *) die "baseline: unknown option $1" ;;
        esac
    done

    dir=$("$0" run "$mode")
    mkdir -p "$baseline_dir"
    cp "$dir/host.csv" "$baseline_dir/$name.csv"
    cp "$dir/host.log" "$baseline_dir/$name.log"
    echo "baseline: $baseline_dir/$name.csv (from $dir)" >&2
}

cmd_compare()
{
    base=$baseline_dir/host.csv
    new=
    threshold=10
    nargs=0

    while [ $# -gt 0 ]; do
        case $1 in
        --threshold) threshold=$2; shift 2 ;;
        -h | --help) usage; exit 0 ;;
        *)
            [ -f "$1" ] || die "compare: not a file: $1"
            nargs=$((nargs + 1))
            if [ "$nargs" -eq 1 ]; then
                base=$1
            elif [ "$nargs" -eq 2 ]; then
                new=$1
            else
                die "compare: at most BASE and NEW"
            fi
            shift
            ;;
        esac
    done

    if [ -z "$new" ]; then
        d=$(latest_dir)
        [ -n "$d" ] || die "compare: no result directory under $results"
        new=$d/host.csv
    fi
    [ -f "$base" ] || die "compare: no baseline $base (run: microbench.sh baseline)"
    [ -f "$new" ] || die "compare: no result $new"

    out=$(dirname "$new")/compare-$(stamp_utc).txt
    awk -F, -v thresh="$threshold" '
        function num(s) { return s + 0 }
        NR == FNR {
            if (FNR == 1) next
            ns[$1] = num($3); mb[$1] = num($4); sum[$1] = $5
            if (!($1 in seen)) order[++nseen] = $1
            seen[$1] = 1
            next
        }
        FNR == 1 {
            printf "name,base_ns,new_ns,delta_ns_pct,base_mb_s,new_mb_s,delta_mb_pct,checksum,verdict\n"
            next
        }
        {
            n = $1; nns = num($3); nmb = num($4); nsum = $5
            if (!seen[n]) {
                printf "%s,,,,,%0.3f,,%s,NEW\n", n, nmb, nsum
                next
            }
            if (ns[n] > 0)
                d = (nns - ns[n]) * 100.0 / ns[n]
            else
                d = 0
            if (mb[n] > 0)
                dm = (nmb - mb[n]) * 100.0 / mb[n]
            else
                dm = 0
            verdict = "stable"
            if (d > thresh) verdict = "REGRESS"
            else if (d < -thresh) verdict = "IMPROVE"
            cs = (sum[n] == nsum) ? "same" : "CHANGED"
            printf "%s,%.3f,%.3f,%+.1f,%.3f,%.3f,%+.1f,%s,%s\n",
                   n, ns[n], nns, d, mb[n], nmb, dm, cs, verdict
            done[n] = 1
        }
        END {
            for (i = 1; i <= nseen; i++) {
                n = order[i]
                if (!(n in done))
                    printf "%s,%.3f,,,,,,,MISSING\n", n, ns[n]
            }
        }
    ' "$base" "$new" | tee "$out"
    echo "compare: $base vs $new (threshold ${threshold}%)" >&2
    echo "compare: saved $out" >&2
}

cmd=${1:-}
[ -n "$cmd" ] || { usage; exit 2; }
shift || true

case $cmd in
run) cmd_run "$@" ;;
baseline) cmd_baseline "$@" ;;
compare) cmd_compare "$@" ;;
remote)
    exec "$root/benches/short.sh" "$@"
    ;;
-h | --help) usage ;;
*) die "unknown command $cmd" ;;
esac
