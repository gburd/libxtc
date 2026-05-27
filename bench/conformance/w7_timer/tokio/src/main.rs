/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * bench/conformance/w7_timer/tokio/src/main.rs
 *   M17 W7 — timer wheel benchmark, Tokio runtime.
 *
 *   Three phases:
 *     1. Schedule N sleep_until tasks.  Record per-spawn latency
 *        (time from before tokio::spawn to after it returns).
 *     2. Abort N/2 JoinHandles chosen by Fisher-Yates shuffle.
 *        Record per-abort latency.
 *     3. Await all non-aborted handles; each task reports how many
 *        nanoseconds after its scheduled deadline it actually woke.
 *
 *   Emits three M17 result lines on stdout:
 *     workload=W7 runtime=tokio_schedule ...
 *     workload=W7 runtime=tokio_cancel  ...
 *     workload=W7 runtime=tokio_fire    ...
 *
 * Build:
 *   cd bench/conformance/w7_timer/tokio && cargo build --release
 *
 * Usage:
 *   ./bench                   # N=100000 (default)
 *   ./bench --N=10000
 *   ./bench --params=N=10000
 */

use std::collections::HashSet;
use std::time::{Duration, Instant};

/* ---------------------------------------------------------------------- */
/* Argument parsing                                                         */
/* ---------------------------------------------------------------------- */

fn parse_n(args: &[String]) -> u64 {
    for a in args.iter().skip(1) {
        if let Some(v) = a.strip_prefix("--N=") {
            if let Ok(n) = v.parse::<u64>() {
                return n;
            }
        }
        if let Some(v) = a.strip_prefix("--params=N=") {
            if let Ok(n) = v.parse::<u64>() {
                return n;
            }
        }
        if let Some(rest) = a.strip_prefix("--params=") {
            for kv in rest.split(':') {
                if let Some(v) = kv.strip_prefix("N=") {
                    if let Ok(n) = v.parse::<u64>() {
                        return n;
                    }
                }
            }
        }
    }
    100_000
}

/* ---------------------------------------------------------------------- */
/* Minimal xorshift64 PRNG (no external deps)                              */
/* ---------------------------------------------------------------------- */

struct Rng {
    state: u64,
}

impl Rng {
    fn new() -> Self {
        let seed = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.subsec_nanos() as u64)
            .unwrap_or(12345)
            + 1;
        Self { state: seed }
    }

    fn next_u64(&mut self) -> u64 {
        let mut x = self.state;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.state = x;
        x
    }

    /// Uniform random in [lo, hi).
    fn next_range(&mut self, lo: u64, hi: u64) -> u64 {
        lo + self.next_u64() % (hi - lo)
    }
}

/* ---------------------------------------------------------------------- */
/* Fisher-Yates in-place shuffle                                            */
/* ---------------------------------------------------------------------- */

fn shuffle<T>(arr: &mut [T], rng: &mut Rng) {
    let n = arr.len();
    for i in (1..n).rev() {
        let j = rng.next_range(0, (i + 1) as u64) as usize;
        arr.swap(i, j);
    }
}

/* ---------------------------------------------------------------------- */
/* Percentile on a sorted slice (linear interpolation omitted; exact bin)  */
/* ---------------------------------------------------------------------- */

fn percentile(sorted: &[u64], pct: f64) -> u64 {
    if sorted.is_empty() {
        return 0;
    }
    let idx = ((pct / 100.0 * sorted.len() as f64) as usize)
        .saturating_sub(0)
        .min(sorted.len() - 1);
    sorted[idx]
}

/* ---------------------------------------------------------------------- */
/* Resource metrics                                                         */
/* ---------------------------------------------------------------------- */

fn peak_rss_kb() -> u64 {
    #[cfg(target_os = "linux")]
    {
        if let Ok(data) = std::fs::read_to_string("/proc/self/status") {
            for line in data.lines() {
                if let Some(rest) = line.strip_prefix("VmPeak:") {
                    return rest
                        .split_whitespace()
                        .next()
                        .and_then(|s| s.parse().ok())
                        .unwrap_or(0);
                }
            }
        }
    }
    0
}

fn cpu_us() -> u64 {
    #[cfg(target_os = "linux")]
    {
        if let Ok(data) = std::fs::read_to_string("/proc/self/stat") {
            let fields: Vec<&str> = data.split_whitespace().collect();
            // fields[13] = utime (jiffies), fields[14] = stime (jiffies)
            if fields.len() > 14 {
                let utime: u64 = fields[13].parse().unwrap_or(0);
                let stime: u64 = fields[14].parse().unwrap_or(0);
                // assume HZ=100; 1 jiffy = 10 ms = 10000 µs
                return (utime + stime) * 10_000;
            }
        }
    }
    0
}

/* ---------------------------------------------------------------------- */
/* main                                                                     */
/* ---------------------------------------------------------------------- */

