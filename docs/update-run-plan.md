# Plan: one update run for applications, and a sealed update state machine

Status: ready for implementation

Two deepening refactors found by an architecture review, and one contract bug
found on the way. They touch disjoint code, so they land in the order below and
each part stands alone:

1. `FirmwareUpdater::report()` tells the truth while an update runs.
2. An **update run** module in `smply::dfu_app` drives an update to completion,
   replacing the loop every application re-implements.
3. The update state machine owns its context, and `FirmwareUpdater` stops
   reading and writing it.

## Problem Statement

**For an application author**, driving an update means writing the same loop
that every caller in the repository has already written: a set of pending flags,
a six-arm event visitor that only sets those flags, a turn of drain, client poll
and updater poll, a reconnect episode around `ReconnectPolicy`, a confirm step,
and a wait on the earlier of two deadlines (the client's and the updater's) with
a fallback when neither has one. It exists in `cli_dfu`, `serial_dfu`,
`winrt_ble_dfu`, the hardware-test rig, the serial hardware test and the
component test's `Application`, six copies that have begun to drift. The
ordering rules that make it correct live only in those copies: close the old
link before reopening a serial port, rebind the client before resuming the
updater, tell the updater when reconnecting is abandoned or it waits forever,
and merge both clocks or a timer is missed. `ReconnectPolicy` was moved to
`support/` to be shared, but it is only the arithmetic; the loop around it,
where the rules are, was not. And because the reconnect delays call the real
clock, the component test cannot run the loop the examples run: it has its own.

**For a caller reading the report**, `FirmwareUpdater::report()` is documented
as "the report as it stands", but it returns an empty report for the whole of a
running update. It is reset when the update starts and filled only when it
finishes.

**For a maintainer of the update decisions**, the seam between the pure state
machine (`dfu::advance()`) and `FirmwareUpdater` leaks both ways. The updater
writes the slot table from a set-state answer straight into the machine's
context, behind the machine's back, and the next image's routing depends on it.
So a unit test that feeds `MarkedForTest` exercises a different contract from
production. The updater also reads the context to carry out effects (the
current target's hash, image number and chosen build, the device's buffer size).
And the unit tests reach states by assigning context fields directly (the slot
table, the current target, `swap_scheduled` and others, about thirty times), so
states the machine can never reach are legal test inputs and moving any field
breaks dozens of tests.

## Solution

**The report.** `report()` returns the report as the machine has written it so
far, and adds the final state, the target hash and the final slot table once
the update is terminal.

**The update run.** A module in `smply::dfu_app` takes an application's
`SmpClient`, `FirmwareUpdater` and a started update, and runs it to the end.
The application supplies three hooks, *open a fresh link*, *approve the new
image* and *observe events*, plus the reconnect settings and an optional
overall deadline. Everything else (the event routing, the reconnect episode,
the confirm, the combined wait) is the module's. Waiting goes through one
seam with two adapters: the real one, a condition variable woken by the
application's `Dispatcher`, and a test one that advances a `ManualClock`. All
three examples and the component test use it.

**The state machine.** The context becomes private to the machine. Events carry
everything the machine routes on, including the slot table a set-state answer
returns. Effects carry everything the updater needs to carry them out. The
machine keeps one report and exposes it. Unit tests reach a state by feeding
events, not by assigning fields.

## User Stories

### Application authors

