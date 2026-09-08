// SPDX-License-Identifier: Apache-2.0

#include "support/bench.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fstream>
#include <iterator>

namespace smply::hil {
namespace {

std::optional<std::string> environment(const char* name)
{
    // std::getenv is what the standard offers; MSVC flags it as unsafe because
    // the returned buffer is shared, which is irrelevant for a read-once value.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const char* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe)
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string{value};
}

} // namespace

std::optional<std::uint64_t> parse_address(const std::string& text)
{
    std::uint64_t value = 0;
    unsigned digits = 0;
    for (const char c : text) {
        if (c == ':' || c == '-') {
            continue;
        }
        unsigned digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<unsigned>(c - 'a') + 10U;
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<unsigned>(c - 'A') + 10U;
        } else {
            return std::nullopt;
        }
        value = (value << 4U) | digit;
        if (++digits > 12) {
            return std::nullopt;
        }
    }
    if (digits != 12) {
        return std::nullopt;
    }
    return value;
}

std::optional<Bench> bench_from_environment(std::string& why)
{
    Bench bench;
    const auto address = environment("SMPLY_HIL_ADDRESS");
    if (!address.has_value()) {
        why = "SMPLY_HIL_ADDRESS is not set";
        return std::nullopt;
    }
    const auto parsed = parse_address(*address);
    if (!parsed.has_value()) {
        why = "SMPLY_HIL_ADDRESS is not a Bluetooth address: " + *address;
        return std::nullopt;
    }
    bench.address = *parsed;

    const auto a = environment("SMPLY_HIL_IMAGE_A");
    const auto b = environment("SMPLY_HIL_IMAGE_B");
    if (!a.has_value() || !b.has_value()) {
        why = "SMPLY_HIL_IMAGE_A / SMPLY_HIL_IMAGE_B are not both set";
        return std::nullopt;
    }
    bench.image_a = *a;
    bench.image_b = *b;
    if (!read_file(bench.image_a).has_value() || !read_file(bench.image_b).has_value()) {
        why = "an image file cannot be read: " + bench.image_a + " / " + bench.image_b;
        return std::nullopt;
    }
    return bench;
}

std::optional<std::vector<std::byte>> read_file(const std::string& path)
{
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::nullopt;
    }
    std::vector<char> raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    if (raw.empty()) {
        return std::nullopt;
    }
    std::vector<std::byte> out(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
    }
    return out;
}

Bench require_bench()
{
    std::string why;
    const auto bench = bench_from_environment(why);
    if (!bench.has_value()) {
        SKIP("bench unavailable: " << why);
    }
    return bench.value_or(Bench{});
}

} // namespace smply::hil
