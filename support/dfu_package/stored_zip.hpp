// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_SUPPORT_DFU_PACKAGE_STORED_ZIP_HPP
#define SMPLY_SUPPORT_DFU_PACKAGE_STORED_ZIP_HPP

/// \file
/// The files of a zip archive whose entries are **stored**, not compressed.
///
/// That is the whole of what a DFU package needs: nRF Connect SDK's
/// `generate_zip.py` writes with Python's default `ZIP_STORED`
/// (docs/protocol-notes.md, S38), so a stored entry's bytes are the file's
/// bytes and can be handed out as a view, with nothing inflated or copied.
/// Anything else -- deflate, encryption, zip64, several disks -- is refused
/// with a message that says which, rather than misread (ADR-0021).
///
/// The archive is a file somebody gave the application, so it is untrusted:
/// every offset and length is bounded against the archive before it is used,
/// nothing is allocated on a size the archive states, and every entry's CRC-32
/// is checked.

#include "smply/bytes.hpp"
#include "smply/result.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace smply::dfu_package {

/// At most this many entries in the central directory. A package holds a
/// manifest and a few images.
inline constexpr std::size_t kMaxZipEntries = 32;

/// At most this long an entry name.
inline constexpr std::size_t kMaxZipNameLength = 255;

/// One file in the archive.
struct ZipEntry
{
    std::string name;
    /// The file's bytes: a view into the archive passed to `read_stored_zip()`.
    ConstBytes data;
};

/// Lists the files of \p archive, in central-directory order. Directory
/// entries are left out.
///
/// \return `InvalidArgument` for an archive smply does not read (a compressed
///         or encrypted entry, zip64, several disks), `MessageTooLarge` for
///         more than `kMaxZipEntries` entries, and `MalformedMessage` for
///         anything structurally wrong, a CRC mismatch or a name given twice.
[[nodiscard]] Result<std::vector<ZipEntry>> read_stored_zip(ConstBytes archive);

/// CRC-32 as zip uses it (the reflected 0xEDB88320 polynomial).
[[nodiscard]] std::uint32_t crc32(ConstBytes data) noexcept;

} // namespace smply::dfu_package

#endif // SMPLY_SUPPORT_DFU_PACKAGE_STORED_ZIP_HPP
