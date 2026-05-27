/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * bench/conformance/w4_mutex/tokio/src/main.rs
 *   W4: mutex contention benchmark -- Tokio runtime.
 *
 *   N tokio tasks contend for a shared u64 counter wrapped in an
 *   Arc<tokio::sync::Mutex<u64>>.  Each task runs a tight
 *   acquire-increment-release loop.  One in every 1000 iterations is
 *   timed with std::time::Instant and the result is recorded in an
 *   HDR histogram.
 *
 *   After all tasks complete the final counter value is verified
 *   against the expected total (mutual exclusion check), then one
 *   M17 line is written to stdout.
 *
 * Usage:
 *   ./bench                          # threads=8, ops=100000
 *   ./bench --threads=4 --ops=10000
 *   ./bench --params=threads=4:ops=10000
 */

use std::sync::Arc;
use std::time::Instant;

use hdrhistogram::Histogram;
use tokio::sync::Mutex;

/* ------------------------------------------------------------------------- */
/* Argument parsing                                                           */
/* ------------------------------------------------------------------------- */

fn parse_usize(key: &str, args: &[String], default: usize) -> usize {
    let prefix = format!("--{}=", key);
    for a in args {
        if let Some(val) = a.strip_prefix(&prefix) {
            if let Ok(n) = val.parse::<usize>() {
                return n;
            }
        } else if let Some(rest) = a.strip_prefix("--params=") {
            for kv in rest.split(':') {
                let kp = format!("{}=", key);
                if let Some(val) = kv.strip_prefix(&kp) {
                    if let Ok(n) = val.parse::<usize>() {
                        return n;
                    }
                }
            }
        }
    }
    default
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
/* main                                                                       */
/* ------------------------------------------------------------------------- */

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();

    let n_tasks: usize = parse_usize("threads", &args, 8).max(1);
    let ops: usize     = parse_usize("ops",     &args, 100_000).max(1);

    let per_task   = ops / n_tasks;
    let actual_ops = per_task * n_tasks;

    /* Build a multi-thread runtime with worker_threads == n_tasks so that
     * each Tokio task can make progress in parallel, matching the intent of
     * the xtc pthread-based contention test.  */
    let rt = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(n_tasks)
        .build()
        .expect("tokio runtime init");

    let (elapsed_ns, cpu_us, rss_kb, p50, p95, p99, p999, counter_val) =
        rt.block_on(async move {
            let counter = Arc::new(Mutex::new(0u64));

            let t_start = Instant::now();

            let handles: Vec<_> = (0..n_tasks)
                .map(|i| {
                    let counter = counter.clone();
                    tokio::spawn(async move {
                        let mut h =
                            Histogram::<u64>::new_with_bounds(1, 60_000_000_000, 2)
                                .expect("histogram init");
                        /* Stagger per-task sample windows to avoid all tasks
                         * sampling the same iteration simultaneously. */
                        let mut sample_n: u64 = i as u64 * 97 + 1;

                        for _ in 0..per_task {
                            sample_n += 1;
                            let do_sample = (sample_n % 1000) == 0;
                            let t0 = if do_sample {
                                Some(Instant::now())
                            } else {
                                None
                            };

                            {
                                let mut lock = counter.lock().await;
                                *lock += 1;
                                /* lock dropped here */
                            }

                            if let Some(t0) = t0 {
                                h.record(t0.elapsed().as_nanos() as u64).ok();
                            }
                        }
                        h
                    })
                })
                .collect();

            let mut merged =
                Histogram::<u64>::new_with_bounds(1, 60_000_000_000, 2)
                    .expect("merge histogram init");

            for handle in handles {
                if let Ok(h) = handle.await {
                    merged += &h;
                }
            }

            let elapsed_ns = t_start.elapsed().as_nanos() as u64;
            let (cpu_us, rss_kb) = resource_usage();

            /* Mutual exclusion check */
            let counter_val = *counter.lock().await;
            if counter_val != actual_ops as u64 {
                eprintln!(
                    "w4/tokio: FAILED mutual exclusion check: \
                     counter={} expected={}",
                    counter_val, actual_ops
                );
            }

            let p50  = merged.value_at_percentile(50.0);
            let p95  = merged.value_at_percentile(95.0);
            let p99  = merged.value_at_percentile(99.0);
            let p999 = merged.value_at_percentile(99.9);

            (elapsed_ns, cpu_us, rss_kb, p50, p95, p99, p999, counter_val)
        });

    let _ = counter_val; /* suppress unused-variable warning */

    println!(
        "workload=W4 runtime=tokio_mutex params=threads={}:ops={} \
         elapsed_ns={} cpu_us={} rss_kb={} \
         p50_ns={} p95_ns={} p99_ns={} p999_ns={}",
        n_tasks,
        actual_ops,
        elapsed_ns,
        cpu_us,
        rss_kb,
        p50,
        p95,
        p99,
        p999,
    );
}
