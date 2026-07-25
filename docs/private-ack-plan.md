# Plan: private OUCH ack over the existing TCP connection

Implementation plan for sending `OuchExecutionReport`s back to the originating
client on its existing order-entry socket. Today the exchange never writes back:
the gateway only `read()`s, execution info flows down the drop-copy path to the
audit log + `/dev/shm`, and fills are visible to firms only anonymously via ITCH
multicast. This adds the missing **private ack** leg of the three-channel model
(public market data / private acks / drop copy).

## Objective

When an order becomes NEW / PARTIAL / FILLED / CANCELED / REJECTED, send an
`OuchExecutionReport` back to the **originating client** on the same socket it
sent the order on — so a firm can attribute fills to its own `order_token`
(closing the loop the ITCH feed can't, since ITCH carries `internal_id`, not the
token).

## The one hard constraint that dictates the design

A client fd is **owned by one gateway worker** — it lives in that worker's
thread-local `client_states`, is registered in that worker's epoll, and only that
thread may `read()`/`write()` it. But fills are produced on **engine threads**,
and cancels/news also originate in the engine. So every execution report must be
**routed back to the owning worker** to be written. That routing is the whole
problem.

## Recommended topology (zero ripple to the audit / Python side)

Mirror the *ingress* pattern exactly. Ingress is `engine_queues[shard][worker]`
(worker → engine, SPSC). Add the symmetric **egress**:

```
ack_queues[shard][worker]   // OuchExecutionReport, SPSC
   producer = engine shard s   (sole writer of ack_queues[s][*])
   consumer = gateway worker w  (sole reader of ack_queues[*][w])
```

Engine shard `s` pushes an ack; worker `w` drains `ack_queues[0..N][w]` after
`epoll_wait` and writes to the client fd. One producer + one consumer per queue →
the `LockFreeQueue` SPSC contract holds, identical to ingress.

**Why this over routing through the OrderManager:** the OM path would need
`worker_id` + `client_id` inside `DropCopyMessage`, but it is already exactly 32
bytes (`order.h`) — growing it changes the audit-log record size and forces
matching updates in `scripts/decode_audit.py` + `monitoring/feeds/audit_reader.py`.
The shard×worker ack queue leaves `DropCopyMessage`, the audit format, and the
entire Python layer **untouched**. Tradeoff: the engine does one extra `push` per
event — but it already pushes to `mkt_data_queue` *and* `drop_copy_queue` per
event, so it is the same class of work, not a new one.

## Step-by-step

**1. Give the Order a `worker_id` (`matching/order.h`).**
`Order` (alignas(64)) has spare room. Add `uint16_t worker_id;`, thread it through
the constructor + `MemoryPool::allocate`, and in `gateway/tcp_server.h` set it
from the worker's own id at allocation (the worker already has `worker_id` in
`handle_client`). This is the only struct change, and it touches **no wire/audit
format**.

**2. Add the ack queues (`app/exchange.cpp`).**
```cpp
// [shard][worker]
std::array<std::vector<std::unique_ptr<LockFreeQueue<OuchExecutionReport, 524288>>>, NUM_SHARDS> ack_queues;
```
Allocate `num_gw` per shard (same loop as `engine_queues`), pass `&` into both
`Engine` (producer) and `TCPServer` (consumer).

**3. Engine produces acks (`matching/orderbook.h` `send_drop_copy` + call sites).**
At each existing `send_drop_copy(...)` point (NEW at 281/337, FILL at
247/249/301/303, CANCEL at 196), *also* build an `OuchExecutionReport` from the
`Order` and push to `ack_queues[shard][order->worker_id]`. The Order already
carries `client_order_id` (the token to echo) and now `worker_id`. Cleanest: give
`send_drop_copy` the `Order*` (or add `worker_id` + `token` params) so it emits
both the drop copy (unchanged) and the ack.

For every execution, generate an execution report for **each affected order**
(both the resting order and the incoming/aggressor order) and enqueue each report
to the corresponding gateway worker using that order's own `worker_id` — the two
sides may belong to different clients on different workers, so each gets its own
report routed to `ack_queues[shard][order->worker_id]`.

This falls out naturally from the existing code: the engine already emits a drop
copy for **both** sides of every match — `orderbook.h` calls `send_drop_copy`
twice per fill (aggressor at lines 247/301, resting order at 249/303). Adding the
ack push at those same two sites means the resting order's firm gets its fill
report just like the aggressor's. Note the resting order keeps the `worker_id` it
was stamped with when *it* was created (by the worker owning that firm's
connection), so its ack routes back to the right client even though a different
firm triggered the match. Both orders are the same instrument → same shard `s`,
and shard `s` is the sole producer into `ack_queues[s][*]`, so pushing to two
different workers' queues still respects the single-producer-per-queue SPSC
contract.

