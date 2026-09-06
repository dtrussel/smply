// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_DETAIL_CLIENT_THREAD_HPP
#define SMPLY_DETAIL_CLIENT_THREAD_HPP

/// \file
/// The debug-only client-context check ADR-0004 promises.
///
/// ADR-0004 makes wrong-thread use *undefined behaviour* rather than a
/// detectable error, and lists this as the mitigation: "debug builds assert on
/// the owning thread id via a `SMPLY_ASSERT_CLIENT_THREAD()` check compiled out
/// in release". Until P14 the macro existed only in that sentence.
///
/// **This is the only place in the core that mentions a thread**, and it does so
/// only when `NDEBUG` is undefined. In a release build `ClientThreadGuard` is an
/// empty class with an empty `check()`, so nothing about threads survives into
/// the shipped library -- which is what `architecture.md` §5 means by "no hidden
/// threads anywhere in the core".
///
/// Where it lives matters. `SmpClient` is pimpl'd, so a member here changes
/// nothing a consumer can see, and `SmpClient::Impl` is defined in exactly one
/// translation unit, so a member that exists only in debug builds is not an ODR
/// problem. `OsManagement` and `ImageManagement` are **not** pimpl'd -- their
/// members sit in public headers, where a debug-only member would make the class
/// a different size in Debug and Release and break a consumer who mixes them.
/// So the check lives in `SmpClient` alone. It loses nothing: every group
/// operation and every updater effect reaches the wire through
/// `SmpClient::request()`, `defer()`, `poll()`, `cancel()` or
/// `rebind_transport()`, and every inbound event arrives through the three
/// listener callbacks. All eight are checked.
///
/// This does not contradict `design.md` §11 ("public entry points validate their
/// arguments and return `InvalidArgument` rather than asserting"). That rule is
/// about *arguments*, which a caller can get wrong by accident and recover from.
/// The calling thread is a *context* precondition: by the time it is wrong the
/// data races have already happened, there is no value to return it through, and
/// ADR-0004 has already classified it as UB.

#include <cassert>

#ifndef NDEBUG
#include <thread>
#endif

namespace smply::detail {

/// Remembers the thread that constructed it; `check()` asserts that it is still
/// the one calling. Empty and free in release builds.
class ClientThreadGuard
{
public:
#ifdef NDEBUG
    void check() const noexcept {}
#else
    ClientThreadGuard() noexcept : owner_{std::this_thread::get_id()} {}

    void check() const noexcept
    {
        assert(std::this_thread::get_id() == owner_ &&
               "smply: called from a thread other than the client context. All calls on "
               "SmpClient, the group clients and FirmwareUpdater -- and every "
               "TransportListener callback -- must happen on the thread that drives poll(). "
               "A transport whose driver delivers on another thread must marshal first; "
               "smply::Dispatcher exists for that. See ADR-0004.");
    }

private:
    std::thread::id owner_;
#endif
};

} // namespace smply::detail

/// Asserts that the caller is on the client context. A no-op in release.
///
/// Spelled with its guard rather than reaching for a member of a fixed name:
/// the check is worth nothing if a reader cannot see what it is checking.
#define SMPLY_ASSERT_CLIENT_THREAD(guard) (guard).check()

#endif // SMPLY_DETAIL_CLIENT_THREAD_HPP