1. As an application author, I want to run an update to completion with one call, so that I do not have to write a pump loop to use smply.
2. As an application author, I want to supply only how to open a fresh link, whether to confirm and what to show, so that the code I write is the code that differs between my product and others.
3. As an application author, I want the update run to rebind the client and resume the updater in the right order after a reconnect, so that I cannot get that ordering wrong.
4. As an application author, I want the updater told when I give up reconnecting, so that an abandoned reconnect ends the update instead of hanging it.
5. As an application author, I want the run to wait on the earlier of the client's and the updater's deadlines, so that I never need to know there are two clocks.
6. As an application author, I want the run to keep waking when neither the client nor the updater has a deadline, so that an overall timeout stays reachable while the library waits on me.
7. As an application author, I want to set reconnect patience with the existing `ReconnectSettings`, so that the give-up bound stays my product's decision (ADR-0005).
8. As an application author, I want my open-a-link hook to say "not yet, try again" or "give up, here is why", so that a port that exists and refuses stops the retries while an absent one is retried.
9. As an application author, I want the open-a-link hook to own the links it opens, so that a dropped transport outlives the client as the lifetime rules require.
10. As an application author, I want to close the old link inside my hook before opening a new one, so that a serial port held exclusively can be reopened.
11. As an application author, I want my approve hook to confirm, so that the default `Test` mode completes when my self-test passes (ADR-0014).
12. As an application author, I want my approve hook to stop before confirming and leave the device in its trial boot, so that a `test-only` tool can exit with the next reset reverting it.
13. As an application author, I want the run to tell me it stopped before confirming, as distinct from finishing or failing, so that I can report that state honestly.
14. As an application author, I want to observe every `UpdateEvent` for progress output, so that my tool can still print what happens.
15. As an application author, I want an optional overall deadline, so that a tool fails rather than waits forever when something below it stalls.
16. As an application author, I want the run to return the update's own result and report, so that I print the same outcome the updater produced.
17. As an application author, I want the run to work with several links sharing one `Dispatcher`, so that my application keeps owning the dispatcher (handoff caveat).
18. As an application author, I want the run to never call `clear()` or `drain()` on another transport's work, so that sharing a dispatcher stays safe.
19. As an application author, I want to start the update myself with whichever `start()` overload fits, so that single images, build pairs and packages all go through the same run.
20. As an application author, I want a reboot during the update that happens more than once (an interrupted upload, a device image applied by the device) to be handled the same way each time, so that the reconnect episode is reusable.
21. As an application author on Windows, I want a blocking WinRT connect inside my hook to work, so that the run does not assume a connect is fast.
22. As an application author, I want the run to call nothing on my hooks from inside a library callback, so that my hooks may block, sleep and reopen ports.

### Callers reading the report

23. As a caller, I want `report()` during an update to show the per-image outcomes decided so far, so that a progress view can show them.
24. As a caller, I want `report()` after a failure to carry the final state, the target hash and the last slot table, so that I can say what the next reset will do.
25. As a caller, I want `report()` documented exactly as it behaves, so that I can rely on it.

### Tool users

26. As a person running `cli_dfu`, `serial_dfu` or `winrt_ble_dfu`, I want the same output and exit codes as before, so that the refactor changes nothing I can see.
27. As a person running `cli_dfu --flaky-reconnect`, I want refused attempts reported as before, so that the backoff stays visible.
28. As a person running `winrt_ble_dfu --stop-before-confirm`, I want it still to exit with the image installed and unconfirmed.

### Maintainers and contributors

29. As a maintainer, I want the reconnect ordering rules in one module, so that a fix there is fixed for every caller.
30. As a maintainer, I want the examples to show only what differs between media, so that a reader sees what is specific to BLE or serial.
31. As a maintainer, I want the component test to drive the same module the examples use, so that CI tests the loop that ships.
32. As a maintainer, I want the update decisions to live wholly inside the state machine, so that no decision is made in the updater's I/O code.
33. As a maintainer, I want the set-state answer to arrive in the event the machine decides on, so that the unit tests and production share one contract.
34. As a maintainer, I want each effect to carry its parameters, so that the updater carries out an effect without reading the machine's internals.
35. As a maintainer, I want one report, owned by the machine, so that two copies cannot disagree.
36. As a maintainer, I want unit tests to reach states by feeding events, so that a state the machine cannot reach cannot be tested as if it could.
37. As a maintainer, I want a small scenario helper for the unit tests, so that reaching a deep state (after a swap is scheduled, on the second image) is one readable line, not a dozen events.
38. As a maintainer, I want moving or renaming a context field not to break the unit tests, so that the machine's implementation can change behind its interface.
39. As a maintainer, I want `design.md` §8 to describe the machine's interface as it is, so that the living documentation stays true (ADR-0013).
40. As a coding agent, I want one module to read to learn how an update is driven, so that I do not reconcile six copies.
41. As a coding agent, I want the hooks to be the only variable parts, so that adding a new medium's example is filling in three functions.

### Test authors

42. As a test author, I want to run an update under `ManualClock` through the update run, so that reconnect backoff is tested without real sleeps.
43. As a test author, I want the test wait adapter to be where the simulated device reboots and links drop, so that device behaviour happens between turns, never inside a library callback.
44. As a test author, I want to give up reconnecting by returning "give up" from the hook, so that `reconnect_failed` is tested through the run.
45. As a test author, I want to decline the confirm from the approve hook, so that the waiting-for-confirmation outcome is testable.
46. As a test author, I want an overall deadline under `ManualClock`, so that a stalled update fails the test deterministically.

## Implementation Decisions

The decisions below were taken from the recommended answers of the review's
design round (questions 1 to 8). See Further Notes.

