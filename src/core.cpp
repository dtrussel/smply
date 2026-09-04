// SPDX-License-Identifier: Apache-2.0

#include "smply/clock.hpp"
#include "smply/error.hpp"
#include "smply/group.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <string>

namespace smply {
namespace {

class SystemClock final : public Clock
{
public:
    [[nodiscard]] TimePoint now() const noexcept override
    {
        return std::chrono::steady_clock::now();
    }
};

/// Appends `value` in decimal without going through iostreams or std::format.
void append_number(std::string& out, std::uint64_t value)
{
    std::array<char, 20> buffer{};
    std::size_t length = 0;
    do {
        buffer.at(length) = static_cast<char>('0' + (value % 10));
        ++length;
        value /= 10;
    } while (value != 0);
    while (length > 0) {
        --length;
        out.push_back(buffer.at(length));
    }
}

} // namespace

const Clock& system_clock() noexcept
{
    static const SystemClock instance;
    return instance;
}

const char* group_name(Group group) noexcept
{
    switch (group) {
    case Group::Os:
        return "os";
    case Group::Image:
        return "image";
    case Group::Stat:
        return "stat";
    case Group::Settings:
        return "settings";
    case Group::Log:
        return "log";
    case Group::Crash:
        return "crash";
    case Group::Split:
        return "split";
    case Group::Run:
        return "run";
    case Group::Fs:
        return "fs";
    case Group::Shell:
        return "shell";
    case Group::Enumeration:
        return "enumeration";
    case Group::Transport:
        return "transport";
    case Group::ZephyrBasic:
        return "zephyr-basic";
    case Group::PerUser:
        return "per-user";
    }
    // Reachable: Group is open, so any 16-bit value is legal on the wire. The
    // numeric value is preserved by the caller.
    return "unknown";
}

std::string_view to_string(ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::Ok:
        return "ok";
    case ErrorCode::InvalidArgument:
        return "invalid argument";
    case ErrorCode::InvalidState:
        return "invalid state";
    case ErrorCode::MalformedMessage:
        return "malformed SMP message";
    case ErrorCode::UnsupportedSmpVersion:
        return "unsupported SMP version";
    case ErrorCode::MessageTooLarge:
        return "message too large";
    case ErrorCode::CborEncode:
        return "CBOR encode failed";
    case ErrorCode::CborDecode:
        return "CBOR decode failed";
    case ErrorCode::ProtocolError:
        return "MCUmgr protocol error";
    case ErrorCode::UnexpectedResponse:
        return "unexpected response";
    case ErrorCode::Timeout:
        return "timed out";
    case ErrorCode::Cancelled:
        return "cancelled";
    case ErrorCode::TransportError:
        return "transport error";
    case ErrorCode::TransportBusy:
        return "transport busy";
    case ErrorCode::Disconnected:
        return "disconnected";
    case ErrorCode::ImageMismatch:
        return "image mismatch";
    case ErrorCode::UpdateFailed:
        return "firmware update failed";
    case ErrorCode::Internal:
        return "internal error";
    }
    // ErrorCode is smply's own closed enumeration; reaching here means someone
    // cast an out-of-range value into it.
    return "unrecognised error code";
}

std::string to_string(const Error& error)
{
    std::string out{to_string(error.code())};

    if (const auto& mgmt = error.mgmt(); mgmt.has_value()) {
        if (mgmt->group_scoped) {
            out += " [";
            out += group_name(mgmt->group);
            out += " rc=";
            append_number(out, mgmt->rc);
            out += ']';
        } else {
            out += " [smp rc=";
            append_number(out, mgmt->rc);
            out += ']';
        }
    }

    if (!error.reason().empty()) {
        out += ": ";
        out += error.reason();
    }

    if (error.where() != nullptr) {
        out += " (at ";
        out += error.where();
        out += ')';
    }

    return out;
}

} // namespace smply
