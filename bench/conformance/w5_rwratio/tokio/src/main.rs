/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * bench/conformance/w5_rwratio/tokio/src/main.rs
 *   W5: reader/writer ratio sweep benchmark -- Tokio runtime.
 *
 *   N tokio tasks share one u64 counter wrapped in an
 *   Arc<tokio::sync::RwLock<u64>>.  Each task runs `ops` operations;
 *   for every (ratio + 1) operations, `ratio` are reads (shared) and 1
 *   is a write (exclusive).  Readers observe the counter, writers
 *   increment it.  One in every 1000 operations is timed with
 *   std::time::Instant and recorded in an HDR histogram.
 *
 *   After all tasks complete, the final counter is verified against the
 *   total number of writes issued (mutual-exclusion check), then one
 *   M17 line per ratio is written to stdout.
 *
 *   This mirrors the xtc arwlock variant: an async, task-parking
 *   reader/writer latch.  See main_pl.rs for the parking_lot::RwLock
 *   variant, which mirrors the wait-free / raw-OS read side.
 *
 * Usage:
 *   ./bench                            # threads=8, ops=100000, ratios 1:10:100
 *   ./bench --threads=4 --ops=10000 --ratio=10
 *   ./bench --params=threads=4:ops=10000:ratio=10
 */

use std::sync::Arc;
use std::time::Instant;

use hdrhistogram::Histogram;
use tokio::sync::RwLock;

/* ------------------------------------------------------------------------- */
/* Argument parsing                                                           */
/* ------------------------------------------------------------------------- */

fn parse_usize(key: &str, args: &[String], default: usize) -> Option<usize> {
    let prefix = format!("--{}=", key);
    for a in args {
        if let Some(val) = a.strip_prefix(&prefix) {
            if let Ok(n) = val.parse::<usize>() {
                return Some(n);
            }
        } else if let Some(rest) = a.strip_prefix("--params=") {
            for kv in rest.split(':') {
                let kp = format!("{}=", key);
                if let Some(val) = kv.strip_prefix(&kp) {
                    if let Ok(n) = val.parse::<usize>() {
                        return Some(n);
                    }
                }
            }
        }
    }
    let _ = default;
    None
}

/* ------------------------------------------------------------------------- */
/* Resource usage helpers                                                     */
/* ------------------------------------------------------------------------- */

/// Returns (cpu_us, rss_kb) from getrusage(RUSAGE_SELF).
/// On Linux ru_maxrss is already KiB; on macOS it is bytes.
fn resource_usage() -> (u64, u64) {
    let mut ru: libc::rusage = unsafe { std::mem::zeroed() };
    unsafe { libc::getrusage(libc::RUSAGE_SELF, &mut ru) };

    let cpu_us = (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) as u64 * 1_000_000
        + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) as u64;

    #[cfg(target_os = "macos")]
    let rss_kb = ru.ru_maxrss as u64 / 1024;
    #[cfg(not(target_os = "macos"))]
    let rss_kb = ru.ru_maxrss as u64; /* Linux: already KiB */

    (cpu_us, rss_kb)
}

/* ------------------------------------------------------------------------- */
/* One ratio point                                                            */
/* ------------------------------------------------------------------------- */

fn run_bench(n_tasks: usize, ops: usize, ratio: usize) {
    let per_task   = ops / n_tasks;
    let actual_ops = per_task * n_tasks;

    /* Build a multi-thread runtime with worker_threads == n_tasks so that
     * each Tokio task can make progress in parallel, matching the intent of
     * the xtc pthread-based contention test.  */
    let rt = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(n_tasks)
        .build()
        .expect("tokio runtime init");

    let (elapsed_ns, cpu_us, rss_kb, p50, p95, p99, p999) =
        rt.block_on(async move {
            let counter = Arc::new(RwLock::new(0u64));

            let t_start = Instant::now();

            let handles: Vec<_> = (0..n_tasks)
                .map(|i| {
                    let counter = counter.clone();
                    tokio::spawn(async move {
                        let mut h =
                            Histogram::<u64>::new_with_bounds(1, 60_000_000_000, 2)
                                .expect("histogram init");
                        let mut sample_n: u64 = i as u64 * 97 + 1;
                        let mut phase: usize = 0;
                        let mut writes: u64 = 0;
                        let mut sink: u64 = 0;

                        for _ in 0..per_task {
                            sample_n += 1;
                            let do_sample = (sample_n % 1000) == 0;
                            let is_write = phase >= ratio;
                            let t0 = if do_sample {
                                Some(Instant::now())
                            } else {
                                None
                            };

                            if is_write {
                                let mut g = counter.write().await;
                                *g += 1;
                                writes += 1;
                                phase = 0;
                            } else {
                                let g = counter.read().await;
                                sink = sink.wrapping_add(*g);
                                phase += 1;
                            }

                            if let Some(t0) = t0 {
                                h.record(t0.elapsed().as_nanos() as u64).ok();
                            }
                        }
                        (h, writes, sink)
                    })
                })
                .collect();

            let mut merged =
                Histogram::<u64>::new_with_bounds(1, 60_000_000_000, 2)
                    .expect("merge histogram init");
            let mut total_writes: u64 = 0;
            let mut sink_total: u64 = 0;

            for handle in handles {
                if let Ok((h, w, s)) = handle.await {
                    merged += &h;
                    total_writes += w;
                    sink_total = sink_total.wrapping_add(s);
                }
            }
            let _ = sink_total; /* correctness sink; suppress unused warning */

            let elapsed_ns = t_start.elapsed().as_nanos() as u64;
            let (cpu_us, rss_kb) = resource_usage();

            /* Mutual exclusion check */
            let counter_val = *counter.read().await;
            if counter_val != total_writes {
                eprintln!(
                    "w5/tokio: FAILED mutual exclusion check: \
                     counter={} expected={}",
                    counter_val, total_writes
                );
            }

            let p50  = merged.value_at_percentile(50.0);
            let p95  = merged.value_at_percentile(95.0);
            let p99  = merged.value_at_percentile(99.0);
            let p999 = merged.value_at_percentile(99.9);

            (elapsed_ns, cpu_us, rss_kb, p50, p95, p99, p999)
        });

    println!(
        "workload=W5 runtime=tokio_rwlock params=threads={}:ops={}:ratio={} \
         elapsed_ns={} cpu_us={} rss_kb={} \
         p50_ns={} p95_ns={} p99_ns={} p999_ns={}",
        n_tasks, actual_ops, ratio,
        elapsed_ns, cpu_us, rss_kb,
        p50, p95, p99, p999,
    );
}

/* ------------------------------------------------------------------------- */
/* main                                                                       */
/* ------------------------------------------------------------------------- */

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();

    let n_tasks: usize = parse_usize("threads", &args, 8).unwrap_or(8).max(1);
    let ops: usize     = parse_usize("ops",     &args, 100_000).unwrap_or(100_000).max(1);
    let ratio_arg      = parse_usize("ratio",   &args, 0);

    match ratio_arg {
        Some(r) => run_bench(n_tasks, ops, r.max(1)),
        None => {
            for &r in &[1usize, 10, 100] {
                run_bench(n_tasks, ops, r);
            }
        }
    }
}