### Order

- Three parts, landing in this order: the `report()` fix, the update run, the
  sealed state machine. Each is its own change and each passes the full
  checklist alone. The first two touch disjoint code, so either could land
  first, but the report fix is a contract bug and should not wait.

### Part 1: the report

- `FirmwareUpdater::report()` returns the machine's live report. The fields only
  known at the end (final state, target hash, final slot table) are filled once
  the update is terminal and are default before that. The doc comment says
  exactly this.
- The separate report copy the updater assembles at finish goes away; the
  `UpdateFinished` result is built from the same report.
- Part 3 later moves the report into the machine; this part does not need to.

### Part 2: the update run

- **Where it lives**: `smply::dfu_app`, beside `ReconnectPolicy`,
  `PackageUpdate` and `FileImageSource`. Not installed (ADR-0016), not in the
  core, and not an `smply::asyncutil` adapter (ADR-0019 decision 4 is
  unaffected). It runs on the application's client context, the thread that
  calls `poll()` (ADR-0004).
- **Interface, in prose**: constructed over an `SmpClient`, a
  `FirmwareUpdater`, a wait adapter, the reconnect settings and the three
  hooks; one blocking call runs an update the application has already started
  and returns its outcome. The outcome is one of: finished with the updater's
  `Result<UpdateReport>`, stopped before confirming (by the approve hook), or
  timed out (the overall deadline).
- **The application starts the update.** Choosing between the single-image and
  image-list `start()` overloads, and passing the run's event handler, stays
  with the application. The run supplies the event handler to pass to `start()`;
  it routes the events it acts on and forwards every event to the observe hook.
- **Hooks**:
  - *Open a link*: called once per reconnect attempt, on the client context,
    never from inside a library callback. It owns what it opens, may close the
    old link first, may block, and returns either a transport to rebind to, a
    "retry" verdict, or a "give up" verdict carrying the error the updater is
    told. The serial example's rule (an absent port is retried, a port that
    exists and refuses is not) and the WinRT example's (retry every failure)
    are both expressible.
  - *Approve*: called when the updater raises `ConfirmationRequired`. Returns
    confirm, or stop. Stop ends the run with the update still waiting, which is
    `--stop-before-confirm`'s behaviour.
  - *Observe*: every `UpdateEvent`, for output only.
- **Ordering rules the run owns**: on `ReconnectRequired`, run one
  `ReconnectPolicy` episode; on success, `rebind_transport` then
  `resume_after_reconnect`; on exhaustion or a give-up verdict,
  `reconnect_failed` with the error. Hooks are never called inside `poll()`;
  events are noted and acted on in the next turn, as every copy does today.
- **The wait seam**: a turn is "wait until there is something to do, then
  deliver it, then poll the client and the updater". The seam has three
  operations: the current time, wait until an optional deadline, and sleep for
  a reconnect delay. Two adapters:
  - *Real*: owns the mutex, condition variable and flag every example writes
    today, provides the wake function an application hands to its
    `Dispatcher`, drains that dispatcher after each wake, reads the steady
    clock, and caps a deadline-less wait (50 ms, as today) so the overall
    deadline stays reachable.
  - *Test*: advances a `ManualClock` and steps the component fixture, and is
    where a test models the device between turns (reboot, drop the link).
  Two adapters make this a real seam, not a hypothetical one.
- **The combined deadline**: the run waits on the earlier of
  `SmpClient::next_deadline()` and `FirmwareUpdater::next_deadline()`, and of
  the overall deadline if set. Callers stop seeing two clocks.
- **Callers converted**: `cli_dfu`, `serial_dfu`, `winrt_ble_dfu`, and the
  component test's `Application` (replaced by the run plus the test adapter).
  The hardware-test rig and the serial hardware test keep their own pump.
- **Examples keep**: argument parsing, building the client, groups and updater,
  choosing the `start()` overload, the three hooks, and printing the report.
  Their output and exit codes do not change.
- **Lifetime**: the run captures the client, updater and wait adapter by
  reference; the application declares them, and every link, before the run, as
  the lifetime caveat already requires.
- **Documentation**: `api.md`'s description of driving an update points at the
  run and keeps its prose account of the loop; `architecture.md` §10 lists the
  run in `smply::dfu_app`; the `support/` line in `CLAUDE.md` names it;
  `testing.md` names the test wait adapter among the doubles.

### Part 3: the sealed state machine

