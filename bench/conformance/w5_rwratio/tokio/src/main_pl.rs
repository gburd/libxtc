// Copyright (c) 2026, The XTC Project
// Use of this source code is governed by the ISC License.
//
// W5 reader/writer ratio sweep: parking_lot::RwLock variant.
//
// The default tokio bench uses tokio::sync::RwLock which is an async
// reader/writer latch; every acquire goes through the task scheduler
// rather than directly to the OS, so it is much slower on the short
// critical sections this workload uses.  This binary uses
// parking_lot::RwLock -- a raw OS reader/writer lock with the std API
// shape -- for the *fair* comparison against the raw-OS / wait-free
// read side (xtc_lrlock, and the shared path of xtc_arwlock off a loop).

use hdrhistogram::Histogram;
use parking_lot::RwLock;
use std::env;
use std::fs::File;
use std::io::{BufRead, BufReader};
use std::sync::Arc;
use std::thread;
use std::time::Instant;

fn rss_kb() -> u64 {
    File::open("/proc/self/status").ok().and_then(|f| {
        BufReader::new(f).lines().filter_map(|l| l.ok()).find_map(|l| {
            l.strip_prefix("VmRSS:").and_then(|rest| {
                rest.split_whitespace().next()?.parse::<u64>().ok()
            })
        })
    }).unwrap_or(0)
}

fn cpu_us() -> u64 {
    let mut ts: libc::timespec = unsafe { std::mem::zeroed() };
    let rc = unsafe {
        libc::clock_gettime(libc::CLOCK_PROCESS_CPUTIME_ID, &mut ts)
    };
    if rc != 0 { return 0; }
    (ts.tv_sec as u64) * 1_000_000 + (ts.tv_nsec as u64 / 1000)
}

fn run_bench(n_threads: usize, ops: u64, ratio: u64) {
    let per_task = ops / (n_threads as u64);
    let total_ops = per_task * (n_threads as u64);

    let counter = Arc::new(RwLock::new(0u64));
    let cpu0 = cpu_us();
    let t0 = Instant::now();

    let handles: Vec<_> = (0..n_threads)
        .map(|idx| {
            let counter = counter.clone();
            thread::spawn(move || {
                let mut h = Histogram::<u64>::new_with_bounds(1, 60_000_000_000, 2)
                    .unwrap();
                let mut sample_n: u64 = idx as u64 * 97 + 1;
                let mut phase: u64 = 0;
                let mut writes: u64 = 0;
                let mut sink: u64 = 0;

                for _ in 0..per_task {
                    sample_n += 1;
                    let do_sample = (sample_n % 1000) == 0;
                    let is_write = phase >= ratio;
                    let t = if do_sample { Some(Instant::now()) } else { None };

                    if is_write {
                        let mut g = counter.write();
                        *g += 1;
                        writes += 1;
                        phase = 0;
                    } else {
                        let g = counter.read();
                        sink = sink.wrapping_add(*g);
                        phase += 1;
                    }

                    if let Some(t) = t {
                        let _ = h.record(t.elapsed().as_nanos() as u64);
                    }
                }
                (h, writes, sink)
            })
        })
        .collect();

    let mut merged = Histogram::<u64>::new_with_bounds(1, 60_000_000_000, 2).unwrap();
    let mut total_writes: u64 = 0;
    let mut sink_total: u64 = 0;
    for handle in handles {
        let (h, w, s) = handle.join().unwrap();
        merged.add(&h).unwrap();
        total_writes += w;
        sink_total = sink_total.wrapping_add(s);
    }
    let _ = sink_total;

    let elapsed = t0.elapsed();
    let cpu1 = cpu_us();
    let val = *counter.read();
    assert_eq!(val, total_writes);

    println!(
        "workload=W5 runtime=tokio_pl_rwlock params=threads={}:ops={}:ratio={} elapsed_ns={} cpu_us={} rss_kb={} p50_ns={} p95_ns={} p99_ns={} p999_ns={}",
        n_threads, total_ops, ratio,
        elapsed.as_nanos(),
        cpu1.saturating_sub(cpu0),
        rss_kb(),
        merged.value_at_quantile(0.50),
        merged.value_at_quantile(0.95),
        merged.value_at_quantile(0.99),
        merged.value_at_quantile(0.999),
    );
}

fn main() {
    let args: Vec<String> = env::args().collect();
    let mut n_threads: usize = 8;
    let mut ops: u64 = 100_000;
    let mut ratio: Option<u64> = None;
    for a in &args[1..] {
        if let Some(v) = a.strip_prefix("--threads=") {
            n_threads = v.parse().unwrap_or(8);
        } else if let Some(v) = a.strip_prefix("--ops=") {
            ops = v.parse().unwrap_or(100_000);
        } else if let Some(v) = a.strip_prefix("--ratio=") {
            ratio = v.parse().ok();
        }
    }
    if n_threads < 1 { n_threads = 1; }
    match ratio {
        Some(r) => run_bench(n_threads, ops, r.max(1)),
        None => {
            for &r in &[1u64, 10, 100] {
                run_bench(n_threads, ops, r);
            }
        }
    }
}
