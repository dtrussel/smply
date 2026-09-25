// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_SRC_GROUPS_IMAGE_DECODE_HPP
#define SMPLY_SRC_GROUPS_IMAGE_DECODE_HPP

/// \file
/// The image group's response decoders. Pure functions over a payload that
/// `SmpClient` has already checked for an MCUmgr error, so what reaches them is
/// a success response -- still untrusted in every field.

#include "smply/bytes.hpp"
#include "smply/groups/image.hpp"
#include "smply/result.hpp"

namespace smply::groups {

/// Decodes an image-state response: the answer to both get-state and
/// set-state, which share one shape.
[[nodiscard]] Result<ImageState> decode_state(ConstBytes payload);

/// Decodes a slot-info response.
[[nodiscard]] Result<SlotInfo> decode_slot_info(ConstBytes payload);

} // namespace smply::groups

#endif // SMPLY_SRC_GROUPS_IMAGE_DECODE_HPP
