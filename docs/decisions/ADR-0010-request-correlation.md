# ADR-0010 — Correlation, SMP version default, and one request in flight

**Status:** Accepted (2026-09-04)

## Context

Three related protocol-policy questions that the specification leaves open
([`../protocol-notes.md`](../protocol-notes.md) §4, §9 A1/A4/A10):

1. `seq` is 8 bits and wraps. What happens when a response arrives late, or with
   a matching `seq` but the wrong group?
2. SMP v2 gives group-scoped error codes, but there is no version negotiation
   and a v1-only server rejects v2 requests.
3. Zephyr's SMP server processes packets sequentially and has a small, bounded
   number of buffers. How many requests may be outstanding?

## Decision

**Correlation.** A response matches a pending request only if
`seq`, `group`, `command` and `op` all agree (`op` must be the response form of
the request's). On mismatch the *message* is discarded and a counter is bumped;
the request stays pending and times out normally. A response whose `seq` matches
nothing pending is checked against a bounded **retired-sequence set** (the last
64 completed/cancelled/timed-out sequence numbers) and dropped silently if
found; otherwise it is counted as unmatched. The sequence allocator skips both
pending and retired values.

*Why discard rather than fail the request:* the specification does not define
this case, and a mismatched message may belong to a stale exchange. Discarding
is the only choice that can never mis-complete a request with someone else's
data. The cost is one timeout in a genuinely broken situation.

**SMP version.** Requests default to **v1** (`version = 0b00`). v2 is opt-in via
`SmpClientConfig::smp_version`. **Responses are always decoded for both error
shapes**, regardless of what was requested — the specification explicitly
requires v2 clients to handle flat `rc` too, and v1 servers only ever produce
it.

*Why v1 by default:* there is no negotiation mechanism, v1 is universally
supported, and a v2 request to an older server fails outright with
`UNSUPPORTED_TOO_NEW`. The benefit of v2 — group-scoped error codes — improves
diagnostics but changes no behaviour, so it is not worth an interoperability
risk as a default. Revisit when the deployed fleet is known to be v2-capable.

**Concurrency.** `max_in_flight = 1` by default. The upload path is therefore
strictly request-response, which is also what makes retransmission safe
(there is never an ambiguous second request in the air for the same offset).
`buf_count` from the OS parameters command is recorded but not yet used to raise
the limit; the pending-request table is written for N and tested at N.

## Alternatives considered

**Correlate on `seq` alone.** What most clients do. It is wrong in exactly the
case that matters: a late response to a timed-out request whose `seq` has been
reused would be attributed to a live request. The retired set plus the tuple
check closes this. Rejected.

**Fail the pending request on a group/command mismatch.** Faster failure, but it
lets a stale or hostile message terminate a legitimate in-flight request — a
denial-of-service primitive. Rejected.

**Default to SMP v2.** Better error detail immediately. Rejected on
interoperability: no negotiation exists, and the failure mode against an older
device is a hard error rather than a graceful degradation.

**Negotiate the version by probing** (send v2, retry as v1 on
`UNSUPPORTED_TOO_NEW`). Doable and possibly a future improvement, but it adds a
retry path to every first request for a diagnostics-only benefit. Deferred, and
recorded as an open question in [`../roadmap.md`](../roadmap.md).

**Pipelining several requests.** Would speed uploads over BLE, where round-trip
latency dominates. Rejected for now: it complicates retransmission semantics
(which offset is authoritative when two chunk requests are outstanding?), risks
exhausting the server's buffers, and the protocol gives no ordering guarantee
across concurrent commands. Listed as a future extension, gated on `buf_count`
and on HIL measurements.

## Consequences

* Late and replayed responses cannot corrupt a live request — mitigation T5 in
  [`../security.md`](../security.md).
* Upload throughput is bounded by round-trip latency. Acceptable for the initial
  scope; measured in the HIL suite so the pipelining decision can be made on
  data.
* Both v1 and v2 error decoding are always exercised; the `ServerSimulator` runs
  every component test in both modes.
* `max_in_flight` is a config field, not a constant, so raising it later is not
  an architectural change.
