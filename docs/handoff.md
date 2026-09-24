# Working on smply

How to pick up work, how to finish it, and the standing caveats: rules that are
true of the code now and have each cost someone time. This file and
[`roadmap.md`](roadmap.md) are the project's working state. There is no session
log. The reasoning behind a change goes in its commit message
([ADR-0018](decisions/ADR-0018-maintenance-process.md)).

---

## Starting

1. **Read [`roadmap.md`](roadmap.md).** "In progress" names the current work.
   Otherwise, pick an item from the backlog whose "When" has arrived.
2. **Read [§ Standing caveats](#standing-caveats)** below.
3. **Read [`architecture.md`](architecture.md)**, and the sections of
   [`design.md`](design.md) and [`protocol-notes.md`](protocol-notes.md) that
   the work touches.
4. **Read the ADRs the work depends on** ([`decisions/`](decisions/)). Do not
   re-decide anything an accepted ADR settles. To change one, follow
   [§ Changing an architectural decision](#changing-an-architectural-decision).
5. **Build and test before changing anything**, so a failure you find later is
   known to be yours.

## While working

* **Stay inside the item.** Anything else you find goes into the roadmap's
  backlog, not into this change. Avoid unrelated refactoring.
* Write tests alongside the implementation, not after.
* Record newly discovered protocol behaviour in
  [`protocol-notes.md`](protocol-notes.md) **as you find it**.
* Keep each commit reviewable. A change growing past about 1,000 lines of diff
  should be split.

## Finishing: the checklist

1. Every Linux preset builds, **checking each build's exit status**, and `ctest`
   is green on each.
2. `tools/format.sh --check`, `tools/lint.sh`, `tools/check_public_headers.py`,
   `tools/check_deps.py` and `tools/check_docs.py` pass.
3. For a change under `src/`, the coverage and sanitizer presets pass
   (`tools/coverage.sh <build-dir> --enforce`). For a change under `tools/` or
   `cmake/`, `tools/verify_gates.sh` passes.
4. [`architecture.md`](architecture.md), [`design.md`](design.md) and
   [`api.md`](api.md) are updated wherever structure, behaviour or signatures
   changed, in the same commit.
5. New or changed ADRs are written for significant decisions.
6. [`roadmap.md`](roadmap.md) is updated. Delete finished items and add
   discovered ones, each with its "When".
7. If you learned something that will bite the next person, add it to the
   standing caveats below. If a caveat stopped being true, delete it.
8. The Definition of Done in [`quality-gates.md`](quality-gates.md) §12 holds.

## Changing an architectural decision

Never drift. If implementation shows that an ADR is wrong:

1. Identify the conflict precisely: which ADR, which claim, and what
   contradicts it.
2. Evaluate the consequences for the other components.
3. Write a **new ADR** that supersedes the old one, and set the old one's status
   to `Superseded by ADR-NNNN`. Do not edit the old decision's body.
4. Update [`architecture.md`](architecture.md) and [`design.md`](design.md).
5. Update the roadmap.
6. *Then* implement.

If the conflict cannot be resolved within the change, stop and record it as an
open question rather than guessing.

## Standing caveats

Everything here is true of the code as it stands. Add an entry when a lesson
will outlive the change that taught it. Delete an entry when it stops being
true.

**Trusting device data**

* **Bound every length before using it.** Never size an allocation on a number
  the device supplied.
* **`cbor::Reader` has two results that look alike.**
  * A getter returning `std::nullopt` means the field was **absent**. That is
    normal: MCUmgr omits a field rather than sending zero or false.
  * A field of the wrong *type* poisons the reader, and every field then looks
    absent.

  **Check `status()` before trusting any decoded struct**, or a malformed
  response reads as a successful one full of defaults.
* **Decoded views die with the callback.** A `std::string_view` or `ConstBytes`
  points into the assembler's buffer and is valid only for the callback. Copy
  anything that outlives it.
* **A bound that is not the tightest bound is not a bound.** Before adding a
  limit, check what the layer underneath already enforces.
  * `limits::kMaxCborNesting` is 14, one below QCBOR's own cap of 15. Reaching
    the cap needs a document one level deeper than the cap, so equalling
    QCBOR's value would not be enough.
  * `Reader::enter_map(key)` does not record a QCBOR error on failure. That is
    deliberate: it doubles as a probe for the optional `err` map. So a limit at
    or above QCBOR's would stop the descent silently, with `status()` clean.

**Layering**

* **Groups are thin.** They allocate no sequence numbers, set no deadlines and
  interpret no `rc`, because `SmpClient` has done all three before a response
  arrives. `src/groups/os/os_management.cpp` is the shape to copy.
* **A callback never runs inside the call that started the operation**,
  including argument rejections and first-chunk failures. `SmpClient::defer()`
  exists for exactly that.
* **`SmpClientConfig::max_in_flight` is 1 by default.** A second concurrent
  request fails with `InvalidState` rather than queueing.
* **A response that matches on `seq` but not on group, command or op is
  discarded**, and the request stays pending (ADR-0010). Do not "fix" this into
  a failure. The version field is deliberately not part of the match. Otherwise
  a peer that answered in the other version would turn a decodable response
  into a guaranteed timeout (protocol-notes §9, A23).
* **`TransportBusy` is a retry request, not a failure, and it should be rare.**
  The adapter admits one waiting message beside the one it is writing
  (`transports/common/send_queue.hpp`), which absorbs the device answering
  before the local write completes. A `TransportBusy` now means two messages
  really are outbound and the medium has stalled. Do not make a test tolerate
  it, and do not add a retry in the core. Nothing below `FirmwareUpdater` owns a
  clock, so such a retry would spin with zero elapsed time (`design.md` §6). A
  clock-driven backoff needs an ADR.
* **The `Dispatcher` belongs to the application, not to an adapter.** Several
  links may share one, as `examples/cli_dfu/main.cpp` shows. So an adapter must
  never call `clear()` or `drain()`: that would discard another transport's
  work, or run application closures from inside `close()`. Posted closures
  instead capture a strong reference to the adapter's state and check
  `LinkState::may_deliver()` before touching the listener.

**Lifetime**

* **A transport, and anything a callback captures, must outlive both the
  `SmpClient` and the `ImageManagement`**, because an upload session lives in
  the latter and its destructor completes the callback. Declare them *before*
  both. That includes a transport a rebind test introduces halfway through:
  `~SmpClient` detaches from whichever one it currently holds.

  Getting the order wrong shows up differently per toolchain: "pure virtual
  method called" under GCC, a stack-use-after-scope under Clang's ASan, or
  nothing at all. Only Clang's ASan reliably catches dangling callback
  captures, so build that preset.

**Protocol facts that bite**

* **Where Zephyr's documentation and source disagree, the source wins, and you
  must read both.** Reset's `force` is the example (protocol-notes §9, A15): the
  docs say integer, and the server decodes a boolean and silently ignores
  anything else.
* **An upload session does not survive a BLE disconnect** (A21). A resume after
  a reconnect is answered near offset 0 and re-sends the whole image. The same
  upload abandoned on a *live* link and resumed by another process continues
  from where it stopped. `UploadDriver::restart()` adopts whatever offset comes
  back. Size reconnect and deadline budgets for a full re-upload, not a resume.
* **SMP v1 destroys image-group error codes on a server with
  `CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL`** (A16, A24), which the bench
  peer and most shipping devices set.
  * The server may translate a group error onto `mcumgr_err_t` and rebuild the
    response. Two different refusals can both arrive as a flat `rc=1`.
  * So `image_error()` returning `nullopt` for a real image failure is normal
    on that configuration, not a malformed reply. Always check `smp_error()` as
    well.
  * Before writing any branch on `image_error()`, ask what a v1 peer sends
    instead. The lost-mark-for-test recovery in
    `src/dfu/update_state_machine.cpp` accepts both `ImageAlreadyPending` and a
    group-less `SmpError::BadState` for this reason.
  * To test that case, use `flat_failure()` in
    `tests/unit/test_update_state_machine.cpp`.
  * To get the group code itself, set `SmpClientConfig::smp_version =
    Version::V2`.
* **MCUmgr uses two different hashes. Do not merge them.**
  * The upload `sha` is SHA-256 over the whole file. It is the fixed 32-byte
    `Hash`, from `sha256(ImageSource&)`.
  * The image-state `hash` is MCUboot's `IMAGE_TLV_SHA` over header and body.
    It is `ImageHash`, 32 **or 64** bytes because a SHA-512 bootloader uses 64,
    from `find_image_tlv_hash()`.

  Conflating the two is the classic client bug, which is why they are
  different types.
* **The upload server's `off` is authoritative in every direction**: larger
  than what was sent, smaller, or zero (protocol-notes §6, rule 5). Never
  compute `next_off = off + sent`. Two consequences look like bugs and are not:
  * An upload can complete on its *first* packet when the device already holds
    the image (rule 9a).
  * A retransmitted *final* chunk is answered `off == 0`, because the server has
    already reset its session (rule 9b).
* **A retransmission repeats the payload, not the message.** It needs a new
  sequence number, because the timeout retired the old one and a reply carrying
  it would be discarded as late (protocol-notes §4).
* **`match` is meaningful only if the full 32-byte `sha` was sent**
  (protocol-notes §6, rule 9c). The final-chunk check compares against the
  stored `sha` zero-padded, with no length guard. A trimmed or absent `sha`
  makes a good upload report `match: false`. Always send all 32 bytes.
* **The slot flags of a trial boot read backwards** (protocol-notes §7). After a
  test swap, the running image reports `active` with **no** `confirmed`, and the
  slot holding the fallback reports `confirmed`. The flags are derived from the
  swap type, not stored, so there is no "pending bit" to consult instead.
* **An ordinary build denies confirming a slot that is not the running one**
  (`IMAGE_CONFIRMATION_DENIED`). The portable way to make an image permanent is
  test, reset, confirm. Re-requesting a swap that is *already* scheduled
  succeeds and does nothing, so an idempotent retry after a lost response is
  safe. Any other change returns `ALREADY_PENDING`.
* **"Absent means false" is only half the rule for image-state flags.** The
  server omits a false flag only under `CONFIG_MCUMGR_GRP_IMG_FRUGAL_LIST`.
  Otherwise it sends the flag explicitly. Absent means false, and present means
  whatever it says. Never read "key present" as "true".
* **The MCUboot TLV trailer is laid out as `bootutil_tlv_iter_begin()` walks
  it** (protocol-notes §7):
  * `it_tlv_tot` includes its own four-byte area header;
  * `ih_protect_tlv_size` must equal the protected area's `it_tlv_tot` exactly;
  * the two areas are walked as one contiguous run.

  A scan cannot spin, because every step advances at least the four-byte entry
  header. So `limits::kMaxImageTlvs` bounds the work, not termination.
* **A real device's CBOR is indefinite-length** (A18).
  * zcbor writes `0xBF`/`0x9F` … `0xFF` unless `CONFIG_ZCBOR_CANONICAL` is set,
    and nothing in MCUmgr sets it.
  * QCBOR mishandles two consecutive indefinite breaks when a map is left with
    `ExitMap()`. Sometimes it silently reads the parent map's entries as array
    elements.
  * So `cbor::Reader::for_each_map_in_array` decodes each element from its own
    byte range, in a child reader. Do not "tidy" it back into enter/exit.
  * Build any new response golden in **both** encodings. `test_cbor.cpp` shows
    the shape, and the `[hardware-golden]` cases carry a device's exact bytes.
* **The final upload chunk is slow to answer, and a retransmission can hide
  it** (A19). With the image check on, the device hashes the whole image before
  replying: 5.57 s for 134 KiB on the bench board. Here is what happens with a
  5 s deadline:
  1. The final chunk times out.
  2. The retransmitted chunk is answered with `off == 0`.
  3. A first packet then completes via rule 9a.
  4. The update **succeeds** while reporting "already present".

  `final_chunk_timeout` exists for this, and `already_present` is false once
  the session has made progress. When a hardware run prints `Completed`, read
  the client counters before believing nothing went wrong.
* **A device advertises the SMP service UUID but puts its name in the scan
  response** (protocol-notes §8, S22). Filtering on the UUID is reliable, but
  **matching on name needs an *active* scan**: a passive scan never requests
  the scan response, so it matches nothing and looks like a device that is
  switched off. Nothing obliges a product to advertise the UUID, so connecting
  by address must stay possible.

**The Windows half: run from a bench, never by CI**

* **No CI job puts a byte of the WinRT code on the air.** `windows-winrt`
  compiles `smply::winrt_ble` and `examples/winrt_ble_dfu/` at `/W4 /WX`, and
  links the adapter's smoke test on a runner with no radio. The adapter has run
  against a real device only from the bench, by hand. So a green badge means
  "it builds", and any change to that code is unproven until someone runs the
  bench again.
* **Three directories are outside clang-tidy and cppcheck**:
  `transports/winrt_ble/`, `examples/winrt_ble_dfu/` and `tests/hil/`. Both
  analysers run from a Linux build, where those translation units do not exist.
  `clang-format` still covers them, and MSVC `/W4 /WX` stands in for the
  analysers. **Never widen that filter to the substring `winrt`.**
  `verify_gates.sh` plants a portable decoy beside each directory and requires
  it to be analysed.

**Before you trust a green run**

* **A failed build leaves the previous test binary in place**, so `ctest` then
  reports the *old* suite passing. Check the build's exit status separately.
  Never read "N tests passed" as evidence that anything was rebuilt.
* **Never edit a source file while a background build is running.** The objects
  come out mixed, and Ninja then reports success because nothing is newer than
  what it has. The binary can fail in one preset only. If a preset fails in a
  way that makes no sense, `rm -rf build` before believing it.
* **Build every preset.** `cmake --list-presets` shows ten Linux presets, and all
  ten build in a Linux container: `linux-gcc`, `linux-clang`,
  `linux-gcc-release`, `linux-gcc-coverage`, `linux-gcc-fallback-expected`,
  `linux-gcc-cxx23-std-expected`, `linux-gcc-asan-ubsan`,
  `linux-clang-asan-ubsan`, `linux-clang-tsan` and `linux-clang-fuzz`. GCC,
  Clang and MSVC each reject things the others accept:
  * **GCC only:** `-Wuseless-cast` rejects a `static_cast` between
    `std::uint64_t` and `std::size_t`. They are the same type on a 64-bit host
    and a real narrowing on a 32-bit one. `image::narrow<To>()` in
    `src/image/source_reader.hpp` is the way round it.
  * **Clang only:** `std::vector<std::pair<std::string, T>>` inside `T` is
    undefined, because a `std::pair` of an incomplete type is. GCC compiles it.
    `std::vector<T>` inside `T` is specifically allowed.
  * **MSVC only:** `/w14242` rejects `std::pair<std::uint16_t,
    std::uint8_t>{0, 6}`, where the `int` literals narrow inside `pair`'s
    constructor template. Prefer a named aggregate to a `std::pair` of narrow
    integers. The Windows jobs run only in CI.
* **Build Release too.** Some template and lifetime bugs appear only above
  `-O0`, and `linux-gcc-release` exists to catch them.
  **`-Wnull-dereference` is deliberately absent** from the GCC set.
  `cmake/warnings.cmake` has the reasoning. Do not add it back without a
  Release build.
* **`cli_dfu_reconnect_gives_up` passes on its output, not its exit code.** It
  must fail *through* `FirmwareUpdater::reconnect_failed()`, so ctest matches
  "could not reconnect" rather than accepting any failure. A `WILL_FAIL` test
  that fails for another reason still reports green. Prefer a
  `PASS_REGULAR_EXPRESSION` for any test that is meant to fail.
* **A gate that passes has checked less than you think.** `check_docs.py` R5
  prints how many layout entries it skipped for this reason. The documentation
  rules catch *shapes* of drift, not wrong claims. A document describing a
  fixed defect as the design needs a person reading it against the code.
* **A gate's self-check fixture must not depend on content that later work will
  change.** A case that injects its violation by rewriting real text becomes a
  no-op when that text is reworded, and still reports PASS.
  `verify_gates.sh`'s `substitute` fails loudly when it changes nothing. Append
  synthetic content where you can. The mirror case matters just as much: a gate
  that fails on the *unmodified* tree makes every "rejects a violation" case
  pass. That is why the documentation cases start with an `expect_ok`, and why
  the scratch copy gets a git index (R5 reads `git ls-files`).
* **Read the uncovered-line list, not the percentage.** A gap of a few points
  has turned out to be genuinely reachable bounds checks. It has also turned
  out to be tests passing **without reaching the check they were named after**,
  because a fixture builder kept two fields in agreement. A malformation knob
  must change exactly one field, or it cannot express an inconsistency.
* **A green hardware suite means nothing unless `deferred_sends > 0`.** Zero
  everywhere says the send-admission race did not happen that run, not that the
  fix absorbed it. The counters are printed as `HIL-METRIC deferred_sends` and
  `refused_sends`. For any fix to something load-dependent, ask what the green
  run would have looked like if the fix did nothing.

**Packaging**

* **The installed package is four targets, and the list is a decision**:
  `smply::smply`, `smply::util`, `smply::transport_common` and
  `smply::asyncutil` (ADR-0019).
  `smply::dfu_app`, `smply::minicbor` and `smply::winrt_ble` are deliberately
  left out, and [ADR-0016](decisions/ADR-0016-installed-package-and-versioning.md)
  gives the reason for each. Adding a target to the package is a compatibility
  promise.
* **`EXPORT_NAME` is not optional on a target in the export set.**
  `install(EXPORT)` names an exported target `<namespace><target-name>`, and
  the in-tree `ALIAS` does not travel. Without `EXPORT_NAME` a target configures,
  builds and installs, and then fails only under `find_package`.
  `tests/consumption/find_package` is the only check that catches this.
* **An installed transport header keeps its in-tree spelling.**
  `#include "common/ble_framing.hpp"` works both in the tree and from a prefix,
  because the installed root is `<prefix>/include/smply/transports`. The
  spelling is the contract, so renaming the directory is a breaking change.
* **`tools/check_install.sh` runs three modes, and each catches something the
  others cannot.** All three build the same `tests/consumption/smoke.cpp`, so
  none can drift from the others.

  | Mode | The only mode that tests |
  |---|---|
  | `find_package` | the export |
  | `add_subdirectory` | smply as a subproject |
  | `FetchContent` | the declaration |

**Tooling**

* **Install `cppcheck`, `gcovr` and `libclang-rt-18-dev` first**, after an
  `apt-get update`. Without them, `tools/lint.sh` skips cppcheck with only a
  note, and `tools/coverage.sh` falls back to plain `gcov`, whose branch figure
  is not comparable. A stale package index makes "not installable" claims look
  true, so run `apt-get update` before believing one.
* **Coverage means exactly what `tools/coverage.sh` reports**: gcovr with
  `--exclude-throw-branches`. Under a different flag the same objects move by
  about 12 points. CI runs it with `--enforce`, which fails below 85 % line or
  75 % branch and refuses to run without gcovr. Running gcovr by hand, put the
  search path **first**. `--txt <build-dir>` takes the directory as that
  option's output file.
* **Delete the `.gcda` files before re-measuring**
  (`find <build-dir> -name '*.gcda' -delete`). Building over an existing
  coverage build mixes counts from two versions of the code. The only sign is a
  `libgcov profiling error` warning that scrolls past.
* **`LCOV_EXCL_LINE` excludes only the line it sits on.** On a comment line
  above the code it is silently ignored. For a multi-line guard, use
  `LCOV_EXCL_START` / `LCOV_EXCL_STOP`, and put the `STOP` inside the guard when
  the `else` arm is the ordinary path.
* **clang-tidy and cppcheck over the whole tree each take minutes.** Run them
  in the background, but **not at the same time as `tools/verify_gates.sh`**.
  That script points its scratch build at `build/linux-clang/_deps`, and
  rewriting a header clang-tidy has memory-mapped kills it with a bus error
  that looks like a compiler crash. `tools/lint.sh` passes cppcheck
  `-UCATCH_CONFIG_DISABLE -UCATCH_CONFIG_PREFIX_ALL`, without which it reports
  a `syntaxError` at the first `TEST_CASE`.
* **The fuzz targets are not part of `ctest`.** Configure `linux-clang-fuzz`,
  then run a target with a *copy* of `tests/fuzz/corpus/<target>/` as its
  argument. libFuzzer writes what it discovers into the directory it is given.
* **clang-tidy contradicts itself on coroutine hooks.** It wants an awaiter's
  `await_ready()` and a promise's `initial_suspend()` / `final_suspend()` made
  `static`. Made static, every `co_await` in the application then trips
  `readability-static-accessed-through-instance`, because the coroutine
  machinery calls the hooks through an object. Keep them members, with a
  `NOLINTNEXTLINE` on the line above, as `smply/async/task.hpp` does. A
  trailing `NOLINT` does not survive clang-format's line wrapping. Also,
  cppcheck cannot parse Catch2's `*_THROWS_*` macros; check an exception with
  a plain `try`/`catch`.
* **`??>` in a C++ string literal is a trigraph**, and `-Werror` rejects it. The
  device's `<???>` version placeholder needs a raw string literal.

**The hardware bench**

* **Read `tests/hil/README.md` before touching the board.**
  * The NUCLEO-WB55RG's Bluetooth controller runs on a second core, whose
    firmware Zephyr does not build.
  * It needs the **HCI-only** STM32CubeWB stack matching the pinned hal_stm32,
    installed through an incremental FUS upgrade.
  * Read the version table with `mode=HOTPLUG`. Under reset it reads as zeros.
* **Only one process may hold the board's COM port.** `uart_log.py` and
  `mcumgr-client` cannot run at once. Stop the logger first.
* **`flash_baseline.py` is the recovery primitive, and exits 2 for "no
  bench".** A supervisor must never turn that into a pass or a fail.
* **HCI capture on the bench is unsolved.** `tests/hil/README.md` says how far
  it got. A capture that is listening is not one that is recording: BTVS can
  produce a valid pcapng with zero packets. So `tools/hci_capture.py` gates on
  `capinfos` and reports `empty` as its own outcome.
* **On the Windows bench, the Linux build runs in WSL, but the shell scripts
  do not.**
  * The working tree is CRLF (`core.autocrlf=true`), so the scripts fail with
    `'bash\r': No such file or directory`. Run clang-tidy by hand from
    `compile_commands.json` instead.
  * Pass `-DFETCHCONTENT_SOURCE_DIR_CATCH2=…` and the same for `QCBOR`, so the
    configure reuses the Windows build's downloads.
  * There is no clang in that image, so the ASan and UBSan jobs stay with CI.
* **On Windows, build with the MSVC developer environment loaded**:
  `cmd /c "call VsDevCmd.bat -arch=x64 && cmake --build --preset windows-winrt"`.
  A shell that cannot find `cl` fails without building, and the stale binary is
  then what runs.
* **Bench evidence lives under `build/hil-evidence/` and is not in git.** Only
  reproducible inputs and concise results go into the repository.