**4. Gateway worker drains + writes (`gateway/tcp_server.h` `worker_loop`).**
- Keep a `std::unordered_map<uint32_t /*client_id*/, int /*fd*/>` — populate on
  `accept`, erase on `close`.
- After `epoll_wait`, drain `ack_queues[0..NUM_SHARDS-1][thread_id]`; for each
  report, look up the fd by the report's client_id and write it. Carry `client_id`
  in the queue via a tiny `{OuchExecutionReport, uint32_t client_id}` wrapper — the
  wire struct stays 32 bytes; the extra id is queue-internal, never sent.

**5. Rejects: write inline (no queue).**
Rejects are produced *in the worker* (`handle_client`, the `gw_reject_queues`
push). The worker already holds the fd and the token, so build + write the reject
`OuchExecutionReport` right there. No routing needed for the reject case.

## The hard part: non-blocking writes + backpressure

Client fds are `O_NONBLOCK` + `EPOLLET`, so a naive `write()` can return `EAGAIN`
or a partial count. Do it properly:

- Add `char out_buf[]; size_t out_len;` to `ClientState`.
- To send: append the 32-byte report to `out_buf`, then try to flush with
  `send(fd, …, MSG_NOSIGNAL)`. On full send → done. On partial/`EAGAIN` → keep the
  remainder and add **`EPOLLOUT`** to that fd's epoll registration.
- On an `EPOLLOUT` event → flush `out_buf`; when empty, drop back to
  `EPOLLIN|EPOLLET`.
- Cap `out_buf`; if a slow client overflows it, close the connection (can't keep
  up).
- `MSG_NOSIGNAL` (or a global `signal(SIGPIPE, SIG_IGN)`) so a client that closed
  mid-stream doesn't `SIGPIPE` the exchange.

This backpressure handling — not the routing — is where the real time goes.

## Message population (`OrderState` → `OuchExecutionReport`)

`OuchExecutionReport{ msg_type, order_token[14], executed_shares, execution_price, liquidity_flag, match_number[8] }` (32 bytes).

- **FILL/PARTIAL** → `executed_shares = match_qty`, `execution_price`,
  `liquidity_flag = 'R'` (removed) for the aggressor / `'A'` (added→hit) for the
  resting side. **Highest-value slice** — the firm's `tcp_rx_thread` already
  consumes exactly this and updates position/PnL, so start here.
- **NEW** (accepted) → `executed_shares = 0`. The struct only comments
  `msg_type='E'`; a full ack channel wants distinct types ('A' accepted, 'C'
  canceled, 'J' rejected). Either overload `msg_type` or add those message types in
  `protocol/messages.h` for NEW/CANCEL/REJECT in a second pass.

## Firm side

Likely **no change** — `LocalExchangeConnector`'s `tcp_rx_thread` already reads
32-byte `OuchExecutionReport`s off the socket. Verify its framing handles
back-to-back reports and that the struct layout matches (both sides include
`messages.h`).

## Testing

- **Unit:** reuse the socketpair harness (`tests/gateway_fixture.h`,
  `tests/test_framing.cpp`) — drive `handle_client` with an order, then assert an
  `OuchExecutionReport` echoing the token is written back on the socket. Same
  pattern already used for framing.
- **Integration:** run `market_maker` (it prints PnL from received reports) or the
  firm against the exchange and confirm fills arrive and positions update. The
  `scripts/multi_firm_demo.sh` harness is a convenient driver.

## Scope & risk

~2–3 hrs: `worker_id` plumbing (30m) · ack queues + engine push (45m) · worker
drain + client_id→fd map (30m) · **non-blocking write buffer + EPOLLOUT (60–90m,
the risk)** · reject inline (15m) · test (30m). Main risk is the
EPOLLOUT/backpressure edge cases; everything else is mechanical and mirrors
existing patterns.

## Sequencing suggestion

1. Ship the **FILL/PARTIAL** execution report end-to-end first (Order.worker_id →
   ack queues → engine push on fills → worker drain → non-blocking write). That
   alone closes the loop the firm cares about.
2. Add NEW/CANCEL/REJECT message types in a second pass.
3. Reject inline last (it is independent and trivial).

## Relationship to firm monitoring

This ack channel is a **prerequisite** for `docs/firm-monitoring-plan.md`. The
firm today tracks position on *intent* (orders it sends), not confirmed fills,
because nothing acks it — so its PnL/position view is optimistic, not true. The
firm-monitoring plan makes the firm's `tcp_rx_thread` apply these
`OuchExecutionReport`s to an ack-driven position/PnL, then publishes that to a
`/dev/shm/firm_stats` seqlock region a Python panel reads. Implement this ack
channel **first**; firm monitoring builds directly on it.
