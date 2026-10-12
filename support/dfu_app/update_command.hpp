// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_DFU_APP_UPDATE_COMMAND_HPP
#define SMPLY_DFU_APP_UPDATE_COMMAND_HPP

/// \file
/// The update command every example tool runs: its shared flags, the one set
/// of rules they are checked by, the image list and plan they name, and the
/// console output of the update -- its events, its errors and its report.
///
/// Three tools ran the same command with three copies of all of it, and the
/// copies drifted: different checks on the same flags, a failure report only
/// one of them printed in full, and a device's `rsn` printed raw by all
/// three. What differs between tools stays in each: the link, the stub
/// device and its demo flags, a tool's own modes, and any line it adds to the
/// report.
///
/// A tool parses its own flags first and hands every other one to
/// `parse_update_flag()`, then checks the result with `valid()`.

#include "dfu_app/update_inputs.hpp"
#include "dfu_package/dfu_package.hpp"

#include "smply/dfu/firmware_updater.hpp"
#include "smply/error.hpp"
#include "smply/result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace smply::dfu_app {

/// The shared flags, parsed.
struct UpdateCommand
{
    /// `--image`: the file to install, image 0.
    std::string image_path;
    /// `--image-secondary`: the same image linked for the secondary slot, for
    /// a direct-XIP device; `image_path` is then the primary slot's build.
    std::string image_secondary_path;
    /// `--package`: a multi-image DFU package (docs/multi-image.md).
    std::string package_path;
    /// Packages the tool was asked to generate, by flags of its own such as
    /// `--demo-package`. Each is one more thing to install.
    unsigned generated_packages = 0;
    /// `--commit N=client|device`, in the order given.
    std::vector<std::pair<std::uint32_t, CommitBy>> commits;
    /// `--board`: the board every image of the package must be for
    /// (docs/protocol-notes.md section 5).
    std::optional<dfu_package::BoardName> board;
    /// `--mode`.
    UpdateMode mode = UpdateMode::TestThenConfirm;
    /// `--fallback-mode`: the mode to assume when the device does not report
    /// one (ADR-0025).
    std::optional<McubootMode> fallback_mode;
    /// `--allow-no-revert`: accept an update the bootloader cannot revert.
    bool allow_no_revert = false;
    /// `--no-downgrade-check`: skip the downgrade check (ADR-0025).
    bool check_downgrade = true;
    /// `--quiet`: print only the outcome.
    bool quiet = false;

    /// A package is what is installed, read or generated.
    [[nodiscard]] bool package_mode() const noexcept
    {
        return generated_packages > 0 || !package_path.empty();
    }

    /// The plan the flags ask for. A tool sets its link's timing on top.
    [[nodiscard]] UpdatePlan plan() const;
};

/// Which of the shared flags a tool offers.
struct UpdateFlags
{
    /// `--package`, `--commit` and `--board`. A tool without package support
    /// rejects all three as unknown flags.
    bool packages = true;
};

/// What `parse_update_flag()` made of a flag.
enum class FlagResult : std::uint8_t
{
    /// A shared flag, recorded.
    Taken,
    /// Not a shared flag the tool offers.
    Unknown,
    /// A shared flag with a missing or unusable value.
    Invalid,
};

/// Parses the shared flag at `args[index]`, and its value if it takes one,
/// into \p out. On `Taken`, \p index is left on the last argument consumed.
[[nodiscard]] FlagResult parse_update_flag(std::span<const std::string> args, std::size_t& index,
                                           const UpdateFlags& offered, UpdateCommand& out);

/// The rules for the shared flags, the same for every tool:
///
/// * one thing to install at most: an image, a package file, or a generated
///   package;
/// * `--commit` and `--board` only with a package, the one thing that has
///   several images or names a board;
/// * `--image-secondary` only beside a named `--image`, and never with a
///   package, which names its own files;
/// * with \p needs_named_input, an `--image` or a `--package` is required: a
///   tool updating real hardware must not generate firmware for it.
[[nodiscard]] bool valid(const UpdateCommand& command, bool needs_named_input) noexcept;

/// The help lines for the shared flags \p offered, as the tools print them.
void print_update_flags_help(std::ostream& out, const UpdateFlags& offered);

/// The update's console: every line the shared command prints.
///
/// Errors go to the error stream as `"<tool>: <error>"`, escaped with
/// `printable()`: `Error::reason()` can be a device's `rsn` or a package's
/// manifest, attacker-controlled text that must not reach a terminal raw
/// (docs/security.md T12). Progress goes to the output stream unless the
/// command is quiet. The report is printed either way.
class UpdateConsole
{
public:
    UpdateConsole(std::string_view tool, bool quiet, std::ostream& out, std::ostream& err);

    /// Stamps each progress line with the milliseconds since this call: on a
    /// real link, the only way to see where the time goes.
    void stamp_lines();

    /// `"<tool>: <error>"`.
    void error(const Error& error) const;
    /// `"<tool>: <context>: <error>"`.
    void error(std::string_view context, const Error& error) const;

    /// The image list \p command names, with its `--commit`s applied: \p
    /// generated_package when the tool generated one, else the package file,
    /// else the image files. Null once it has said why it cannot.
    [[nodiscard]] std::unique_ptr<UpdateInputs>
    open_inputs(const UpdateCommand& command,
                std::optional<std::vector<std::byte>> generated_package = std::nullopt) const;

    /// One update event, as a progress line.
    void event(const UpdateEvent& event) const;

    /// What the approve hook says before it confirms.
    void confirming() const;

    /// The update's end. A completed or stopped update's report goes to the
    /// output stream; a failure, and what it left behind, to the error stream
    /// (`print_failure_details()`).
    ///
    /// \param outcome The run's result.
    /// \param report  The updater's report, which a failure still carries.
    /// \return True when the update completed.
    bool finish(const Result<UpdateReport>& outcome, const UpdateReport& report) const;

private:
    /// The line prefix: two spaces, and the time when stamped.
    [[nodiscard]] std::string prefix() const;

    std::string tool_;
    bool quiet_;
    std::ostream* out_;
    std::ostream* err_;
    std::optional<std::chrono::steady_clock::time_point> stamped_from_;
};

} // namespace smply::dfu_app

#endif // SMPLY_DFU_APP_UPDATE_COMMAND_HPP
