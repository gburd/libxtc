#!/usr/bin/env escript
%%
%% Copyright (c) 2026, The XTC Project
%% Use of this source code is governed by the ISC License.
%%
%% bench/conformance/w1_spawn/erlang/main.erl
%%   M17 W1 -- spawn-N-await-all, Erlang/BEAM runtime.
%%
%%   Spawns N BEAM processes via spawn_monitor; each process does trivial
%%   work (sends one message then exits).  Waits for all 'DOWN' monitors,
%%   then emits one M17 result line on stdout.
%%
%% Usage (via wrapper script):
%%   ./bench              # N=10000 (default)
%%   ./bench --N=50000
%%   ./bench --params=N=50000
%%
%% Requires: OTP 24 or later.

-mode(compile).

main(Args) ->
    N = parse_n(Args),

    %% Wall-clock start (nanoseconds via erlang:monotonic_time).
    T0 = erlang:monotonic_time(nanosecond),

    %% CPU time baseline (milliseconds; user+sys via statistics/1).
    statistics(runtime),  %% reset the 'since last call' counter

    %% Spawn N processes and collect their monitor references.
    Refs = spawn_all(N, []),

    %% Wait for all processes to exit.
    wait_all(Refs),

    %% Wall-clock end.
    T1 = erlang:monotonic_time(nanosecond),
    ElapsedNs = T1 - T0,

    %% CPU time: statistics(runtime) returns {TotalMs, SinceLastMs}.
    {_Total, SinceLast} = statistics(runtime),
    CpuUs = SinceLast * 1000,

    %% Peak RSS: read VmPeak from /proc/self/status (Linux).
    %% Falls back to erlang:memory(total) / 1024 on non-Linux.
    RssKb = peak_rss_kb(),

    io:format("workload=W1 runtime=erlang params=N=~w"
              " elapsed_ns=~w cpu_us=~w rss_kb=~w"
              " p50_ns=0 p95_ns=0 p99_ns=0 p999_ns=0~n",
              [N, ElapsedNs, CpuUs, RssKb]).

%% ---------------------------------------------------------------------------
%% Spawn N processes, collecting {Pid, Ref} pairs.
%% ---------------------------------------------------------------------------

spawn_all(0, Acc) ->
    Acc;
spawn_all(N, Acc) ->
    {_Pid, Ref} = spawn_monitor(fun() -> ok end),
    spawn_all(N - 1, [Ref | Acc]).

%% ---------------------------------------------------------------------------
%% Wait for all monitors to fire.
%% ---------------------------------------------------------------------------

wait_all([]) ->
    ok;
wait_all([Ref | Rest]) ->
    receive
        {'DOWN', Ref, process, _Pid, _Reason} ->
            wait_all(Rest)
    end.

%% ---------------------------------------------------------------------------
%% Argument parsing: --N=<int> or --params=N=<int>
%% ---------------------------------------------------------------------------

parse_n([]) ->
    10000;
parse_n(["--N=" ++ V | _]) ->
    list_to_integer(V);
parse_n(["--params=N=" ++ V | _]) ->
    list_to_integer(V);
parse_n(["--params=" ++ Rest | _]) ->
    %% params may be KEY=VALUE:KEY=VALUE
    KVs = string:tokens(Rest, ":"),
    case [V || "N=" ++ V <- KVs] of
        [V | _] -> list_to_integer(V);
        []      -> 10000
    end;
parse_n([_ | Rest]) ->
    parse_n(Rest).

%% ---------------------------------------------------------------------------
%% Peak RSS in KiB.
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
            %% After looks like "  12345 kB"
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
