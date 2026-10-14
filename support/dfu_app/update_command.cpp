// SPDX-License-Identifier: Apache-2.0

#include "dfu_app/update_command.hpp"

#include "dfu_app/bootloader_mode.hpp"
#include "dfu_app/console_report.hpp"
#include "dfu_app/printable.hpp"

#include <ostream>
#include <variant>

namespace smply::dfu_app {

UpdatePlan UpdateCommand::plan() const
{
    UpdatePlan out;
    out.mode = mode;
    out.fallback_mode = fallback_mode;
    out.allow_no_revert = allow_no_revert;
    out.check_downgrade = check_downgrade;
    return out;
}

FlagResult parse_update_flag(std::span<const std::string> args, std::size_t& index,
                             const UpdateFlags& offered, UpdateCommand& out)
{
    const std::string& flag = args[index];
    const bool has_value = index + 1 < args.size();
    // The value of a flag that takes one: consumed only once the flag is known.
    const auto value = [&]() -> const std::string& { return args[++index]; };

    if (flag == "--quiet") {
        out.quiet = true;
        return FlagResult::Taken;
    }
    if (flag == "--allow-no-revert") {
        out.allow_no_revert = true;
        return FlagResult::Taken;
    }
    if (flag == "--no-downgrade-check") {
        out.check_downgrade = false;
        return FlagResult::Taken;
    }
    const bool takes_value =
        flag == "--image" || flag == "--image-secondary" || flag == "--mode" ||
        flag == "--fallback-mode" ||
        (offered.packages && (flag == "--package" || flag == "--commit" || flag == "--board"));
    if (!takes_value) {
        return FlagResult::Unknown;
    }
    if (!has_value) {
        return FlagResult::Invalid;
    }
    if (flag == "--image") {
        out.image_path = value();
    } else if (flag == "--image-secondary") {
        out.image_secondary_path = value();
    } else if (flag == "--package") {
        out.package_path = value();
    } else if (flag == "--mode") {
        const std::optional<UpdateMode> mode = parse_update_mode(value());
        if (!mode.has_value()) {
            return FlagResult::Invalid;
        }
        out.mode = *mode;
    } else if (flag == "--fallback-mode") {
        out.fallback_mode = parse_mcuboot_mode(value());
        if (!out.fallback_mode.has_value()) {
            return FlagResult::Invalid;
        }
    } else if (flag == "--commit") {
        const auto commit = parse_commit(value());
        if (!commit.has_value()) {
            return FlagResult::Invalid;
        }
        out.commits.push_back(*commit);
    } else {
        // --board: a board name could match nothing when it names no board.
        out.board = parse_board(value());
        if (!out.board.has_value()) {
            return FlagResult::Invalid;
        }
    }
    return FlagResult::Taken;
}

bool valid(const UpdateCommand& command, bool needs_named_input) noexcept
{
    const unsigned sources = (command.image_path.empty() ? 0U : 1U) +
                             (command.package_path.empty() ? 0U : 1U) + command.generated_packages;
    if (sources > 1) {
        return false;
    }
    // A board is compared only with a package's manifest, and only a package
    // has images for another party to commit.
    if (!command.package_mode() && (command.board.has_value() || !command.commits.empty())) {
        return false;
    }
    // A second build goes with a first one named on the command line, and
    // never with a package, which names its own files.
    if (!command.image_secondary_path.empty() &&
        (command.image_path.empty() || command.package_mode())) {
        return false;
    }
    return !needs_named_input || !command.image_path.empty() || !command.package_path.empty();
}

void print_update_flags_help(std::ostream& out, const UpdateFlags& offered)
{
    out << "  --image PATH  the signed MCUboot image to install\n"
           "  --image-secondary PATH  direct-XIP: the same image linked for the\n"
           "                secondary slot (--image is then the primary slot's build);\n"
           "                the one for the slot the device is not running is sent\n";
    if (offered.packages) {
        out << "  --package PATH  a multi-image DFU package (docs/multi-image.md): image 0\n"
               "                is confirmed here, every other image is left to the device\n"
               "  --commit N=client|device  with a package: who commits image N (repeatable)\n"
               "  --board NAME  with a package: refuse it, before anything is sent, unless\n"
               "                every image names this board; NAME is a board name or a\n"
               "                board target (nrf54l15dk or nrf54l15dk/nrf54l15/cpuapp)\n";
    }
    out << "  --mode MODE   test-then-confirm (default) | confirm-immediately | upload-only\n"
           "  --fallback-mode M  the MCUboot mode to assume when the device does not\n"
           "                report one, named as the report prints it: single-slot,\n"
           "                swap-using-scratch, upgrade-only, swap-using-move, direct-xip,\n"
           "                direct-xip-with-revert, ram-load, firmware-loader,\n"
           "                single-slot-ram-load, swap-using-offset\n"
           "  --allow-no-revert  update a device whose bootloader cannot revert\n"
           "                (upgrade-only); without it, such an update is refused\n"
           "  --no-downgrade-check  send an image older than the running one even\n"
           "                when the device prevents downgrades; it will refuse it at boot\n"
           "  --quiet       print only the outcome\n";
}

UpdateConsole::UpdateConsole(std::string_view tool, bool quiet, std::ostream& out,
                             std::ostream& err)
    : tool_{tool}, quiet_{quiet}, out_{&out}, err_{&err}
{}

void UpdateConsole::stamp_lines()
{
    stamped_from_ = std::chrono::steady_clock::now();
}

void UpdateConsole::error(const Error& error) const
{
    *err_ << tool_ << ": " << printable(to_string(error)) << '\n';
}

void UpdateConsole::error(std::string_view context, const Error& error) const
{
    *err_ << tool_ << ": " << printable(context) << ": " << printable(to_string(error)) << '\n';
}

std::unique_ptr<UpdateInputs>
UpdateConsole::open_inputs(const UpdateCommand& command,
                           std::optional<std::vector<std::byte>> generated_package) const
{
    const auto read_inputs = [&]() -> Result<std::unique_ptr<UpdateInputs>> {
        if (generated_package.has_value()) {
            return UpdateInputs::from_package_bytes(std::move(*generated_package), command.board);
        }
        if (!command.package_path.empty()) {
            return UpdateInputs::from_package(command.package_path, command.board);
        }
        return UpdateInputs::from_files(command.image_path, command.image_secondary_path);
    };
    Result<std::unique_ptr<UpdateInputs>> read = read_inputs();
    if (!read.has_value()) {
        error(read.error());
        return nullptr;
    }
    std::unique_ptr<UpdateInputs> inputs = std::move(*read);
    for (const auto& [image, commit] : command.commits) {
        if (const Result<void> set = inputs->set_commit(image, commit); !set.has_value()) {
            error("--commit " + std::to_string(image), set.error());
            return nullptr;
        }
    }
    return inputs;
}

std::string UpdateConsole::prefix() const
{
    if (!stamped_from_.has_value()) {
        return "  ";
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - *stamped_from_);
    return "  [" + std::to_string(elapsed.count()) + " ms] ";
}

void UpdateConsole::event(const UpdateEvent& event) const
{
    if (quiet_) {
        return;
    }
    // One handler per kind of event: std::visit refuses to compile if a kind
    // is left out. Output only: the update run itself acts on the last three.
    std::visit(overloaded{
                   [&](const UpdateStateChanged& changed) {
                       *out_ << prefix() << to_string(changed.to) << '\n';
                   },
                   [&](const UploadProgress& progress) {
                       if (progress.total == 0) {
                           return;
                       }
                       *out_ << '\r' << prefix() << "uploading " << progress.transferred << '/'
                             << progress.total << " bytes" << std::flush;
                       if (progress.transferred == progress.total) {
                           *out_ << '\n';
                       }
                   },
                   [&](const DisconnectExpected&) {
                       *out_ << prefix() << "the device is about to reboot\n";
                   },
                   [](const ReconnectRequired&) {},
                   [](const ConfirmationRequired&) {},
                   [](const UpdateFinished&) {},
               },
               event);
}

void UpdateConsole::confirming() const
{
    if (!quiet_) {
        *out_ << prefix() << "the new image is running unconfirmed; confirming\n";
    }
}

bool UpdateConsole::finish(const Result<UpdateReport>& outcome, const UpdateReport& report) const
{
    if (!outcome.has_value()) {
        *err_ << tool_ << ": update failed: " << printable(to_string(outcome.error())) << '\n';
        print_failure_details(report, *err_);
        return false;
    }
    const UpdateReport& done = *outcome;
    *out_ << "update " << to_string(done.final_state) << '\n';
    *out_ << "  bytes transferred: " << done.bytes_transferred
          << (done.upload_skipped ? " (the device already held this image)" : "") << '\n';
    if (done.rolled_back) {
        *out_ << "  the device reverted to its previous image\n";
    }
    if (done.revert_pending) {
        *out_ << "  a swap is scheduled but unconfirmed: it will revert on the next reset\n";
    }
    *out_ << "  " << describe_mode(done) << '\n';
    print_images(done, *out_);
    return done.final_state == UpdateState::Completed;
}

} // namespace smply::dfu_app
