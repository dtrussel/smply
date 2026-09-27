// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_GROUPS_SETTINGS_HPP
#define SMPLY_GROUPS_SETTINGS_HPP

/// \file
/// The settings (config) management group, group 3 (docs/protocol-notes.md
/// section 11).
///
/// Read, write and erase one setting, and ask the device to commit, load or
/// save its settings. Like the OS group, this is a thin encoder/decoder over
/// `SmpClient`: it allocates no sequence numbers, sets no deadlines and
/// interprets no `rc`.
///
/// **A setting's value is bytes, not text.** Its type and encoding belong to
/// the application that registered the setting on the device; nothing in the
/// protocol says what they are. smply sends and returns the bytes exactly, and
/// never interprets them.
///
/// **Setting a value is not persisting it.** A write changes the value the
/// device's settings handler holds at run time; `save()` writes it to storage
/// and `commit()` asks the handlers to apply what they have been given. Which
/// of those a given setting needs is the device application's design.
///
/// **Threading and lifetime.** As everywhere: calls and callbacks happen on the
/// client context, a callback never runs inside the call that started the
/// operation, and whatever a callback captures must outlive the `SmpClient`
/// (see `smply/smp_client.hpp`). A `SettingsManagement` is a handle onto a
/// client, so it must not outlive it either.

#include "smply/bytes.hpp"
#include "smply/error.hpp"
#include "smply/limits.hpp"
#include "smply/result.hpp"
#include "smply/smp_client.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace smply {

/// A setting's value, as read from the device.
struct SettingValue
{
    /// The value's bytes exactly as the device sent them. May be empty. Never
    /// longer than the `max_size` the read asked for, or
    /// `limits::kMaxSettingValueLength` when it asked for none.
    std::vector<std::byte> value;

    /// The device's own read limit, reported only when the read asked for more
    /// than it supports: `CONFIG_MCUMGR_GRP_SETTINGS_VALUE_LEN`. When present,
    /// a value that filled it may have been cut short by the device
    /// (docs/protocol-notes.md section 11).
    std::optional<std::uint32_t> max_size;

    [[nodiscard]] friend bool operator==(const SettingValue&, const SettingValue&) = default;
};

/// The settings group's own error codes, `settings_mgmt_ret_code_t`
/// (docs/protocol-notes.md section 3).
///
/// Group-scoped, like `ImageError`: use `settings_error()`, which checks the
/// group before reading the number. A value outside this list is carried
/// through numerically rather than rejected (docs/protocol-notes.md section 9,
/// A2).
enum class SettingsError : std::uint16_t
{
    Ok = 0,
    Unknown = 1,
    /// The name is too long for the device's `CONFIG_MCUMGR_GRP_SETTINGS_NAME_LEN`.
    KeyTooLong = 2,
    /// No setting of that name.
    KeyNotFound = 3,
    /// The setting's handler cannot be read.
    ReadNotSupported = 4,
    /// No settings handler owns the name's first component.
    RootKeyNotFound = 5,
    /// The setting's handler cannot be written.
    WriteNotSupported = 6,
    /// The setting cannot be deleted.
    DeleteNotSupported = 7,
    /// The subtree or setting cannot be saved.
    SaveNotSupported = 8,
    /// A single setting could not be saved because its value is longer than
    /// the device can read back to compare.
    SaveFailedValueTooLongToRead = 9,
};

/// The settings-group code an error carries, if it carries one.
///
/// Returns `std::nullopt` unless the device reported a **group-scoped** `rc`
/// belonging to the settings group. Two things make that common:
///
/// * Over SMP v1 -- smply's default -- a server built with
///   `CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL` translates the code onto
///   `mcumgr_err_t` and drops the group: `KeyNotFound` arrives as
///   `SmpError::NoEntry`, `KeyTooLong` as `SmpError::InvalidArgument`, and
///   most of the rest as `SmpError::Unknown` (docs/protocol-notes.md
///   section 3). Check `smp_error()` as well.
/// * A device's settings access hook may refuse with another group's code
///   (docs/protocol-notes.md section 9, A32), which is not a settings error
///   and is not reported as one.
[[nodiscard]] std::optional<SettingsError> settings_error(const Error& error) noexcept;

/// Settings read, write, erase, commit, load and save.
///
/// Holds a reference to the client and no state of its own, so several may
/// exist over one client and any may be destroyed at any time. Destroying it
/// does not cancel requests it issued -- the returned `RequestHandle` does
/// that.
///
/// **Names.** Every call that takes a setting name rejects one that is empty,
/// longer than `limits::kMaxSettingNameLength`, or contains a NUL byte, with
/// `ErrorCode::InvalidArgument` on the next poll -- the device would refuse the
/// first, and silently act on a shorter name for the last. The device's own
/// limit is usually lower: a Zephyr server refuses a name of
/// `CONFIG_MCUMGR_GRP_SETTINGS_NAME_LEN` bytes or more (31 usable by default)
/// with `SettingsError::KeyTooLong`. A name is borrowed for the call only.
class SettingsManagement
{
public:
    explicit SettingsManagement(SmpClient& client) noexcept;

    /// Reads the setting named \p name.
    ///
    /// Asks for no particular size, so the device returns at most its own
    /// `CONFIG_MCUMGR_GRP_SETTINGS_VALUE_LEN` bytes -- 32 by default. How a
    /// longer value is treated then depends on the device application's
    /// settings handler, so read with `max_size` when a value may be longer
    /// (docs/protocol-notes.md section 9, A31).
    RequestHandle read(std::string_view name, Callback<SettingValue> on_done);

    /// \overload Asks for at most \p max_size bytes.
    ///
    /// \p max_size must be between 1 and `limits::kMaxSettingValueLength`, or
    /// the call fails with `ErrorCode::InvalidArgument`. A device whose own
    /// limit is lower reads that much instead, and says so in
    /// `SettingValue::max_size`. A reply longer than \p max_size is refused as
    /// `ErrorCode::CborDecode`.
    RequestHandle read(std::string_view name, std::uint32_t max_size,
                       Callback<SettingValue> on_done);

    /// Sets the setting named \p name to \p value, exactly as given.
    ///
    /// \p value may be empty, and may be at most
    /// `limits::kMaxSettingValueLength` bytes, or the call fails with
    /// `ErrorCode::InvalidArgument`. It is borrowed for the call only. The new
    /// value is not persisted until it is saved.
    RequestHandle write(std::string_view name, ConstBytes value, Callback<void> on_done);

    /// Deletes the setting named \p name from the device's storage.
    ///
    /// MCUmgr calls this command "delete"; smply's verb for removing stored
    /// data is erase.
    RequestHandle erase(std::string_view name, Callback<void> on_done);

    /// Asks every settings handler on the device to apply the values it has
    /// been given.
    RequestHandle commit(Callback<void> on_done);

    /// Asks the device to reload its settings from storage.
    RequestHandle load(Callback<void> on_done);

    /// Asks the device to save every setting to storage.
    RequestHandle save(Callback<void> on_done);

    /// \overload Saves only the subtree named \p name, or the one setting of
    /// that name when the device is built with
    /// `CONFIG_SETTINGS_SAVE_SINGLE_SUBTREE_WITHOUT_MODIFICATION`.
    RequestHandle save(std::string_view name, Callback<void> on_done);

private:
    SmpClient* client_;
};

} // namespace smply

#endif // SMPLY_GROUPS_SETTINGS_HPP
