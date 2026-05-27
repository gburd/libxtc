/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * bench/conformance/w6_tail/tokio/src/main.rs
 *   W6: tail latency under backpressure — Tokio runtime.
 *
 *   N generator tasks each try to acquire an OwnedSemaphorePermit from a
 *   Semaphore(cap) using try_acquire_owned() (non-blocking).  If the cap
 *   is exhausted the task records a rejection and immediately continues —
 *   no waiting.  If the permit is obtained the task bundles it with a
 *   send-timestamp into a Msg and pushes it onto an unbounded MPSC channel
 *   to the consumer task.  The consumer receives each Msg, records the
 *   latency (now - msg.ts), then drops the Msg (which drops the permit,
 *   releasing one slot back to the Semaphore).
 *
 *   This is the idiomatic Tokio backpressure idiom: the Semaphore bounds
 *   the number of in-flight messages; callers that cannot acquire a permit
 *   observe backpressure immediately rather than queuing forever.
 *
 * Usage:
 *   ./bench [--gens=<int>] [--ops=<int>] [--cap=<int>]
 *   ./bench --params=gens=8:ops=1000000:cap=1000
 *   Defaults: gens=8, ops=1000000, cap=1000
 *
 * Output:
 *   workload=W6 runtime=tokio params=gens=8:ops=1000000:cap=1000
 *   elapsed_ns=... cpu_us=... rss_kb=...
 *   p50_ns=... p95_ns=... p99_ns=... p999_ns=... rejected=...
 */

use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Instant;

use hdrhistogram::Histogram;
use tokio::sync::{mpsc, Semaphore};

/* -------------------------------------------------------------------------
 * Message type
 *   The OwnedSemaphorePermit is dropped when Msg is dropped by the
 *   consumer, releasing one slot back to the Semaphore.
 * ------------------------------------------------------------------------- */

struct Msg {
    ts:      Instant,
    _permit: tokio::sync::OwnedSemaphorePermit,
}

/* -------------------------------------------------------------------------
 * Argument parsing
 * ------------------------------------------------------------------------- */

#[derive(Debug)]
struct Args {
    gens: u64,
    ops:  u64,
    cap:  usize,
}

impl Default for Args {
    fn default() -> Self {
        Args { gens: 8, ops: 1_000_000, cap: 1_000 }
    }
}

fn parse_args(raw: &[String]) -> Args {
    let mut a = Args::default();

    for arg in raw {
        if let Some(v) = arg.strip_prefix("--gens=") {
            if let Ok(n) = v.parse::<u64>() { a.gens = n.max(1); }
        } else if let Some(v) = arg.strip_prefix("--ops=") {
            if let Ok(n) = v.parse::<u64>() { a.ops = n.max(1); }
        } else if let Some(v) = arg.strip_prefix("--cap=") {
            if let Ok(n) = v.parse::<usize>() { a.cap = n.max(1); }
        } else if let Some(rest) = arg.strip_prefix("--params=") {
            for kv in rest.split(':') {
                if let Some(v) = kv.strip_prefix("gens=") {
                    if let Ok(n) = v.parse::<u64>() { a.gens = n.max(1); }
                } else if let Some(v) = kv.strip_prefix("ops=") {
                    if let Ok(n) = v.parse::<u64>() { a.ops = n.max(1); }
                } else if let Some(v) = kv.strip_prefix("cap=") {
                    if let Ok(n) = v.parse::<usize>() { a.cap = n.max(1); }
                }
            }
        }
    }

    a
}

/* -------------------------------------------------------------------------
 * Resource usage helpers
 * ------------------------------------------------------------------------- */

/// Returns (cpu_us, rss_kb) via getrusage(RUSAGE_SELF).
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

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

#[tokio::main]
async fn main() {
    let raw_args: Vec<String> = std::env::args().skip(1).collect();
    let args = parse_args(&raw_args);

    let gens = args.gens;
    let ops  = args.ops;
    let cap  = args.cap;

    /* Semaphore: cap permits = max cap in-flight messages */
    let sem      = Arc::new(Semaphore::new(cap));
    let rejected = Arc::new(AtomicU64::new(0));

    /*
     * Unbounded MPSC: the Semaphore is the backpressure mechanism;
     * the channel itself never blocks.
     */
    let (tx, mut rx) = mpsc::unbounded_channel::<Msg>();

    let t_start = Instant::now();

    /* ---- consumer task ---- */
    let consumer = tokio::spawn(async move {
        let mut hist = Histogram::<u64>::new_with_bounds(1, 60_000_000_000, 2)
            .expect("histogram init");

        while let Some(msg) = rx.recv().await {
            let lat_ns = msg.ts.elapsed().as_nanos() as u64;
            hist.record(lat_ns).unwrap_or(());
            /* msg dropped here: _permit released → Semaphore slot freed */
        }

        hist
    });

    /* ---- generator tasks ---- */
    let ops_per_gen = ops / gens;
    let remainder   = (ops % gens) as usize;
    let mut gen_handles = Vec::with_capacity(gens as usize);

    for i in 0..gens {
        let sem_clone      = Arc::clone(&sem);
        let tx_clone       = tx.clone();
        let rejected_clone = Arc::clone(&rejected);
        let my_ops: u64    = ops_per_gen + if (i as usize) < remainder { 1 } else { 0 };

        gen_handles.push(tokio::spawn(async move {
            for _ in 0..my_ops {
                match Arc::clone(&sem_clone).try_acquire_owned() {
                    Ok(permit) => {
                        /*
                         * Admitted: timestamp and forward to consumer.
                         * The permit travels with the Msg; consumer
                         * releases it on drop.
                         */
                        let msg = Msg { ts: Instant::now(), _permit: permit };
                        let _ = tx_clone.send(msg);
                    }
                    Err(_) => {
                        /*
                         * TryAcquireError::NoPermits: cap is exhausted.
                         * Record rejection and continue — no blocking.
                         *
                         * Yield to the Tokio scheduler so the consumer
                         * task gets CPU time to drain messages and release
                         * permits.  Without this, generator tasks (which
                         * have no other await points) starve the consumer
                         * in Tokio's cooperative runtime.
                         */
                        rejected_clone.fetch_add(1, Ordering::Relaxed);
                        tokio::task::yield_now().await;
                    }
                }
            }
            /* tx_clone dropped here; when last generator finishes all
             * clones are gone, rx.recv() returns None → consumer exits */
        }));
    }

    /* Drop the original sender so the consumer sees EOF once all
     * generator clones are also dropped. */
    drop(tx);

    /* Wait for generators first */
    for h in gen_handles {
        h.await.expect("generator task panicked");
    }

    /* Consumer exits when channel is empty and closed */
    let hist = consumer.await.expect("consumer task panicked");

    let elapsed_ns     = t_start.elapsed().as_nanos() as u64;
    let total_rejected = Arc::try_unwrap(rejected)
        .expect("Arc still shared")
        .into_inner();

    let (cpu_us, rss_kb) = resource_usage();

    /* ---- M17 results line ---- */
    println!(
        "workload=W6 runtime=tokio params=gens={}:ops={}:cap={} \
         elapsed_ns={} cpu_us={} rss_kb={} \
         p50_ns={} p95_ns={} p99_ns={} p999_ns={} rejected={}",
        gens,
        ops,
        cap,
        elapsed_ns,
        cpu_us,
        rss_kb,
        hist.value_at_percentile(50.0),
        hist.value_at_percentile(95.0),
        hist.value_at_percentile(99.0),
        hist.value_at_percentile(99.9),
        total_rejected,
    );
}
