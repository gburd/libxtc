/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * bench/conformance/w3_pingpong/tokio/src/main.rs
 *   W3: mailbox ping-pong benchmark — Tokio runtime.
 *
 *   Two tokio::spawn'd tasks exchange a small message N times via a
 *   pair of bounded mpsc channels (capacity 1, one per direction).
 *   The ping task measures round-trip latency with std::time::Instant
 *   and records each sample in an HDR histogram.  At the end it
 *   writes the M17 results line.
 *
 * Usage:
 *   ./bench [--N=<int>] [--params=N=<int>]
 *   Default N = 1 000 000
 */

use std::time::Instant;

use hdrhistogram::Histogram;
use tokio::sync::mpsc;

/* ------------------------------------------------------------------------- */
/* Argument parsing                                                           */
/* ------------------------------------------------------------------------- */

fn parse_n(args: &[String]) -> u64 {
    const DEFAULT_N: u64 = 1_000_000;

    for a in args {
        if let Some(val) = a.strip_prefix("--N=") {
            if let Ok(n) = val.parse::<u64>() {
                return n;
            }
        } else if let Some(rest) = a.strip_prefix("--params=") {
            for kv in rest.split(':') {
                if let Some(val) = kv.strip_prefix("N=") {
                    if let Ok(n) = val.parse::<u64>() {
                        return n;
                    }
                }
            }
        }
    }
    DEFAULT_N
}

/* ------------------------------------------------------------------------- */
/* Resource usage helpers                                                     */
/* ------------------------------------------------------------------------- */

/// Returns (cpu_us, rss_kb) via getrusage(RUSAGE_SELF).
/// On Linux ru_maxrss is in KiB; on macOS it is in bytes (we normalise).
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
/* pong task: echo each message; exit when channel closes                    */
/* ------------------------------------------------------------------------- */

async fn pong_task(mut rx: mpsc::Receiver<u64>, tx: mpsc::Sender<u64>) {
    while let Some(seq) = rx.recv().await {
        if tx.send(seq + 1).await.is_err() {
            break;
        }
    }
    /* rx closed from ping side (tx_to_pong dropped) → task exits. */
}

/* ------------------------------------------------------------------------- */
/* main                                                                       */
/* ------------------------------------------------------------------------- */

#[tokio::main]
async fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let n = parse_n(&args);

    /* Two bounded channels of capacity 1: ping→pong and pong→ping. */
    let (tx_to_pong, rx_by_pong) = mpsc::channel::<u64>(1);
    let (tx_to_ping, mut rx_by_ping) = mpsc::channel::<u64>(1);

    tokio::spawn(pong_task(rx_by_pong, tx_to_ping));

    /* HDR histogram: 1 ns to 60 s, 2 significant decimal digits. */
    let mut hist = Histogram::<u64>::new_with_bounds(1, 60_000_000_000, 2)
        .expect("histogram init");

    let t_start = Instant::now();

    for seq in 0..n {
        let t0 = Instant::now();
        tx_to_pong.send(seq).await.expect("send to pong");
        let _ = rx_by_ping.recv().await.expect("recv from pong");
        let rtt_ns = t0.elapsed().as_nanos() as u64;
        hist.record(rtt_ns).unwrap_or(());
    }

    let elapsed_ns = t_start.elapsed().as_nanos() as u64;

    /* Drop sender so pong_task sees channel closed and exits cleanly. */
    drop(tx_to_pong);

    let (cpu_us, rss_kb) = resource_usage();

    println!(
        "workload=W3 runtime=tokio params=N={} \
         elapsed_ns={} cpu_us={} rss_kb={} \
         p50_ns={} p95_ns={} p99_ns={} p999_ns={}",
        n,
        elapsed_ns,
        cpu_us,
        rss_kb,
        hist.value_at_percentile(50.0),
        hist.value_at_percentile(95.0),
        hist.value_at_percentile(99.0),
        hist.value_at_percentile(99.9),
    );
}
