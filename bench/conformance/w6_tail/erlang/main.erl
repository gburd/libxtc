#!/usr/bin/env escript
%%! -smp enable
%%-
%% Copyright (c) 2026, The XTC Project
%% Use of this source code is governed by the ISC License.
%%
%% bench/conformance/w6_tail/erlang/main.erl
%%   W6: tail latency under backpressure -- Erlang runtime.
%%
%%   A bounded-queue server process accepts synchronous {call, ...} messages
%%   from N generator processes.  Generators use a manual synchronous-call
%%   idiom (send + receive-with-timeout) that mirrors gen_server:call/3 but
%%   works inside a self-contained escript without needing an -export'd
%%   gen_server callback module.
%%
%%   Backpressure mechanism:
%%     1. Pre-check: generators read the server's message_queue_len before
%%        sending; if it is already >= cap they reject immediately.
%%     2. Timeout: if the pre-check passes but the server is slow to reply,
%%        the caller times out after Timeout ms (default 5 ms) -> rejection.
%%
%%   Latency is measured server-side: TsRecv (when the server dequeues the
%%   message) minus TsSend (embedded in the message by the caller).  This
%%   captures the queuing latency experienced by admitted messages.
%%
%%   RUNTIME SEMANTICS NOTE:
%%   With N=8 synchronous generators, each blocks waiting for a reply before
%%   sending the next message.  At most N=8 requests can be simultaneously
%%   in-flight, so the cap=1000 mailbox check rarely fires.  Rejections come
%%   primarily from the call timeout under BEAM scheduling jitter.  This is
%%   the natural Erlang synchronous-server idiom; compare to xtc where the
%%   cap fires explicitly on every request over the threshold.
%%
%% Usage:
%%   escript main.erl [--gens=<int>] [--ops=<int>] [--cap=<int>]
%%   escript main.erl --params=gens=8:ops=1000000:cap=1000
%%   Defaults: gens=8, ops=1000000, cap=1000

main(Args) ->
    Gens    = parse_int_arg("gens", Args, 8),
    Ops     = parse_int_arg("ops",  Args, 1000000),
    Cap     = parse_int_arg("cap",  Args, 1000),
    Timeout = 5,   %% ms; "tiny timeout" as per spec

    T0 = erlang:monotonic_time(nanosecond),
    erlang:statistics(runtime),

    Parent = self(),

    %% Start bounded-queue server.
    Server = spawn_link(fun() -> server_loop(Cap, Parent) end),
    receive server_ready -> ok end,

    %% Spawn N generator processes.
    OpsPerGen = Ops div Gens,
    Remainder = Ops rem Gens,
    Pids = [begin
                MyOps = OpsPerGen + (if I < Remainder -> 1; true -> 0 end),
                spawn_link(fun() ->
                    gen_loop(Server, Cap, MyOps, Timeout, Parent)
                end)
            end || I <- lists:seq(0, Gens - 1)],

    %% Collect {gen_done, Pid, Admitted, Rejected} from each generator.
    {TotalAdmitted, TotalRejected} = collect_gen_results(Pids, 0, 0),

    %% Collect {lat, Ns} sent by the server for each admitted message.
    Latencies = collect_latencies(TotalAdmitted, []),

    %% Signal server to stop.
    Server ! stop,

    T1 = erlang:monotonic_time(nanosecond),
    {CpuMs, _} = erlang:statistics(runtime),

    ElapsedNs = T1 - T0,
    CpuUs     = CpuMs * 1000,
    RssKb     = rss_kb(),

    Sorted = lists:sort(Latencies),
    Len    = length(Sorted),
    P50    = nth_pct(Sorted, Len, 50.0),
    P95    = nth_pct(Sorted, Len, 95.0),
    P99    = nth_pct(Sorted, Len, 99.0),
    P999   = nth_pct(Sorted, Len, 99.9),

    io:format(
        "workload=W6 runtime=erlang params=gens=~w:ops=~w:cap=~w"
        " elapsed_ns=~w cpu_us=~w rss_kb=~w"
        " p50_ns=~w p95_ns=~w p99_ns=~w p999_ns=~w rejected=~w~n",
        [Gens, Ops, Cap,
         ElapsedNs, CpuUs, RssKb,
         P50, P95, P99, P999,
         TotalRejected]).

%% =========================================================================
%% Bounded-queue server process
%%
%%   Receives synchronous {call, From, Ref, {msg, TsSend}} messages.
%%   Records server-side latency (now - TsSend) and forwards {lat, Lat}
%%   to the parent collector before replying to the caller.
%%
%%   The "capacity" is enforced by callers via process_info pre-check;
%%   the server itself is unbounded.
%% =========================================================================

