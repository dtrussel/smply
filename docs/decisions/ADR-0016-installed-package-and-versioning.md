# ADR-0016 — What the installed package contains, and what a version promises

**Status:** Accepted (2026-09-18)

## Context

P15a built install/export and shipped two targets, `smply::smply` and
`smply::util`, because those were the two the phase needed. Everything else was
left for P18 with a follow-up row each, and the rows had accumulated: whether
`smply::transport_common`, `smply::dfu_app`, `smply::minicbor` and
`smply::winrt_ble` belong in the package. The tree has eight installable
candidates and no written rule for choosing, so "is this installed?" was
answered per target, in a CMake comment, by whoever last touched it.

Two things make the answer matter now rather than later.

The first is that an **out-of-tree adapter is the reason the transport contract
exists**. `Transport` is public, `design.md` §9 is normative, and ADR-0005
froze the interface in P4 precisely so that somebody outside this repository
could implement it. Until P18 they could not: the fragmentation arithmetic and
the send-admission queue that a correct BLE adapter needs were in a target the
package did not contain.

The second is that **an installed target is a promise**. Once a consumer links
`smply::X` from a package, removing or reshaping it is a breaking change. That
cost is worth paying for what the library is for, and is not worth paying for
what happens to be convenient.

## Decision

**1. The installed package is `smply::smply`, `smply::util` and
`smply::transport_common`, and nothing else.**

| Target | | Why |
| ------ | - | --- |
| `smply::smply` | **in** | The product. |
| `smply::util` | **in** | ADR-0004 makes marshalling the *adapter's* obligation and deliberately does not link `Dispatcher` into the core. An adapter that cannot get `Dispatcher` from the package cannot meet an obligation the documentation states normatively. |
| `smply::transport_common` | **in** | `fragment_size()` is arithmetic with a unit suite behind it, and `SendQueue` **is** the fix for A22 — a defect that killed an upload six cases into a bench run and that the entire simulated suite could not see. Leaving it out does not keep a third-party adapter simple; it makes that adapter re-derive both, including the bug. Header-only, so there is no ABI to keep. |
| `smply::dfu_app` | **out** | Application policy, not protocol: a file reader and a reconnect backoff. O4 already decided the library does not ship file I/O, and P17b re-documented `ReconnectPolicy`'s defaults as a give-up ceiling justified by measurements on one platform — numbers that mean nothing off that bench and that a version promise should not freeze. Both examples show how to write them. |
| `smply::minicbor` | **out** | Test and example scaffolding, and deliberately a *second* CBOR implementation so a symmetric bug cannot hide inside a round trip. Shipping it would offer a consumer a codec smply itself does not use. |
| `smply::winrt_ble` | **out** | A *reference* adapter: source to read and copy, not a package member. It is the one target neither clang-tidy nor cppcheck sees (`tools/lint.sh`), so a compatibility promise about its surface would rest on MSVC `/W4` alone; and a consumption check for it could only run on the Windows CI job. What a Windows consumer actually needs from the package — the core, `Dispatcher`, the framing and the queue — is installed, and this adapter is the worked example of using them. |
| `smply::test_support`, `smply::smply_internal`, `smply_internal_options` | **out** | Development scaffolding. `smply_internal_options` must additionally stay `$<BUILD_INTERFACE:>`, or the export is impossible at all (P15a). |

**2. `smply::winrt_ble` being out settles `send_counters()`: it stays.**
`WinRtBleTransport::send_counters()` is not part of `Transport` and is not
promised to anyone. It exists because ADR-0015 requires a green bench run to be
distinguishable from a run in which the race merely did not fire, and it is the
only thing that makes `deferred_sends > 0` observable. Since the adapter is not
a package member, keeping it costs no compatibility promise at all — and
dropping it would cost the bench its only evidence that the A22 fix does
anything. It is documented as a per-adapter diagnostic, and an adapter is free
to have them.

**3. An installed header keeps its in-tree spelling.** `transports/` is the
build-tree include root, so an adapter writes `#include "common/ble_framing.hpp"`.
The installed root is `<prefix>/include/smply/transports`, with `common/` kept
underneath, so that same line compiles out of an install tree. The root is
nested under `smply/` rather than placed at `<prefix>/include`, so that linking
the target cannot put a bare `common/` on a consumer's search path.

**4. The stable surface is `include/smply/` plus the installed transport
headers.** Everything under `support/`, `tests/`, `examples/` and
`transports/winrt_ble/` is outside it and may change in any release.

**5. Versioning is SemVer, and the promise starts at 1.0.** While the version
is `0.x`, a minor bump may break the surface above, which is what `0.x` means.
`1.0.0` is a deliberate act, not a consequence of the packaging being finished:
P18 built and proved the package and left the version at `0.1.0` so that the
compatibility promise is made by a person. `CHANGELOG.md` records what changes;
the config package already declares `COMPATIBILITY SameMajorVersion`, which is
the machine-readable half of the same rule.

## Alternatives considered

**Install everything.** It answers every follow-up row at once and it is what a
tree of eight targets drifts towards. Rejected: it would promise `minicbor` —
a codec that exists to *disagree* with `src/cbor/` — and freeze
`ReconnectPolicy`'s bench-derived constants into a compatibility contract.

**Install nothing but `smply::smply`,** and let an adapter vendor the rest.
Coherent, and it is what the package was before P18. Rejected because it makes
the transport contract's public status a fiction: ADR-0005 invites an
out-of-tree adapter and this would send that author to copy `send_queue.hpp` by
hand, which is exactly how A22 gets reimplemented.

**Ship `winrt_ble` conditionally on `SMPLY_BUILD_WINRT`,** with package
components so a consumer can ask for it. Technically straightforward and
genuinely useful to a Windows consumer. Rejected for now on evidence rather than
principle: it is the one target no static analyser reads, and its installed form
could only be exercised by a Windows CI job that does not exist. If a consumer
asks, this is the alternative to revisit — the CMake is already shaped for it
(`EXPORT_NAME winrt_ble` is set).

**Rename the transport headers to `smply/transport/…`** so the installed layout
needs no nested root. Cleaner, and rejected as churn: it moves four headers and
rewrites include lines in the Windows adapter and the HIL rig, neither of which
can be compiled on the machine that would be making the change.

## Consequences

* A third-party BLE adapter is now writable against the package alone. That is
  the first time it has been true.
* `tests/consumption/` proves all three consumption modes —`find_package`,
  `add_subdirectory` and `FetchContent` — against one shared smoke program, and
  the `find_package` mode is the only one that can catch an export-name defect.
* Four follow-up rows close, and the two that stay open (`winrt_ble` for a
  consumer who asks, `dfu_app` likewise) now have a written reason rather than
  a deferral.
* An installed **sanitizer** build still does not carry the runtime's link
  options, because they ride on `smply_internal_options` (P15a). Nobody ships
  one; if that changes, the options need a home outside that target.
