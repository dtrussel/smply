// SPDX-License-Identifier: Apache-2.0

/// \file
/// The statistics (group 2) and settings (group 3) cases: the first time
/// either group meets a device (docs/protocol-notes.md sections 10 and 11).
///
/// Everything these groups know about a Zephyr server was read from source;
/// these cases are where it is observed instead. Each case asserts what
/// protocol-notes.md section 9 inferred -- A29, A31, A33, A34 -- so a failure
/// here is a finding to record against that row, not a test to loosen.
///
/// What the cases need from the peer (tests/hil/README.md):
///
/// * the sample's own statistics group, `smp_svr_stats`, with one 32-bit
///   counter, `ticks`, incremented once a second by its main loop;
/// * the bench-only settings handler of the BL54L15 recipe
///   (`firmware/bl54l15/settings_module/`): `smply/v` read/write up to 64
///   bytes, `smply/ro` read-only, `smply/commits` counting `h_commit` calls;
/// * `CONFIG_MCUMGR_GRP_SETTINGS_NAME_LEN` and `_VALUE_LEN` at their default
///   32, and `CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL` on (as on the WB55).
///
/// The WB55 peer has the statistics group but no settings: its settings cases
/// fail at the first request, which is the honest result for that bench.
///
/// Run each group twice, as for `o2`: once as is (SMP v1) and once with
/// `SMPLY_HIL_SMP_VERSION=2`. The refusal cases assert the version's own
/// answer either way.

#include "support/bench.hpp"
#include "support/rig.hpp"

#include "smply/error.hpp"
#include "smply/groups/settings.hpp"
#include "smply/groups/statistics.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace smply;      // NOLINT(google-build-using-namespace) -- a test
using namespace smply::hil; // NOLINT(google-build-using-namespace)

namespace {

/// The server's `CONFIG_MCUMGR_GRP_SETTINGS_VALUE_LEN` on the bench peer.
constexpr std::uint32_t kDeviceValueLen = 32;
/// `CONFIG_MCUMGR_GRP_SETTINGS_NAME_LEN` / `CONFIG_MCUMGR_GRP_STAT_MAX_NAME_LEN`:
/// a name of this many bytes or more is refused.
constexpr std::size_t kDeviceNameLen = 32;

/// A connected client, no images involved.
struct MgmtSession
{
    Bench bench = require_bench();
    Rig rig{bench};

    MgmtSession()
    {
        try {
            REQUIRE(rig.connect().has_value());
            rig.timeline().note(std::string{"SMP v"} +
                                (bench.smp_version == Version::V2 ? "2" : "1"));
        } catch (...) {
            std::cout << rig.timeline().dump() << std::flush;
            throw;
        }
    }

    ~MgmtSession()
    {
        rig.record_send_counters();
        std::cout << rig.timeline().dump() << std::flush;
    }

    MgmtSession(const MgmtSession&) = delete;
    MgmtSession& operator=(const MgmtSession&) = delete;

    [[nodiscard]] bool v2() const noexcept
    {
        return bench.smp_version == Version::V2;
    }

