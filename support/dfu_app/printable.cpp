// SPDX-License-Identifier: Apache-2.0

#include "dfu_app/printable.hpp"

#include <cstddef>

namespace smply::dfu_app {

std::string printable(std::string_view text)
{
    constexpr std::string_view kHex = "0123456789ABCDEF";
    const auto escape = [&kHex](std::string& out, unsigned byte) {
        out += "\\x";
        out += kHex[(byte >> 4U) & 0x0FU];
        out += kHex[byte & 0x0FU];
    };
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        const bool c1 = byte == 0xC2U && i + 1 < text.size() &&
                        static_cast<unsigned char>(text[i + 1]) >= 0x80U &&
                        static_cast<unsigned char>(text[i + 1]) <= 0x9FU;
        if (c1) {
            escape(out, byte);
            escape(out, static_cast<unsigned char>(text[++i]));
        } else if (byte < 0x20U || byte == 0x7FU) {
            escape(out, byte);
        } else {
            out += text[i];
        }
    }
    return out;
}

} // namespace smply::dfu_app
