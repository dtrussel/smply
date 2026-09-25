// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_SRC_GROUPS_IMAGE_DECODE_HPP
#define SMPLY_SRC_GROUPS_IMAGE_DECODE_HPP

/// \file
/// The image group's response decoders. Each receives a reader already inside
/// the top-level map of a success response (`groups::complete()` opens it),
/// reads its fields and leaves the map. Every field is still untrusted.

#include "cbor/cbor.hpp"
#include "smply/groups/image.hpp"
#include "smply/result.hpp"

namespace smply::groups {

/// Decodes an image-state response: the answer to both get-state and
/// set-state, which share one shape.
[[nodiscard]] Result<ImageState> decode_state(cbor::Reader& reader);

/// Decodes a slot-info response.
[[nodiscard]] Result<SlotInfo> decode_slot_info(cbor::Reader& reader);

} // namespace smply::groups

#endif // SMPLY_SRC_GROUPS_IMAGE_DECODE_HPP
