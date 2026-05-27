#!/usr/bin/env escript
%%
%% Copyright (c) 2026, The XTC Project — All rights reserved.
%% Use of this source code is governed by the ISC License.
%%
%% bench/conformance/w7_timer/erlang/main.erl
%%   M17 W7 — timer wheel benchmark, Erlang/BEAM runtime.
%%
%%   Three phases:
%%     1. Schedule N timers via erlang:start_timer/3 in [1 ms, 10 s].
%%        Record per-call latency.
%%     2. Cancel N/2 of them at random via erlang:cancel_timer/1.
%%        Record per-call latency.
%%     3. Receive all remaining timer messages; record fire-accuracy
%%        (nanoseconds late relative to the scheduled deadline).
%%
%%   Emits three M17 result lines on stdout:
%%     workload=W7 runtime=erlang_schedule ...
%%     workload=W7 runtime=erlang_cancel  ...
%%     workload=W7 runtime=erlang_fire    ...
%%
%% Usage (via wrapper script):
%%   ./bench               # N=100000 (default)
%%   ./bench --N=10000
%%   ./bench --params=N=10000
%%
%% Requires: OTP 21 or later (erlang:start_timer precision).

-mode(compile).

main(Args) ->
    N = parse_n(Args),

    %% Seed the PRNG so delays are different each run.
    rand:seed(exsss, {erlang:phash2([]), erlang:phash2([node()]),
                      erlang:monotonic_time()}),

    NCancel = N div 2,
    NFire0  = N - NCancel,   %% lower-bound; see cancel phase below

    %% ================================================================
    %% Phase 1 — Schedule N timers
    %% ================================================================
    %%
    %% Each timer entry: {Ref, DeadlineNs, SchedLatNs}
    %%   Ref        — cancel/fire handle
    %%   DeadlineNs — erlang:monotonic_time(nanosecond) + delay
    %%   SchedLatNs — wall time consumed by start_timer call

    T0Sched = erlang:monotonic_time(nanosecond),
    {Timers, SchedLats} = schedule_all(N),
    T1Sched = erlang:monotonic_time(nanosecond),
    ElapsedSched = T1Sched - T0Sched,

    %% ================================================================
    %% Phase 2 — Cancel N/2 at random
    %% ================================================================
    %%
    %% Shuffle the timer list and split.  erlang:cancel_timer returns:
    %%   integer  — ms remaining; timer cancelled successfully
    %%   false    — already fired; message already in our mailbox
    %%
    %% Track NActuallyCancelled so Phase 3 knows how many messages
    %% to receive.

    Shuffled = shuffle(Timers),
    {ToCancel, _ToKeep} = lists:split(NCancel, Shuffled),

    T0Cancel = erlang:monotonic_time(nanosecond),
    {CancelLats, NActuallyCancelled} = cancel_all(ToCancel),
    T1Cancel = erlang:monotonic_time(nanosecond),
    ElapsedCancel = T1Cancel - T0Cancel,

    NFireExpected = N - NActuallyCancelled,

    %% ================================================================
    %% Phase 3 — Receive fire messages
    %% ================================================================
    %%
    %% Each message: {timeout, Ref, DeadlineNs}
    %% Fire latency  = max(0, Now - DeadlineNs)  (nanoseconds)
    %%
    %% 35-second per-message safety timeout; normal completion is ≤10 s.

    T0Fire = erlang:monotonic_time(nanosecond),
    FireLats = receive_fires(NFireExpected, []),
    T1Fire = erlang:monotonic_time(nanosecond),
    ElapsedFire = T1Fire - T0Fire,

    %% ================================================================
    %% Resource usage
    %% ================================================================

    RssKb  = peak_rss_kb(),
    CpuUs  = cpu_us(),

    %% ================================================================
    %% Percentiles
    %% ================================================================

    P50Sched  = percentile(SchedLats,  50.0),
    P95Sched  = percentile(SchedLats,  95.0),
    P99Sched  = percentile(SchedLats,  99.0),
    P999Sched = percentile(SchedLats,  99.9),

    P50Cancel  = percentile(CancelLats,  50.0),
    P95Cancel  = percentile(CancelLats,  95.0),
    P99Cancel  = percentile(CancelLats,  99.0),
    P999Cancel = percentile(CancelLats,  99.9),

    P50Fire  = percentile(FireLats,  50.0),
    P95Fire  = percentile(FireLats,  95.0),
    P99Fire  = percentile(FireLats,  99.0),
    P999Fire = percentile(FireLats,  99.9),

    %% ================================================================
    %% Emit three M17 result lines
    %% ================================================================

    io:format("workload=W7 runtime=erlang_schedule params=N=~w"
              " elapsed_ns=~w cpu_us=0 rss_kb=0"
              " p50_ns=~w p95_ns=~w p99_ns=~w p999_ns=~w~n",
              [N, ElapsedSched,
               P50Sched, P95Sched, P99Sched, P999Sched]),

    io:format("workload=W7 runtime=erlang_cancel params=N=~w"
              " elapsed_ns=~w cpu_us=0 rss_kb=0"
              " p50_ns=~w p95_ns=~w p99_ns=~w p999_ns=~w~n",
              [NCancel, ElapsedCancel,
               P50Cancel, P95Cancel, P99Cancel, P999Cancel]),

    io:format("workload=W7 runtime=erlang_fire params=N=~w"
              " elapsed_ns=~w cpu_us=~w rss_kb=~w"
              " p50_ns=~w p95_ns=~w p99_ns=~w p999_ns=~w~n",
              [NFire0, ElapsedFire, CpuUs, RssKb,
               P50Fire, P95Fire, P99Fire, P999Fire]).

