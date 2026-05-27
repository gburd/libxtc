#!/usr/bin/env escript
%%!
%%-
%% Copyright (c) 2026, The XTC Project — All rights reserved.
%% Use of this source code is governed by the ISC License.
%%
%% bench/conformance/w4_mutex/erlang/main.erl
%%   W4: mutex contention benchmark — Erlang runtime.
%%
%%   Erlang has no first-class mutex.  The idiomatic serialization
%%   mechanism is a single-process actor: all writers send a message to
%%   a coordinator process which processes them one at a time (increment
%%   and reply) — this is Erlang's natural mutex substitute.
%%
%%   N worker processes each run a tight loop: send {incr, self()} to
%%   the coordinator, wait for {ack}, repeat ops/N times.  Per-round-
%%   trip latency is sampled 1-in-1000 via erlang:monotonic_time.
%%   After all workers finish, the coordinator's final counter is read
%%   and verified.
%%
%%   This file is a self-contained escript; no compiled .beam
%%   dependencies beyond the OTP stdlib.
%%
%% Usage:
%%   escript main.erl                         # threads=8, ops=100000
%%   escript main.erl --threads=4 --ops=10000
%%   escript main.erl --params=threads=4:ops=10000

main(Args) ->
    Threads = parse_int("threads", Args, 8),
    Ops     = parse_int("ops",     Args, 100000),

    PerWorker = Ops div Threads,
    ActualOps = PerWorker * Threads,

    T0 = erlang:monotonic_time(nanosecond),
    erlang:statistics(runtime),

    %% Spawn the coordinator (the "mutex").
    Self  = self(),
    Coord = spawn(fun() -> coordinator(Self, 0) end),
    receive coord_ready -> ok end,

    %% Spawn N worker processes.
    [spawn(fun() -> worker(Coord, Self, PerWorker, I) end)
     || I <- lists:seq(0, Threads - 1)],

    %% Collect one {done, Latencies} message from each worker.
    AllLatencies = collect_workers(Threads, []),

    %% Read and verify the final counter value.
    Coord ! {get_count, Self},
    receive
        {count, FinalCount} ->
            if FinalCount =/= ActualOps ->
                io:format(standard_error,
                    "w4/erlang: FAILED: counter=~w expected=~w~n",
                    [FinalCount, ActualOps]);
               true -> ok
            end
    end,

    %% Shut down the coordinator.
    Coord ! stop,

    T1 = erlang:monotonic_time(nanosecond),
    {CpuMs, _} = erlang:statistics(runtime),

    ElapsedNs = T1 - T0,
    CpuUs     = CpuMs * 1000,
    RssKb     = rss_kb(),

    %% Compute percentiles from the sampled latency list.
    Sorted = lists:sort(AllLatencies),
    Len    = length(Sorted),
    P50    = nth_pct(Sorted, Len, 50.0),
    P95    = nth_pct(Sorted, Len, 95.0),
    P99    = nth_pct(Sorted, Len, 99.0),
    P999   = nth_pct(Sorted, Len, 99.9),

    io:format(
        "workload=W4 runtime=erlang_actor params=threads=~w:ops=~w"
        " elapsed_ns=~w cpu_us=~w rss_kb=~w"
        " p50_ns=~w p95_ns=~w p99_ns=~w p999_ns=~w~n",
        [Threads, ActualOps,
         ElapsedNs, CpuUs, RssKb,
         P50, P95, P99, P999]).

%% -------------------------------------------------------------------------
%% coordinator — the Erlang "mutex"
%%   Signals readiness, then serialises all {incr, From} requests.
%%   Responds to {get_count, From} without incrementing.
%%   Exits cleanly on 'stop'.
%% -------------------------------------------------------------------------

coordinator(Parent, Counter) ->
    Parent ! coord_ready,
    coordinator_loop(Counter).

coordinator_loop(Counter) ->
    receive
        {incr, From} ->
            From ! ack,
            coordinator_loop(Counter + 1);
        {get_count, From} ->
            From ! {count, Counter},
            coordinator_loop(Counter);
        stop ->
            ok
    end.

