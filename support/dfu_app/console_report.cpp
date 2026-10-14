// SPDX-License-Identifier: Apache-2.0

#include "dfu_app/console_report.hpp"

#include "dfu_app/bootloader_mode.hpp"

namespace smply::dfu_app {

std::string_view device_image_state(const ImageReport& image) noexcept
{
    if (image.committed) {
        return "committed";
    }
    return image.applied ? "applied, not committed" : "not applied";
}

void print_images(const UpdateReport& report, std::ostream& out)
{
    if (report.images.size() < 2) {
        return;
    }
    for (const ImageReport& image : report.images) {
        out << "  image " << image.image << " ("
            << (image.commit == CommitBy::Client ? "client" : "device") << "): ";
        if (image.commit == CommitBy::Device) {
            out << device_image_state(image);
        } else if (image.rolled_back) {
            out << "reverted";
        } else {
            out << image.bytes_transferred << " bytes transferred";
        }
        out << '\n';
    }
}

void print_failure_details(const UpdateReport& report, std::ostream& out)
{
    out << "  " << describe_mode(report) << '\n';
    print_images(report, out);
    if (report.revert_pending) {
        out << "  a swap is scheduled but unconfirmed: it will revert on the next reset\n";
    }
}

} // namespace smply::dfu_app