%% ---------------------------------------------------------------------------
%% Phase 1 helpers
%% ---------------------------------------------------------------------------

%% schedule_all/1 — schedule N timers; return {[{Ref,DeadlineNs}], [LatNs]}.
%%
%% erlang:start_timer(Time, Dest, Msg) requires Time in *milliseconds*
%% as a non-negative integer.  We draw from [1, 10000] ms = [1 ms, 10 s].
%% The deadline is captured just before start_timer so it matches what
%% xtc and Tokio record.

schedule_all(N) ->
    schedule_loop(N, [], []).

schedule_loop(0, Timers, Lats) ->
    {lists:reverse(Timers), lists:reverse(Lats)};
schedule_loop(N, Timers, Lats) ->
    DelayMs   = rand:uniform(10000),          %% [1, 10000] ms
    Tb        = erlang:monotonic_time(nanosecond),
    DeadlineNs = Tb + DelayMs * 1_000_000,
    Ref       = erlang:start_timer(DelayMs, self(), DeadlineNs),
    Ta        = erlang:monotonic_time(nanosecond),
    schedule_loop(N - 1,
                  [{Ref, DeadlineNs} | Timers],
                  [Ta - Tb           | Lats]).

%% ---------------------------------------------------------------------------
%% Phase 2 helpers
%% ---------------------------------------------------------------------------

%% cancel_all/1 — cancel each timer; return {[LatNs], NActuallyCancelled}.
%%   NActuallyCancelled counts only refs where cancel_timer returned an
%%   integer (message successfully removed from the timer wheel).

cancel_all(ToCancel) ->
    cancel_loop(ToCancel, [], 0).

cancel_loop([], Lats, NCancelled) ->
    {lists:reverse(Lats), NCancelled};
cancel_loop([{Ref, _DeadlineNs} | Rest], Lats, NCancelled) ->
    Tb  = erlang:monotonic_time(nanosecond),
    Res = erlang:cancel_timer(Ref),
    Ta  = erlang:monotonic_time(nanosecond),
    N2  = case Res of
              false -> NCancelled;      %% already fired; msg in mailbox
              _     -> NCancelled + 1  %% successfully cancelled
          end,
    cancel_loop(Rest, [Ta - Tb | Lats], N2).

%% ---------------------------------------------------------------------------
%% Phase 3 helpers
%% ---------------------------------------------------------------------------

%% receive_fires/2 — collect NExpected {timeout,_,DeadlineNs} messages.
%% Any extra messages from cancel_timer returning false (timer already fired)
%% are counted alongside the intentional fires.

receive_fires(0, Lats) ->
    Lats;
receive_fires(N, Lats) ->
    receive
        {timeout, _Ref, DeadlineNs} ->
            Now  = erlang:monotonic_time(nanosecond),
            Late = max(0, Now - DeadlineNs),
            receive_fires(N - 1, [Late | Lats])
    after 35000 ->
        %% Safety drain: should not trigger under normal operation
        %% (all timers fire within ≤10 s).
        Lats
    end.

%% ---------------------------------------------------------------------------
%% Shuffle — sort by random key (O(N log N), idiomatic in Erlang)
%% ---------------------------------------------------------------------------

shuffle([]) -> [];
shuffle(List) ->
    Tagged  = [{rand:uniform(), X} || X <- List],
    Sorted  = lists:sort(Tagged),
    [X || {_, X} <- Sorted].

%% ---------------------------------------------------------------------------
%% Percentile — sort-and-index, 0-based indexing clamped to bounds
%% ---------------------------------------------------------------------------

percentile([], _Pct) ->
    0;
percentile(Values, Pct) ->
    Sorted = lists:sort(Values),
    N      = length(Sorted),
    %% 1-based index: clamp to [1, N]
    Idx    = max(1, min(N, round(Pct * N / 100.0))),
    lists:nth(Idx, Sorted).

%% ---------------------------------------------------------------------------
%% Argument parsing: --N=<int> or --params=N=<int>
%% ---------------------------------------------------------------------------

parse_n([]) ->
    100000;
parse_n(["--N=" ++ V | _]) ->
    list_to_integer(V);
parse_n(["--params=N=" ++ V | _]) ->
    list_to_integer(V);
parse_n(["--params=" ++ Rest | _]) ->
    KVs = string:tokens(Rest, ":"),
    case [V || "N=" ++ V <- KVs] of
        [V | _] -> list_to_integer(V);
        []      -> 100000
    end;
parse_n([_ | Rest]) ->
    parse_n(Rest).

%% ---------------------------------------------------------------------------
%% Resource metrics
%% ---------------------------------------------------------------------------

peak_rss_kb() ->
    case file:read_file("/proc/self/status") of
        {ok, Data} ->
            Lines = string:tokens(binary_to_list(Data), "\n"),
            find_vmrss(Lines);
        _ ->
            erlang:memory(total) div 1024
    end.

find_vmrss([]) ->
    0;
find_vmrss([Line | Rest]) ->
    case string:prefix(Line, "VmRSS:") of
        nomatch ->
            find_vmrss(Rest);
        After ->
            Tokens = string:tokens(After, " \t"),
            case Tokens of
                [NumStr | _] ->
                    try list_to_integer(NumStr)
                    catch _:_ -> find_vmrss(Rest)
                    end;
                _ ->
                    find_vmrss(Rest)
            end
    end.

cpu_us() ->
    %% statistics(runtime) returns {TotalMs, SinceLastMs} of Erlang VM
    %% CPU time.  One call at the end captures cumulative VM time.
    {TotalMs, _} = statistics(runtime),
    TotalMs * 1000.
