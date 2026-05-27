#!/usr/bin/env escript
%%!-
%% Copyright (c) 2026, The XTC Project
%% Use of this source code is governed by the ISC License.
%%
%% bench/conformance/w2_echo/erlang/main.erl
%%   M17 W2 -- TCP echo server, Erlang/BEAM runtime.
%%
%%   Architecture:
%%     - Server: gen_tcp:listen + accept loop, one echo process per conn.
%%     - Clients: N processes, gen_tcp:connect + send/recv loop.
%%     - Latency: per-RTT via erlang:monotonic_time(nanosecond).
%%
%% Usage:
%%   escript main.erl [--clients=N] [--msgs=M]
%%   escript main.erl --params=clients=N:msgs=M

-module(main).
-export([main/1]).

%% ============================================================
%% Entry point
%% ============================================================

main(Args) ->
    {Clients, Msgs} = parse_args(Args),

    %% ETS table for RTT samples (bag allows duplicate keys).
    Tid = ets:new(samples, [bag, public]),

    %% Start listening on an ephemeral port (0 = OS picks).
    {ok, LSock} = gen_tcp:listen(0, [
        binary, {active, false},
        {reuseaddr, true},
        {nodelay, true},
        {backlog, 4096}
    ]),
    {ok, {_, Port}} = inet:sockname(LSock),

    %% Start accept-loop process.
    spawn(fun() -> accept_loop(LSock) end),

    %% Coordinator: collect {done, _Pid} from each client.
    Self = self(),

    %% Wall-clock start.
    T0 = erlang:monotonic_time(nanosecond),

    %% Spawn client processes.
    lists:foreach(
        fun(_) ->
            spawn(fun() -> client_proc(Self, Port, Msgs, Tid) end)
        end,
        lists:seq(1, Clients)
    ),

    %% Wait for all clients to finish.
    wait_clients(Clients),

    T1 = erlang:monotonic_time(nanosecond),
    ElapsedNs = T1 - T0,

    %% Gather all RTT samples.
    AllSamples = [V || {_K, V} <- ets:tab2list(Tid)],

    %% Resource usage.
    {CpuUs, RssKb} = resource_usage(),

    %% Percentiles.
    Sorted = lists:sort(AllSamples),
    N      = length(Sorted),
    P50  = percentile(Sorted, N, 50.0),
    P95  = percentile(Sorted, N, 95.0),
    P99  = percentile(Sorted, N, 99.0),
    P999 = percentile(Sorted, N, 99.9),

    io:format(
        "workload=W2 runtime=erlang params=clients=~w:msgs=~w"
        " elapsed_ns=~w cpu_us=~w rss_kb=~w"
        " p50_ns=~w p95_ns=~w p99_ns=~w p999_ns=~w~n",
        [Clients, Msgs,
         ElapsedNs, CpuUs, RssKb,
         P50, P95, P99, P999]
    ).

%% ============================================================
%% Accept loop -- spawns one echo process per connection.
%% ============================================================

accept_loop(LSock) ->
    case gen_tcp:accept(LSock) of
        {ok, Sock} ->
            spawn(fun() -> echo_proc(Sock) end),
            accept_loop(LSock);
        {error, _} ->
            ok
    end.

echo_proc(Sock) ->
    case gen_tcp:recv(Sock, 0) of
        {ok, Data} ->
            gen_tcp:send(Sock, Data),
            echo_proc(Sock);
        {error, _} ->
            gen_tcp:close(Sock)
    end.

%% ============================================================
%% Client process
%% ============================================================

client_proc(ParentPid, Port, Msgs, Tid) ->
    case gen_tcp:connect({127,0,0,1}, Port, [
        binary, {active, false},
        {nodelay, true}
    ], 10000) of
        {ok, Sock} ->
            client_loop(Sock, Msgs, Tid),
            gen_tcp:close(Sock);
        {error, _} ->
            ok
    end,
    ParentPid ! {done, self()}.

client_loop(_Sock, 0, _Tid) ->
    ok;
client_loop(Sock, N, Tid) ->
    Msg = <<"xtcping!">>,
    Len = byte_size(Msg),
    T0  = erlang:monotonic_time(nanosecond),
    ok  = gen_tcp:send(Sock, Msg),
    case recv_exact(Sock, Len, 0, <<>>) of
        {ok, _} ->
            T1 = erlang:monotonic_time(nanosecond),
            ets:insert(Tid, {self(), T1 - T0}),
            client_loop(Sock, N - 1, Tid);
        {error, _} ->
            ok
    end.

recv_exact(_Sock, Need, Got, Acc) when Got >= Need ->
    {ok, Acc};
recv_exact(Sock, Need, Got, Acc) ->
    case gen_tcp:recv(Sock, Need - Got, 10000) of
        {ok, Data} ->
            NewGot = Got + byte_size(Data),
            recv_exact(Sock, Need, NewGot, <<Acc/binary, Data/binary>>);
        {error, _} = Err ->
            Err
    end.

%% ============================================================
%% Wait for N {done, _} messages from client processes.
%% ============================================================

wait_clients(0) ->
    ok;
wait_clients(N) ->
    receive
        {done, _Pid} -> wait_clients(N - 1)
    after 60000 ->
        ok  %% timeout -- treat as done
    end.

%% ============================================================
%% Percentile on a sorted list (1-indexed, clamped).
%% ============================================================

percentile([], _N, _Pct) -> 0;
percentile(Sorted, N, Pct) ->
    Idx0 = round(Pct / 100.0 * N),
    Idx  = max(1, min(Idx0, N)),
    lists:nth(Idx, Sorted).

%% ============================================================
%% Argument parsing
%% Accepts: --clients=N  --msgs=M  --params=clients=N:msgs=M
%% ============================================================

parse_args(Args) ->
    parse_args(Args, 1000, 10).

parse_args([], Clients, Msgs) ->
    {Clients, Msgs};
parse_args([Arg | Rest], Clients, Msgs) ->
    {C1, M1} = case Arg of
        "--clients=" ++ V ->
            {list_to_integer(V), Msgs};
        "--msgs=" ++ V ->
            {Clients, list_to_integer(V)};
        "--params=" ++ Params ->
            parse_params(string:split(Params, ":", all), Clients, Msgs);
        _ ->
            {Clients, Msgs}
    end,
    parse_args(Rest, C1, M1).

parse_params([], C, M) -> {C, M};
parse_params([KV | Rest], C, M) ->
    {C1, M1} = case KV of
        "clients=" ++ V -> {list_to_integer(V), M};
        "msgs="    ++ V -> {C, list_to_integer(V)};
        _               -> {C, M}
    end,
    parse_params(Rest, C1, M1).

%% ============================================================
%% Resource usage (Linux /proc)
%% ============================================================

resource_usage() ->
    {cpu_time_us(), rss_kb()}.

cpu_time_us() ->
    %% erlang:statistics(runtime) gives {TotalWallMs, SinceLastMs};
    %% we want total CPU time in microseconds.
    {TotalMs, _} = erlang:statistics(runtime),
    TotalMs * 1000.

rss_kb() ->
    case file:read_file("/proc/self/status") of
        {ok, Data} ->
            Lines = binary:split(Data, <<"\n">>, [global]),
            find_vmrss(Lines);
        _ ->
            0
    end.

find_vmrss([]) -> 0;
find_vmrss([<<"VmRSS:", V/binary>> | _]) ->
    Trimmed = string:trim(binary_to_list(V)),
    case string:split(Trimmed, " ") of
        [Num | _] -> list_to_integer(Num);
        _         -> 0
    end;
find_vmrss([_ | Rest]) ->
    find_vmrss(Rest).