server_loop(Cap, Parent) ->
    Parent ! server_ready,
    server_recv(Cap, Parent).

server_recv(Cap, Parent) ->
    receive
        {call, From, Ref, {msg, TsSend}} ->
            TsRecv = erlang:monotonic_time(nanosecond),
            Lat    = TsRecv - TsSend,
            Parent ! {lat, Lat},
            From ! {reply, Ref, ok},
            server_recv(Cap, Parent);
        stop ->
            ok
    end.

%% =========================================================================
%% Generator loop
%%
%%   Each generator runs this loop for `Ops` iterations.
%%   Step 1: pre-check server mailbox length; reject if >= Cap.
%%   Step 2: synchronous call with Timeout-ms deadline.
%%   Sends {gen_done, self(), Admitted, Rejected} to Parent when done.
%% =========================================================================

gen_loop(Server, Cap, Ops, Timeout, Parent) ->
    gen_loop(Server, Cap, Ops, Timeout, Parent, 0, 0).

gen_loop(_Server, _Cap, 0, _Timeout, Parent, Admitted, Rejected) ->
    Parent ! {gen_done, self(), Admitted, Rejected};

gen_loop(Server, Cap, Remaining, Timeout, Parent, Admitted, Rejected) ->
    %% Pre-check: reject immediately if server mailbox is at or beyond cap.
    {message_queue_len, Qlen} = erlang:process_info(Server, message_queue_len),
    if Qlen >= Cap ->
        gen_loop(Server, Cap, Remaining - 1, Timeout,
                 Parent, Admitted, Rejected + 1);
    true ->
        TsSend = erlang:monotonic_time(nanosecond),
        Ref    = make_ref(),
        Server ! {call, self(), Ref, {msg, TsSend}},
        receive
            {reply, Ref, ok} ->
                gen_loop(Server, Cap, Remaining - 1, Timeout,
                         Parent, Admitted + 1, Rejected)
        after Timeout ->
            %% Timed out: call took longer than Timeout ms -> rejection.
            gen_loop(Server, Cap, Remaining - 1, Timeout,
                     Parent, Admitted, Rejected + 1)
        end
    end.

%% =========================================================================
%% Result collectors
%% =========================================================================

collect_gen_results([], Admitted, Rejected) ->
    {Admitted, Rejected};
collect_gen_results(Pids, Admitted, Rejected) ->
    receive
        {gen_done, Pid, A, R} ->
            Remaining = lists:delete(Pid, Pids),
            collect_gen_results(Remaining, Admitted + A, Rejected + R)
    end.

collect_latencies(0, Acc) ->
    Acc;
collect_latencies(N, Acc) ->
    receive
        {lat, Lat} ->
            collect_latencies(N - 1, [Lat | Acc])
    after 30000 ->
        %% Safety valve: if no {lat, _} arrives within 30 s, stop
        %% collecting to avoid a deadlock (can happen if a caller timed
        %% out AFTER the server had started handling the call -- the
        %% server still sends {lat, _} and {reply, Ref, ok} to the
        %% caller, but the caller has moved on; the lat message arrives
        %% here later).  The after branch returns the partial list.
        Acc
    end.

%% =========================================================================
%% Percentile from a sorted list
%% =========================================================================

nth_pct(_Sorted, 0, _Pct) ->
    0;
nth_pct(Sorted, Len, Pct) ->
    Idx0 = round(Pct / 100.0 * Len),
    Idx  = max(1, min(Idx0, Len)),
    lists:nth(Idx, Sorted).

%% =========================================================================
%% Peak RSS (KiB) from /proc/self/status; 0 if unavailable.
%% =========================================================================

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

%% =========================================================================
%% Argument parsing
%%   Handles --key=value and --params=key=val:key=val
%% =========================================================================

parse_int_arg(Key, Args, Default) ->
    LongKey  = "--" ++ Key ++ "=",
    ParamsKV = Key ++ "=",
    lists:foldl(
        fun(Arg, Acc) ->
            case string:prefix(Arg, LongKey) of
                nomatch ->
                    case string:prefix(Arg, "--params=") of
                        nomatch ->
                            Acc;
                        Params ->
                            parse_kv(ParamsKV,
                                     string:tokens(Params, ":"),
                                     Acc)
                    end;
                ValStr ->
                    list_to_integer(ValStr)
            end
        end,
        Default,
        Args).

parse_kv(_Prefix, [], Acc) ->
    Acc;
parse_kv(Prefix, [KV | Rest], Acc) ->
    case string:prefix(KV, Prefix) of
        nomatch -> parse_kv(Prefix, Rest, Acc);
        ValStr  -> list_to_integer(ValStr)
    end.