#[tokio::main]
async fn main() {
    let args: Vec<String> = std::env::args().collect();
    let n = parse_n(&args) as usize;
    let n_cancel = n / 2;
    let n_fire = n - n_cancel;

    let mut rng = Rng::new();

    /* ================================================================== */
    /* Phase 1 — Schedule: spawn N sleep_until tasks                      */
    /* ================================================================== */

    let mut handles: Vec<tokio::task::JoinHandle<u64>> = Vec::with_capacity(n);
    let mut sched_lats: Vec<u64> = Vec::with_capacity(n);

    let t_sched0 = Instant::now();

    for _ in 0..n {
        /* Delay in [1 ms, 10 s] expressed as nanoseconds. */
        let delay_ns = rng.next_range(1_000_000, 10_001_000_000);
        let delay = Duration::from_nanos(delay_ns);

        /*
         * Record the std::Instant deadline and the equivalent
         * tokio::time::Instant for sleeping.  Both are captured
         * immediately before spawning so the deadline is as tight
         * as possible.
         */
        let std_deadline = Instant::now() + delay;
        let tokio_deadline = tokio::time::Instant::now() + delay;

        let spawn_t0 = Instant::now();
        let handle = tokio::spawn(async move {
            tokio::time::sleep_until(tokio_deadline).await;
            /*
             * Fire latency = actual wake time − scheduled deadline.
             * saturating_duration_since returns 0 if we woke early
             * (rare clock-read races).
             */
            Instant::now()
                .saturating_duration_since(std_deadline)
                .as_nanos() as u64
        });
        let sched_lat = Instant::now().duration_since(spawn_t0).as_nanos() as u64;

        sched_lats.push(sched_lat);
        handles.push(handle);
    }

    let elapsed_sched = t_sched0.elapsed().as_nanos() as u64;

    /* ================================================================== */
    /* Phase 2 — Cancel N/2 handles via abort()                           */
    /* ================================================================== */

    let mut order: Vec<usize> = (0..n).collect();
    shuffle(&mut order, &mut rng);
    let cancel_set: HashSet<usize> = order[..n_cancel].iter().copied().collect();

    let mut cancel_lats: Vec<u64> = Vec::with_capacity(n_cancel);

    let t_cancel0 = Instant::now();

    for &idx in &order[..n_cancel] {
        let c0 = Instant::now();
        handles[idx].abort();
        let c_lat = Instant::now().duration_since(c0).as_nanos() as u64;
        cancel_lats.push(c_lat);
    }

    let elapsed_cancel = t_cancel0.elapsed().as_nanos() as u64;

    /* ================================================================== */
    /* Phase 3 — Await non-cancelled handles; collect fire latencies      */
    /* ================================================================== */

    let t_fire0 = Instant::now();
    let mut fire_lats: Vec<u64> = Vec::with_capacity(n_fire);

    for (idx, handle) in handles.into_iter().enumerate() {
        if cancel_set.contains(&idx) {
            /*
             * Await the aborted handle to completion so Tokio can
             * reclaim the task resources.  This returns Err(Cancelled).
             */
            let _ = handle.await;
            continue;
        }
        if let Ok(late_ns) = handle.await {
            fire_lats.push(late_ns);
        }
    }

    let elapsed_fire = t_fire0.elapsed().as_nanos() as u64;

    /* ---- compute percentiles ---- */
    sched_lats.sort_unstable();
    cancel_lats.sort_unstable();
    fire_lats.sort_unstable();

    let rss = peak_rss_kb();
    let cpu = cpu_us();

    /* ================================================================== */
    /* Emit three M17 result lines                                        */
    /* ================================================================== */

    println!(
        "workload=W7 runtime=tokio_schedule params=N={n} \
         elapsed_ns={elapsed_sched} cpu_us=0 rss_kb=0 \
         p50_ns={} p95_ns={} p99_ns={} p999_ns={}",
        percentile(&sched_lats, 50.0),
        percentile(&sched_lats, 95.0),
        percentile(&sched_lats, 99.0),
        percentile(&sched_lats, 99.9),
    );

    println!(
        "workload=W7 runtime=tokio_cancel params=N={n_cancel} \
         elapsed_ns={elapsed_cancel} cpu_us=0 rss_kb=0 \
         p50_ns={} p95_ns={} p99_ns={} p999_ns={}",
        percentile(&cancel_lats, 50.0),
        percentile(&cancel_lats, 95.0),
        percentile(&cancel_lats, 99.0),
        percentile(&cancel_lats, 99.9),
    );

    println!(
        "workload=W7 runtime=tokio_fire params=N={n_fire} \
         elapsed_ns={elapsed_fire} cpu_us={cpu} rss_kb={rss} \
         p50_ns={} p95_ns={} p99_ns={} p999_ns={}",
        percentile(&fire_lats, 50.0),
        percentile(&fire_lats, 95.0),
        percentile(&fire_lats, 99.0),
        percentile(&fire_lats, 99.9),
    );
}
