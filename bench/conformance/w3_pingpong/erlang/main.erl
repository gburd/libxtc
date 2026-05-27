#!/usr/bin/env escript
%%!
%%-
%% Copyright (c) 2026, The XTC Project
%% Use of this source code is governed by the ISC License.
%%
%% bench/conformance/w3_pingpong/erlang/main.erl
%%   W3: mailbox ping-pong benchmark -- Erlang runtime.
%%
%%   Two Erlang processes (ping, pong) exchange messages N times using
%%   the built-in "!" send and "receive" primitives.  Per-round-trip
%%   latency is measured with erlang:monotonic_time(nanosecond) and
%%   collected in a list; at the end the list is sorted and percentiles
%%   are extracted by index.
%%
%%   This file is a self-contained escript; no compiled .beam dependencies.
%%
%% Usage:
%%   escript main.erl [--N=<int>] [--params=N=<int>]
%%   Default N = 1 000 000

main(Args) ->
    N = parse_n(Args, 1000000),

    %% Start timing and CPU accounting.
    T0 = erlang:monotonic_time(nanosecond),
    erlang:statistics(runtime),  %% reset the runtime counter

    %% Spawn pong, then run ping in this process.
    Self = self(),
    Pong = spawn(fun() -> pong_loop(Self) end),
    receive pong_ready -> ok end,

    Latencies = ping_loop(Pong, N, N, []),

    T1      = erlang:monotonic_time(nanosecond),
    {CpuMs, _} = erlang:statistics(runtime),

    %% Signal pong to exit.
    Pong ! stop,

    ElapsedNs = T1 - T0,
    CpuUs     = CpuMs * 1000,
    RssKb     = rss_kb(),

    %% Compute percentiles from sorted latency list.
    Sorted = lists:sort(Latencies),
    Len    = length(Sorted),
    P50    = nth_pct(Sorted, Len, 50.0),
    P95    = nth_pct(Sorted, Len, 95.0),
    P99    = nth_pct(Sorted, Len, 99.0),
    P999   = nth_pct(Sorted, Len, 99.9),

    io:format(
        "workload=W3 runtime=erlang params=N=~w"
        " elapsed_ns=~w cpu_us=~w rss_kb=~w"
        " p50_ns=~w p95_ns=~w p99_ns=~w p999_ns=~w~n",
        [N, ElapsedNs, CpuUs, RssKb, P50, P95, P99, P999]).

%% -------------------------------------------------------------------------
%% pong process
%%   Signals readiness, then echoes {ping, From} messages with {pong}
%%   until it receives the atom 'stop'.
%% -------------------------------------------------------------------------

pong_loop(Parent) ->
    Parent ! pong_ready,
    pong_recv().

pong_recv() ->
    receive
        {ping, From} ->
            From ! pong,
            pong_recv();
        stop ->
            ok
    end.

%% -------------------------------------------------------------------------
%% ping loop (tail-recursive, runs in the main escript process)
%%   Sends {ping, self()} to Pong, waits for 'pong', records RTT.
%%   Returns accumulated latency list (in reverse arrival order).
%% -------------------------------------------------------------------------

ping_loop(_Pong, _N, 0, Acc) ->
    Acc;
ping_loop(Pong, N, Remaining, Acc) ->
    T0 = erlang:monotonic_time(nanosecond),
    Pong ! {ping, self()},
    receive pong -> ok end,
    T1  = erlang:monotonic_time(nanosecond),
    ping_loop(Pong, N, Remaining - 1, [T1 - T0 | Acc]).

%% -------------------------------------------------------------------------
%% Percentile from a sorted list.
%%   pct in [0.0, 100.0]; returns the value at the corresponding index.
%% -------------------------------------------------------------------------

nth_pct(_Sorted, 0, _Pct) ->
    0;
nth_pct(Sorted, Len, Pct) ->
    Idx0 = round(Pct / 100.0 * Len),
    Idx  = max(1, min(Idx0, Len)),
    lists:nth(Idx, Sorted).

%% -------------------------------------------------------------------------
%% Peak RSS (KiB) from /proc/self/status; 0 if unavailable.
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
%%   Handles --N=<int> and --params=N=<int>[:<key>=<val>...]
%% -------------------------------------------------------------------------

parse_n([], Default) ->
    Default;
parse_n(["--N=" ++ Val | _], _Default) ->
    list_to_integer(Val);
parse_n(["--params=" ++ Params | _Rest], Default) ->
    parse_params(string:tokens(Params, ":"), Default);
parse_n([_ | Rest], Default) ->
    parse_n(Rest, Default).

parse_params([], Default) ->
    Default;
parse_params(["N=" ++ Val | _], _Default) ->
    list_to_integer(Val);
parse_params([_ | Rest], Default) ->
    parse_params(Rest, Default).