- **A `dfu::Machine` class** owns the context and the current `UpdateState`.
  Constructed from the targets and the plan; one operation applies an event and
  returns a `Step`; read-only queries expose the state and the report. The free
  function and the public `Context` struct go away. It stays internal to
  `src/dfu/` and is not public interface.
- **Events carry what the machine routes on.** `MarkedForTest` and `Confirmed`
  carry the slot table the set-state command returned, borrowed for the call,
  as `StateRead` already does. The machine records it. The updater's direct
  write of the slot table is removed.
- **Effects carry their parameters.** `StartUpload` names the target and build
  to send, the image number and the device's buffer size. `MarkForTest` and
  `Confirm` name the hash and image. The updater never reads the machine's
  context to carry out an effect. Its timers (`AwaitDisconnect`, `AwaitApply`)
  keep reading only the plan and its own clock state.
- **One report**: owned by the machine, written as it decides, exposed live.
  The updater adds nothing to it except through events; the final state, target
  hash and final slot table are filled by the machine on entering a terminal
  state. Part 1's behaviour of `report()` is preserved exactly.
- **The rules stay where they are**: the rollback-from-flags rule, the single
  mark-for-test recovery, the fatal-refused-confirm rule and ADR-0021's
  client-after-device rule remain the machine's, and their row-by-row tests in
  `design.md` §8's failure table remain.
- **Documentation**: `design.md` §8 describes the machine's interface (events
  in, steps out, report exposed) in place of the context struct, in the same
  change (ADR-0013). No ADR changes: ADR-0008's split between decisions and
  I/O is what this restores.

## Testing Decisions

- **A good test crosses the module's interface and nothing else.** For the
  update run that is: start an update, run it through the run with the test
  wait adapter, and assert on the outcome, the events observed and the
  simulator's requests. For the machine: feed events, assert on the steps and
  the report. No test reads or assigns internal fields.
- **Part 1** gets a component test that reads `report()` partway through a
  multi-image update and sees the outcomes decided so far, and one that reads
  it after a failure and sees the final state and slot table. Prior art: the
  component tests in the firmware-update suite that read `report()` after the
  update.
- **Part 2** is tested through the update run in the component suite, against
  `ServerSimulator` and `FakeTransport` under `ManualClock`:
  - every existing component test that uses `Application` now uses the run,
    with the same assertions;
  - new cases: reconnect after refused attempts with exact backoff delays read
    from the `ManualClock`; give-up through the policy's exhaustion; give-up
    through the hook's verdict, with that error reaching the updater; approve
    returning stop; the overall deadline; a second reboot in the same update.
  The `cli_dfu` and `serial_dfu` ctests (the pty stub, `--flaky-reconnect`,
  the package runs) keep passing unchanged; they are the end-to-end check that
  the examples' output did not move. `winrt_ble_dfu` is compile-checked on
  Windows CI only.
- **Part 3**: the unit suite for the update state machine is rewritten to drive
  `dfu::Machine` through events. A small scenario helper builds common
  prefixes (parameters and bootloader read, state read, upload finished, marked
  for test, reset accepted, reconnected) so a deep state is one call. Every
  case that assigned a context field is re-expressed as the event sequence that
  reaches that state; a case that cannot be is deleted with a note in the
  commit message, because it tested an unreachable state. A new case covers
  the leak this part fixes: `MarkedForTest` carrying a slot table that changes
  the next image's routing. The component suite must pass unchanged, which
  proves the updater's observable behaviour did not move.
- **Coverage**: parts 1 and 3 change `src/`, so the coverage and sanitizer
  presets pass with `--enforce`. Every preset builds, Clang's ASan included,
  because the run captures references across callbacks.

## Out of Scope

- **One `poll` and one `next_deadline` for the whole core**, by running
  `FirmwareUpdater`'s timers on `SmpClient::poll`. It changes the public
  interface and touches ADR-0003 and ADR-0004. The update run hides the two
  clocks from callers anyway. Recorded as a roadmap backlog row.
- **The hardware-test rig and the serial hardware test** keep their pump; they
  inject faults mid-update at moments the hooks do not expose. They can adopt
  the run later if they fit.
- **One owner for an update's inputs** (a single file, a build pair or a
  package), the review's third candidate. The run takes an already-started
  update, so it neither needs nor blocks that work.
