// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_GROUPS_STATISTICS_HPP
#define SMPLY_GROUPS_STATISTICS_HPP

/// \file
/// The statistics management group, group 2 (docs/protocol-notes.md
/// section 10).
///
/// Two commands: list the statistics groups a device has registered, and read
/// one group's counters. Both are read-only on the device. Like the OS group,
/// this is a thin encoder/decoder over `SmpClient`: it allocates no sequence
/// numbers, sets no deadlines and interprets no `rc`.
///
/// **Everything in a response is bounded before it is kept**: the number of
/// groups (`limits::kMaxStatisticsGroups`), the number of fields
/// (`limits::kMaxStatisticsFields`) and every name
/// (`limits::kMaxStatisticsNameLength`). A response over any of them fails as
/// `ErrorCode::CborDecode` rather than being truncated.
///
/// **Threading and lifetime.** As everywhere: calls and callbacks happen on the
/// client context, a callback never runs inside the call that started the
/// operation, and whatever a callback captures must outlive the `SmpClient`
/// (see `smply/smp_client.hpp`). A `StatisticsManagement` is a handle onto a
/// client, so it must not outlive it either.

#include "smply/error.hpp"
#include "smply/limits.hpp"
#include "smply/result.hpp"
#include "smply/smp_client.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace smply {

/// One named counter of a statistics group.
struct StatisticsField
{
    /// The field's name, exactly as the device reported it.
    std::string name;
    /// The counter's value.
    ///
    /// The protocol says unsigned integer and smply accepts the full 64-bit
    /// range, but a Zephyr server encodes every value as 32 bits, so a 64-bit
    /// statistic above 2^32 - 1 arrives truncated (docs/protocol-notes.md
    /// section 9, A28).
    std::uint64_t value = 0;

    [[nodiscard]] friend bool operator==(const StatisticsField&, const StatisticsField&) = default;
};

/// One statistics group's counters, as read from the device.
struct StatisticsGroup
{
    /// The group's name, as the device reported it.
    std::string name;
    /// The counters, in the order the device sent them. Names are unique: a
    /// response that repeats one is refused as `ErrorCode::CborDecode`. An
    /// empty list is a valid answer.
    std::vector<StatisticsField> fields;

    /// The field named \p field_name, or nullptr.
    [[nodiscard]] const StatisticsField* find(std::string_view field_name) const noexcept;

    [[nodiscard]] friend bool operator==(const StatisticsGroup&, const StatisticsGroup&) = default;
};

/// The statistics group's own error codes, `stat_mgmt_err_code_t`
/// (docs/protocol-notes.md section 3).
///
/// Group-scoped, like `ImageError`: use `statistics_error()`, which checks the
/// group before reading the number. A value outside this list is carried
/// through numerically rather than rejected (docs/protocol-notes.md section 9,
/// A2).
enum class StatisticsError : std::uint16_t
{
    Ok = 0,
    Unknown = 1,
    /// Defined, but a Zephyr server reports an unknown group as
    /// `InvalidStatName` instead (docs/protocol-notes.md section 9, A29).
    InvalidGroup = 2,
    /// No statistics group of the requested name.
    InvalidStatName = 3,
    /// A statistic of a size the server cannot encode.
    InvalidStatSize = 4,
    /// The server's walk of the group failed part-way.
    WalkAborted = 5,
};

/// The statistics-group code an error carries, if it carries one.
///
/// Returns `std::nullopt` unless the device reported a **group-scoped** `rc`
/// belonging to the statistics group. Over SMP v1 -- smply's default -- a
/// server built with `CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL` translates
/// the code onto `mcumgr_err_t` and drops the group: an unknown group then
/// arrives as `SmpError::NoEntry` (docs/protocol-notes.md section 3). Check
/// `smp_error()` as well.
[[nodiscard]] std::optional<StatisticsError> statistics_error(const Error& error) noexcept;

/// List statistics groups, and read one.
///
/// Holds a reference to the client and no state of its own, so several may
/// exist over one client and any may be destroyed at any time. Destroying it
/// does not cancel requests it issued -- the returned `RequestHandle` does
/// that.
class StatisticsManagement
{
public:
    explicit StatisticsManagement(SmpClient& client) noexcept;

    /// Lists the names of the statistics groups the device has registered, in
    /// the order it sent them.
    ///
    /// An empty list is a success: a device may register none.
    RequestHandle list_groups(Callback<std::vector<std::string>> on_done);

    /// Reads the counters of the group named \p name.
    ///
    /// \p name must be non-empty, at most `limits::kMaxStatisticsNameLength`
    /// bytes, and free of NUL bytes, or the call fails with
    /// `ErrorCode::InvalidArgument` (on the next poll, never inline). The
    /// device's own limit is usually lower: a Zephyr server refuses a name of
    /// `CONFIG_MCUMGR_GRP_STAT_MAX_NAME_LEN` bytes or more (31 usable by
    /// default) with `SmpError::InvalidArgument`.
    ///
    /// An unknown group is an ordinary `ErrorCode::ProtocolError`:
    /// `StatisticsError::InvalidStatName` over SMP v2, usually
    /// `SmpError::NoEntry` over v1.
    ///
    /// \p name is borrowed for the duration of this call only.
    RequestHandle read_group(std::string_view name, Callback<StatisticsGroup> on_done);

private:
    SmpClient* client_;
};

} // namespace smply

#endif // SMPLY_GROUPS_STATISTICS_HPP