%% -------------------------------------------------------------------------
%% worker — one writer process
%%   Sends PerWorker increments to the coordinator, sampling 1-in-1000
%%   for latency, then sends {done, Latencies} to Parent.
%% -------------------------------------------------------------------------

worker(Coord, Parent, PerWorker, WorkerIdx) ->
    StartSample = WorkerIdx * 97 + 1,
    Latencies   = worker_loop(Coord, PerWorker, StartSample, []),
    Parent ! {done, Latencies}.

worker_loop(_Coord, 0, _SampleN, Acc) ->
    Acc;
worker_loop(Coord, Remaining, SampleN, Acc) ->
    DoSample = (SampleN rem 1000) =:= 0,
    T0 = case DoSample of
        true  -> erlang:monotonic_time(nanosecond);
        false -> 0
    end,
    Coord ! {incr, self()},
    receive ack -> ok end,
    NewAcc = case DoSample of
        true ->
            T1 = erlang:monotonic_time(nanosecond),
            [T1 - T0 | Acc];
        false ->
            Acc
    end,
    worker_loop(Coord, Remaining - 1, SampleN + 1, NewAcc).

%% -------------------------------------------------------------------------
%% collect_workers — receive N {done, Latencies} messages from workers
%% -------------------------------------------------------------------------

collect_workers(0, Acc) ->
    Acc;
collect_workers(N, Acc) ->
    receive
        {done, Latencies} ->
            collect_workers(N - 1, Latencies ++ Acc)
    end.

%% -------------------------------------------------------------------------
%% nth_pct — pct-th percentile from a sorted list
%% -------------------------------------------------------------------------

nth_pct(_Sorted, 0, _Pct) ->
    0;
nth_pct(Sorted, Len, Pct) ->
    Idx0 = round(Pct / 100.0 * Len),
    Idx  = max(1, min(Idx0, Len)),
    lists:nth(Idx, Sorted).

%% -------------------------------------------------------------------------
%% rss_kb — peak RSS in KiB from /proc/self/status (Linux); 0 if unavailable
%% -------------------------------------------------------------------------

rss_kb() ->
    case file:read_file("/proc/self/status") of
        {ok, Data} ->
            find_rss(binary:split(Data, <<"\n">>, [global]));
        _ ->
            0
    end.

find_rss([]) ->
    0;
find_rss([<<"VmRSS:", Rest/binary>> | _]) ->
    Trimmed = string:trim(binary_to_list(Rest)),
    [Num | _] = string:tokens(Trimmed, " \t"),
    list_to_integer(Num);
find_rss([_ | T]) ->
    find_rss(T).

%% -------------------------------------------------------------------------
%% Argument parsing
%%   Handles --key=<int> and --params=key=<int>[:<key>=<int>...]
%% -------------------------------------------------------------------------

parse_int(Key, Args, Default) ->
    Prefix = "--" ++ Key ++ "=",
    ParamsPrefix = "--params=",
    parse_int_args(Key, Prefix, ParamsPrefix, Args, Default).

parse_int_args(_Key, _Prefix, _PP, [], Default) ->
    Default;
parse_int_args(Key, Prefix, PP, [Arg | Rest], Default) ->
    case lists:prefix(Prefix, Arg) of
        true ->
            list_to_integer(lists:nthtail(length(Prefix), Arg));
        false ->
            case lists:prefix(PP, Arg) of
                true ->
                    Params = lists:nthtail(length(PP), Arg),
                    parse_params(Key, string:tokens(Params, ":"), Default);
                false ->
                    parse_int_args(Key, Prefix, PP, Rest, Default)
            end
    end.

parse_params(_Key, [], Default) ->
    Default;
parse_params(Key, [KV | Rest], Default) ->
    Prefix = Key ++ "=",
    case lists:prefix(Prefix, KV) of
        true  -> list_to_integer(lists:nthtail(length(Prefix), KV));
        false -> parse_params(Key, Rest, Default)
    end.