- **The upload resume rule**, the review's fourth candidate.
- Any change to update behaviour, events, the public `FirmwareUpdater`
  interface (beyond `report()`'s behaviour matching its doc), or the examples'
  command-line options.

## Further Notes

- **The design round was not answered.** The decisions above are the
  recommended answers to the review's eight questions: convert all three
  examples (1); a blocking run over a wait seam (2); the examples plus the
  component test (3); the core single-`poll` change out of scope (4); fix
  `report()` first (5); a `dfu::Machine` class (6); events carry the slot table
  (7); this order (8). Overturning one changes this plan before implementation
  starts.
- **The seams to test at** are the update run's interface (new, with the wait
  seam inside it as the one new seam with two adapters) and `dfu::Machine`'s
  interface (the existing machine seam, narrowed). `FirmwareUpdater`'s public
  interface stays the seam the component suite tests at.
- The trade-off accepted in part 2: the examples are no longer the one file
  that shows the whole loop. `api.md` keeps the prose account, and the update
  run becomes the code to read.
- Part 3 is the largest diff, mostly in the unit tests. If it grows past about
  1,000 lines, split it: first the event carrying the slot table and effects
  carrying parameters, then the class and the test rewrite.

## Stages

### Stage 1: `report()` tells the truth while an update runs

Blocked by: none

Delivers part 1. `FirmwareUpdater::report()` returns the live report, with the
terminal-only fields filled once the update is terminal; one report, not a copy
assembled at finish. Doc comment and `api.md` match.

- [x] A component test reads `report()` partway through a multi-image update and sees the outcomes decided so far (fails before the fix).
- [x] A component test reads `report()` after a failure and sees the final state, target hash and final slot table.
- [x] The `UpdateFinished` result and `report()` come from the same report.
- [x] `report()`'s doc comment and `api.md` describe exactly this behaviour.

### Stage 2: the update run, proven by the component suite

Blocked by: none

Delivers part 2's module: the update run in `smply::dfu_app`, its three hooks,
the wait seam with its real adapter and its `ManualClock` test adapter, and the
component suite's `Application` replaced by it. The examples are not touched.

- [x] The update run and the real wait adapter live in `smply::dfu_app`; the test wait adapter lives with the component test support.
- [x] Every component test that used `Application` uses the run, with its assertions unchanged.
- [x] New component cases: refused attempts with exact backoff delays read from `ManualClock`; give-up by policy exhaustion; give-up by the hook's verdict with that error reaching the updater; approve returning stop; the overall deadline; a second reboot in one update.
- [x] No hook is called from inside `poll()`; no test reads the real clock.
- [x] `architecture.md` §10, `testing.md` and the `support/` line in `CLAUDE.md` name the run and the test adapter.

### Stage 3: the three examples use the update run

Blocked by: 2

Delivers the rest of part 2. `cli_dfu`, `serial_dfu` and `winrt_ble_dfu` keep
their setup, `start()` choice, hooks and report printing, and drop their
pending flags, visitor routing, reconnect loop, confirm step and wait.

- [x] Each example's output and exit codes are unchanged; the `cli_dfu` and `serial_dfu` ctests pass unmodified.
- [x] `serial_dfu`'s rule survives: an absent port is retried, a port that exists and refuses ends the episode.
- [x] `winrt_ble_dfu --stop-before-confirm` exits with the image installed and unconfirmed, via the approve hook.
- [x] `winrt_ble_dfu` still compiles: reviewed against the "Windows half" caveats in `handoff.md`, since only Windows CI builds it.
- [x] `api.md`'s account of driving an update points at the run.

### Stage 4: events carry the slot table, effects carry their parameters

Blocked by: 1

Delivers the first half of part 3. `MarkedForTest` and `Confirmed` carry the
set-state answer, and the machine records it; `StartUpload`, `MarkForTest` and
`Confirm` carry what the updater needs. `FirmwareUpdater` no longer writes the
context, and reads it for nothing but the report.

- [x] A unit test shows `MarkedForTest` carrying a slot table that changes the next image's routing (fails before the change).
- [x] The updater's direct write of the slot table is gone; carrying out an effect reads only the step.
- [x] The component suite passes unchanged.
- [x] `design.md` §8 describes the events and effects as they now are.

### Stage 5: `dfu::Machine` owns its context, state and report

Blocked by: 4

Delivers the rest of part 3. The free function and public `Context` give way to
`dfu::Machine`; the unit suite drives it through events with a scenario helper.

- [x] `dfu::Machine` owns the context, the current state and the one report; the updater holds a machine, not a context.
- [x] No unit test assigns a context field; deep states are reached through a scenario helper. A case that tested an unreachable state is deleted and named in the commit message.
- [x] The component suite passes unchanged, and `report()` behaves exactly as stage 1 left it.
- [x] `design.md` §8 describes the machine's interface in place of the context struct.