    /// Records the device's report for a refusal, so the bundle shows the
    /// exact `(group, rc)` whatever the assertion made of it.
    void note_refusal(const char* what, const Error& error)
    {
        std::string line = std::string{what} + ": " + to_string(error);
        if (error.mgmt().has_value()) {
            const MgmtError& m = *error.mgmt();
            line += m.group_scoped ? " [group " + std::to_string(static_cast<int>(m.group)) +
                                         " rc " + std::to_string(m.rc) + "]"
                                   : " [flat rc " + std::to_string(m.rc) + "]";
        }
        rig.timeline().note(line);
    }
};

std::vector<std::byte> bytes_of(std::string_view text)
{
    std::vector<std::byte> out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

/// The bench handler's `smply/commits`: a little-endian u32.
std::uint32_t commits_of(const SettingValue& value)
{
    REQUIRE(value.value.size() == 4);
    std::uint32_t n = 0;
    for (std::size_t i = 4; i-- > 0;) {
        n = (n << 8U) | std::to_integer<std::uint32_t>(value.value[i]);
    }
    return n;
}

std::uint32_t read_commits(MgmtSession& s)
{
    const auto value = s.rig.setting_read("smply/commits");
    REQUIRE(value.has_value());
    return commits_of(*value);
}

} // namespace

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

TEST_CASE("hil: stat -- the device lists its groups and a counter advances", "[hil][stat]")
{
    MgmtSession s;

    const auto groups = s.rig.stat_groups();
    REQUIRE(groups.has_value());
    std::string names;
    for (const std::string& name : *groups) {
        names += " " + name;
    }
    s.rig.timeline().note("statistics groups:" + names);
    REQUIRE(std::find(groups->begin(), groups->end(), "smp_svr_stats") != groups->end());

    const auto first = s.rig.stat_read("smp_svr_stats");
    REQUIRE(first.has_value());
    CHECK(first->name == "smp_svr_stats");
    const StatisticsField* ticks = first->find("ticks");
    REQUIRE(ticks != nullptr);
    // A28: a Zephyr server writes every value as 32 bits. Only a 64-bit
    // statistic above 2^32 - 1 could show the truncation, and the sample has
    // none, so this is consistency with A28, not a demonstration of it.
    CHECK(ticks->value <= UINT32_MAX);

    std::this_thread::sleep_for(std::chrono::milliseconds{2500});
    const auto second = s.rig.stat_read("smp_svr_stats");
    REQUIRE(second.has_value());
    const StatisticsField* later = second->find("ticks");
    REQUIRE(later != nullptr);
    s.rig.timeline().metric("stat_ticks_first", static_cast<std::int64_t>(ticks->value));
    s.rig.timeline().metric("stat_ticks_second", static_cast<std::int64_t>(later->value));
    // Once a second in the sample's main loop: 2.5 s is at least two ticks.
    CHECK(later->value >= ticks->value + 2);
    CHECK(s.rig.stats().timeouts == 0);
}

TEST_CASE("hil: stat -- an unknown group and an over-long name are refused as the source says",
          "[hil][stat][refusal]")
{
    MgmtSession s;

    // A29: an unknown group is INVALID_STAT_NAME (3), not INVALID_GROUP. Which
    // *group* the server files it under depends on the Zephyr revision (A35):
    // upstream (the WB55's pin) says MGMT_GROUP_ID_STAT, so v2 sees a
    // statistics error and v1 a flat ENOENT (A34); NCS v3.3.0's fork (the
    // BL54L15) says ZEPHYR_MGMT_GRP_BASIC, so v2 sees (63, 3) -- no statistics
    // error at all -- and v1 group 63's translation, a flat EUNKNOWN. Either is
    // accepted; anything else is a new finding. The variant seen is recorded.
    const auto unknown = s.rig.stat_read("smply-no-such-group");
    REQUIRE_FALSE(unknown.has_value());
    s.note_refusal("unknown group", unknown.error());
    CHECK(unknown.error().code() == ErrorCode::ProtocolError);
    REQUIRE(unknown.error().mgmt().has_value());
    const MgmtError& m = *unknown.error().mgmt();
    if (s.v2()) {
        const bool upstream = statistics_error(unknown.error()) == StatisticsError::InvalidStatName;
        const bool ncs_basic = m.group_scoped && m.group == Group::ZephyrBasic && m.rc == 3;
        s.rig.timeline().note(upstream    ? "A35 variant: statistics group (upstream)"
                              : ncs_basic ? "A35 variant: Zephyr basic group (NCS v3.3.0)"
                                          : "A35 variant: neither -- a new finding");
        CHECK((upstream || ncs_basic));
    } else {
        CHECK_FALSE(statistics_error(unknown.error()).has_value());
        const auto flat = smp_error(unknown.error());
        s.rig.timeline().note(flat == SmpError::NoEntry ? "A35 variant: flat ENOENT (upstream)"
                              : flat == SmpError::Unknown
                                  ? "A35 variant: flat EUNKNOWN (NCS v3.3.0)"
                                  : "A35 variant: neither -- a new finding");
        CHECK((flat == SmpError::NoEntry || flat == SmpError::Unknown));
    }

    // A33: a name of MAX_NAME_LEN bytes or more is a flat EINVAL, in both
    // versions -- the handler returns it rather than writing a group error.
    const std::string long_name(kDeviceNameLen, 'x');
    const auto too_long = s.rig.stat_read(long_name);
    REQUIRE_FALSE(too_long.has_value());
    s.note_refusal("over-long group name", too_long.error());
    CHECK(smp_error(too_long.error()) == SmpError::InvalidArgument);
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

// No commas in a case name: run_hil.py selects a case by its name, and Catch2
// reads a comma in a test spec as "or", splitting the name into four filters
// that match nothing.
TEST_CASE("hil: settings -- a value round-trips through write read commit save load and delete",
          "[hil][settings]")
{
    MgmtSession s;
    const auto saved = bytes_of("smply-saved-0123");
    const auto unsaved = bytes_of("unsaved");

    REQUIRE(s.rig.setting_write("smply/v", ConstBytes{saved}).has_value());
    const auto read = s.rig.setting_read("smply/v");
    REQUIRE(read.has_value());
    CHECK(read->value == saved);
    // A31: the limit is reported only when the read asked for more than it.
    CHECK_FALSE(read->max_size.has_value());

    // commit runs every handler's h_commit; the bench handler counts them.
    const std::uint32_t before_commit = read_commits(s);
    REQUIRE(s.rig.settings_commit().has_value());
    CHECK(read_commits(s) == before_commit + 1);

    // save writes the handler's export to storage. A later unsaved write
    // changes only the run-time value, and load puts the stored one back
    // (and commits again: settings_load() ends in settings_commit()).
    REQUIRE(s.rig.settings_save().has_value());
    REQUIRE(s.rig.setting_write("smply/v", ConstBytes{unsaved}).has_value());
    const std::uint32_t before_load = read_commits(s);
    REQUIRE(s.rig.settings_load().has_value());
    const auto reloaded = s.rig.setting_read("smply/v");
    REQUIRE(reloaded.has_value());
    CHECK(reloaded->value == saved);
    CHECK(read_commits(s) == before_load + 1);

    // delete removes the stored value, not the run-time one: after it, load
    // has nothing to put back, so an unsaved write survives a load.
    REQUIRE(s.rig.setting_erase("smply/v").has_value());
    REQUIRE(s.rig.setting_write("smply/v", ConstBytes{unsaved}).has_value());
    REQUIRE(s.rig.settings_load().has_value());
    const auto after_delete = s.rig.setting_read("smply/v");
    REQUIRE(after_delete.has_value());
    CHECK(after_delete->value == unsaved);

    // A named save of one subtree is accepted too.
    REQUIRE(s.rig.settings_save(std::string_view{"smply"}).has_value());
    CHECK(s.rig.stats().timeouts == 0);
}

TEST_CASE("hil: settings -- the read limit is VALUE_LEN and is reported only when asked for more",
          "[hil][settings]")
{
    MgmtSession s;
    // Longer than the device's VALUE_LEN (32), within the handler's 64.
    const auto long_value = bytes_of("0123456789abcdefghijklmnopqrstuvwxyzABCD");
    REQUIRE(long_value.size() == 40);
    REQUIRE(s.rig.setting_write("smply/v", ConstBytes{long_value}).has_value());

    // No max_size: the device reads into VALUE_LEN bytes, says nothing about
    // it, and the bench handler truncates (a handler's choice, A31).
    const auto plain = s.rig.setting_read("smply/v");
    REQUIRE(plain.has_value());
    s.rig.timeline().metric("settings_plain_read_len",
                            static_cast<std::int64_t>(plain->value.size()));
    CHECK(plain->value.size() == kDeviceValueLen);
    CHECK_FALSE(plain->max_size.has_value());

    // Asking for more than VALUE_LEN: lowered to it, and reported (A31) --
    // VALUE_LEN, not NAME_LEN as Zephyr's documentation says.
    const auto asked_more = s.rig.setting_read("smply/v", 64);
    REQUIRE(asked_more.has_value());
    CHECK(asked_more->value.size() == kDeviceValueLen);
    REQUIRE(asked_more->max_size.has_value());
    CHECK(*asked_more->max_size == kDeviceValueLen);
    s.rig.timeline().metric("settings_reported_max_size",
                            static_cast<std::int64_t>(asked_more->max_size.value_or(0)));

    // Asking for less: exactly that much, nothing reported.
    const auto asked_less = s.rig.setting_read("smply/v", 16);
    REQUIRE(asked_less.has_value());
    CHECK(asked_less->value.size() == 16);
    CHECK_FALSE(asked_less->max_size.has_value());
    CHECK(std::equal(asked_less->value.begin(), asked_less->value.end(), long_value.begin()));
}

TEST_CASE("hil: settings -- refusals carry what this server's SMP version allows",
          "[hil][settings][refusal]")
{
    MgmtSession s;

    struct Expectation
    {
        const char* what;
        SettingsError v2;
        SmpError v1;
    };

    // A34's v1 translation, from settings_mgmt.c's legacy mapping.
    const auto check = [&s](const Result<void>& result, const Expectation& e) {
        REQUIRE_FALSE(result.has_value());
        s.note_refusal(e.what, result.error());
        CHECK(result.error().code() == ErrorCode::ProtocolError);
        if (s.v2()) {
            CHECK(settings_error(result.error()) == e.v2);
        } else {
            CHECK_FALSE(settings_error(result.error()).has_value());
            CHECK(smp_error(result.error()) == e.v1);
        }
    };
    const auto as_void = [](const Result<SettingValue>& r) -> Result<void> {
        if (r.has_value()) {
            return {};
        }
        return fail(r.error());
    };

    check(as_void(s.rig.setting_read("smplynoroot/x")),
          {"unknown root", SettingsError::RootKeyNotFound, SmpError::Unknown});
    check(as_void(s.rig.setting_read("smply/nosuch")),
          {"unknown key", SettingsError::KeyNotFound, SmpError::NoEntry});
    check(s.rig.setting_write("smply/ro", ConstBytes{bytes_of("x")}),
          {"read-only key", SettingsError::WriteNotSupported, SmpError::Unknown});
    // A33: NAME_LEN bytes or more is KEY_TOO_LONG, which v1 makes EINVAL.
    const std::string long_name = "smply/" + std::string(kDeviceNameLen - 6, 'x');
    REQUIRE(long_name.size() == kDeviceNameLen);
    check(as_void(s.rig.setting_read(long_name)),
          {"over-long name", SettingsError::KeyTooLong, SmpError::InvalidArgument});
    CHECK(s.rig.stats().timeouts == 0);
}
