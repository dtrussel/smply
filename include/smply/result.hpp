// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_RESULT_HPP
#define SMPLY_RESULT_HPP

/// \file
/// `Result<T>` -- smply's fallible-return type (ADR-0002).
///
/// `Result<T>` is `std::expected<T, Error>` where the standard library provides
/// it, and an API-compatible subset otherwise. Every fallible operation returns
/// one, or delivers one to a callback; no error is ever signalled by a sentinel
/// value or an exception.
///
/// **Use only the common subset**: `has_value()` / `operator bool`,
/// `operator*`, `operator->`, `error()` and `value_or()`. In particular there
/// is no `value()` -- the standard one throws, and smply treats protocol
/// failures as ordinary outcomes rather than exceptional ones. Construct the
/// failure state with `fail()` rather than naming either backing directly.

#include "smply/error.hpp"

#include <functional>
#include <utility>
#include <version>

#if defined(__cpp_lib_expected) && (__cpp_lib_expected >= 202202L) &&                              \
    !defined(SMPLY_FORCE_FALLBACK_EXPECTED)
#include <expected>
/// 1 when Result is backed by std::expected, 0 when by smply's own.
#define SMPLY_USING_STD_EXPECTED 1
#else
#include "smply/detail/expected.hpp"
#define SMPLY_USING_STD_EXPECTED 0
#endif

namespace smply {

namespace detail {

// The conditional lives here so the public aliases below stay single,
// documented declarations rather than one per preprocessor branch.
#if SMPLY_USING_STD_EXPECTED
template<class T, class E>
using expected_backing = std::expected<T, E>;
template<class E>
using unexpected_backing = std::unexpected<E>;
#else
template<class T, class E>
using expected_backing = detail::expected<T, E>;
template<class E>
using unexpected_backing = detail::unexpected<E>;
#endif

} // namespace detail

/// The expected template backing `Result`, whichever is in use.
///
/// Named here so nothing outside this header has to know which one it is.
/// Prefer `Result<T>`; this alias exists for the rare generic context that
/// needs a different error type.
template<class T, class E>
using expected = detail::expected_backing<T, E>;

/// The unexpected wrapper matching `expected`. Prefer `fail()` at call sites.
template<class E>
using unexpected = detail::unexpected_backing<E>;

/// The result of a fallible operation: a T, or an Error.
///
/// `Result<void>` is the form used by operations that either succeed or fail
/// without producing a value.
template<class T>
using Result = expected<T, Error>;

/// Builds the failure state of any `Result`.
///
/// Preferred over naming `unexpected` at call sites, so the choice of backing
/// stays invisible:
/// \code
///     if (length > limit) {
///         return fail(ErrorCode::MessageTooLarge, "assembler");
///     }
/// \endcode
[[nodiscard]] inline unexpected<Error> fail(Error error) noexcept
{
    return unexpected<Error>{std::move(error)};
}

/// \overload Convenience for the common case of a bare code plus a call site.
[[nodiscard]] inline unexpected<Error> fail(ErrorCode code, const char* where = nullptr) noexcept
{
    return unexpected<Error>{Error{code, where}};
}

/// How an asynchronous operation reports its outcome.
///
/// Invoked exactly once. `Callback<void>` is the form for an operation with no
/// value to return, which still has a failure to report.
///
/// Whatever the callback captures must outlive the `SmpClient` driving the
/// operation: the client's destructor completes anything still outstanding, so
/// a callback can run during that destruction (see `smply/smp_client.hpp`).
template<class T>
using Callback = std::function<void(Result<T>)>;

} // namespace smply

#endif // SMPLY_RESULT_HPP
