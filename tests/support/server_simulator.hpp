// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_TESTS_SERVER_SIMULATOR_HPP
#define SMPLY_TESTS_SERVER_SIMULATOR_HPP

/// \file
/// A deterministic in-memory MCUmgr device: groups 0 and 1, an MCUboot-like
/// swap, and the awkward answers a real server gives.
///
/// It exists because every layer below the DFU state machine is correct *in
/// isolation* and nothing proves the sequence. It is **not** a reference
/// implementation, and it is never linked into the library.
///
/// **It does not answer from inside `send()`.** `smply/smp_client.hpp` requires
/// that a transport not deliver inbound bytes before `send()` returns; a
/// simulator that replied inline would re-enter reassembly, which the assembler
/// refuses as an error (docs/design.md section 2). Instead it watches
/// `FakeTransport::sent()` and answers from `pump()`, which is the device's
/// turn in the test's loop:
///
/// \code
///     while (!done) {
///         sim.pump(clock.now());     // the device consumes and answers
///         client.poll(clock.now());  // the client sees the answer
///         clock.advance(1ms);
///     }
/// \endcode
///
/// `tests/component/harness.hpp` wraps that loop with an iteration budget.
///
/// **Precondition:** a test driving a simulator must not call
/// `FakeTransport::clear_sent()`. The simulator tracks how much of that vector
/// it has consumed, and clearing it would make the device answer requests
/// twice.
///
/// The behaviour is specified by docs/protocol-notes.md sections 5, 6 and 7,
/// which are written as server behaviour precisely so they can be implemented
/// here. Three answers matter more than the rest, because each makes a correct
/// client look broken, and all three fall out of implementing the real decision
/// order rather than being special cases:
///
/// * an upload can complete on the **first** packet, with no data transferred
///   (section 6, rule 9a);
/// * a retransmitted **final** chunk is answered `off == 0`, because the server
///   resets its session on completion (rule 9b);
/// * a wrong offset is answered with **success** and the offset the server
///   wants, which may be larger than the one it was sent (rule 5).

#include "fake_transport.hpp"

#include "smply/bytes.hpp"
#include "smply/clock.hpp"
#include "smply/groups/image.hpp"
#include "smply/smp/header.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace smply::test {

/// What MCUboot will do on the next boot (docs/protocol-notes.md section 7).
///
/// The client never sees this value; it sees the slot flags derived from it,
/// which is exactly the indirection that makes a trial boot hard to recognise.
enum class SwapType : std::uint8_t
{
    None,   ///< Boot the active slot again.
    Test,   ///< Swap on the next boot, then require a confirm.
    Perm,   ///< Swap on the next boot and keep it.
    Revert, ///< A trial boot is running unconfirmed; the next boot undoes it.
};

/// How this device is built.
///
/// Everything here is a *capability*: what the firmware was compiled with.
/// Scripted misbehaviour lives in the methods below instead, so that a config
/// stays a description of a device rather than becoming a test script.
///
/// The SMP version is deliberately **not** here. The version on the wire is the
/// client's to choose (`SmpClientConfig::smp_version`), and a real server
/// answers in the version it was asked in; a device with an opinion of its own
/// would not be modelling anything that exists.
struct ServerConfig
{
    /// `buf_size` from MCUmgr parameters: the whole-SMP-message budget.
    std::uint32_t buf_size = 256;

    /// False makes MCUmgr parameters answer `ENOTSUP`, as older servers do.
    bool supports_mcumgr_params = true;

    /// `CONFIG_MCUMGR_GRP_IMG_SLOT_INFO`. Off by default, as in Zephyr.
    bool supports_slot_info = false;

    /// `CONFIG_IMG_ENABLE_IMAGE_CHECK`: emits `match`, and enables the
    /// already-present check that completes an upload on the first packet.
    bool image_check_enabled = true;

    /// Omit `image` from image-state entries, as a single-image device does.
    bool single_image = true;

    /// `CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL`: an image-group code sent
    /// to a **v1** client is translated onto `mcumgr_err_t` and the payload
    /// rebuilt as a flat `rc` (docs/protocol-notes.md section 9, A16). With
    /// this false the `err` map survives even for a v1 request.
    bool translate_v1_errors = true;

    /// Slot capacity in bytes. An upload larger than this is refused with
    /// `InvalidImageTooLarge`. Zero means unbounded.
    std::uint32_t slot_size = 0;

    /// How long the device takes to answer. Zero answers within the same
    /// `pump()`.
    Duration response_delay{0};
};

/// An in-memory MCUmgr server driven through a `FakeTransport`.
///
/// One image, two slots: slot 0 is primary and always the running one, as it is
/// in a swap-based MCUboot build, and a swap exchanges the two slots' contents.
class ServerSimulator
{
public:
    /// \param transport The same transport the client under test is bound to.
    ///                  Must outlive this simulator.
    explicit ServerSimulator(FakeTransport& transport, ServerConfig config = {});

    ServerSimulator(const ServerSimulator&) = delete;
    ServerSimulator(ServerSimulator&&) = delete;
    ServerSimulator& operator=(const ServerSimulator&) = delete;
    ServerSimulator& operator=(ServerSimulator&&) = delete;
    ~ServerSimulator() = default;

    /// The device's turn: consume whatever the client has sent, and deliver
    /// whatever is now due.
    ///
    /// Requests that arrive *during* this call -- a client callback sending the
    /// next chunk as it receives the previous answer -- are left for the next
    /// `pump()`, which is what a real device does with a packet that arrives
    /// while it is still transmitting.
    void pump(TimePoint now);

    // --- Device state ------------------------------------------------------

    /// Puts an image into a slot, as though it had been flashed there.
    void load_slot(std::size_t slot, std::vector<std::byte> content);

    [[nodiscard]] ConstBytes slot_content(std::size_t slot) const;

    /// Moves the device onto a new link, keeping everything it holds.
    ///
    /// The application re-establishing a dropped connection gives the client a
    /// new transport; the device on the other end is still the same device,
    /// with the same flash and the same upload session. \p transport must
    /// outlive this simulator.
    void rebind_transport(FakeTransport& transport);

    /// Reboots, performing the swap the current state calls for
    /// (docs/protocol-notes.md section 7):
    ///
    /// | before | after           |
    /// | ------ | --------------- |
    /// | None   | unchanged       |
    /// | Test   | swapped, Revert |
    /// | Perm   | swapped, None   |
    /// | Revert | swapped back, None |
    ///
    /// A reboot also forgets any upload session, which is the `area_id == -1`
    /// case: a continuation then gets `off == 0` with no special handling.
    void reboot();

    [[nodiscard]] SwapType swap_type() const noexcept
    {
        return swap_;
    }

    /// True once a reset command has been accepted. A real device answers the
    /// reset and *then* goes down, so a test drops the link itself.
    [[nodiscard]] bool reset_requested() const noexcept
    {
        return reset_requested_;
    }

    // --- Scripted faults ---------------------------------------------------

    /// Answers the next upload chunk with \p off, whatever offset the device
    /// actually holds -- rule 5's correction, in either direction, including an
    /// offset *larger* than the one the client sent and the zero of rule 7.
    ///
    /// It changes only the answer, never the flash or the session, so the
    /// device stays self-consistent: a client that follows the correction is
    /// put right again on the next round trip, and one that computes its own
    /// offsets flashes a corrupt image. That asymmetry is the whole point.
    void answer_offset_once(std::uint64_t off);

    /// Fails the next image-group command with \p code.
    ///
    /// \param op When given, only a command with that operation is failed --
    ///           the group's read and write share command 0, so "fail the next
    ///           set-state" and "fail the next get-state" are otherwise the
    ///           same request. Without it a test that means one can silently
    ///           hit the other and pass for the wrong reason.
    void fail_next(ImageError code, std::optional<Operation> op = std::nullopt);

    /// Silently drops the next response, as a lost packet would.
    void drop_next_response();

    /// The next reset is refused with `EBUSY`, which a client retries with
    /// `force` (docs/protocol-notes.md section 5).
    void reset_busy_once();

    // --- Inspection --------------------------------------------------------

    /// The header of every request the device has processed, in order. This is
    /// what a test asserts a command *sequence* against.
    [[nodiscard]] const std::vector<Header>& requests() const noexcept
    {
        return requests_;
    }

    /// Bytes of image data actually written to a slot, across all sessions.
    /// Zero after an upload that completed via the already-present check.
    [[nodiscard]] std::uint64_t bytes_written() const noexcept
    {
        return bytes_written_;
    }

    /// Responses dropped by `drop_next_response()`.
    [[nodiscard]] std::size_t dropped() const noexcept
    {
        return dropped_;
    }

    /// The `force` flag of the most recent reset request, if it carried one.
    [[nodiscard]] std::optional<bool> last_reset_force() const noexcept
    {
        return last_reset_force_;
    }

private:
    /// A response waiting for its delivery time.
    struct Pending
    {
        TimePoint due;
        std::vector<std::byte> message;
    };

    /// The upload session, cleared on completion and on reboot.
    struct Session
    {
        bool active = false;
        std::size_t slot = 1;
        std::uint64_t off = 0;
        std::uint64_t size = 0;
        /// The `sha` exactly as the client sent it: the server accepts one
        /// trimmed to any length and remembers whatever it was given.
        std::vector<std::byte> sha;
    };

    void handle(const Header& header, ConstBytes payload, TimePoint now);
    void enqueue(const Header& request, ConstBytes payload, TimePoint now);

    // Group handlers. Each returns the response payload.
    [[nodiscard]] std::vector<std::byte> handle_os(const Header& header, ConstBytes payload);
    [[nodiscard]] std::vector<std::byte> handle_image(const Header& header, ConstBytes payload);
    [[nodiscard]] std::vector<std::byte> handle_upload(const Header& header, ConstBytes payload);
    [[nodiscard]] std::vector<std::byte> handle_state_read() const;
    [[nodiscard]] std::vector<std::byte> handle_state_write(Version version, ConstBytes payload);
    [[nodiscard]] std::vector<std::byte> handle_erase(Version version, ConstBytes payload);
    [[nodiscard]] std::vector<std::byte> handle_slot_info() const;

    /// The error payload for an image-group code, in whichever shape this
    /// device produces for \p version.
    [[nodiscard]] std::vector<std::byte> image_failure(Version version, ImageError code) const;
    /// A flat SMP-level failure, which never carries a group.
    [[nodiscard]] static std::vector<std::byte> smp_failure(Version version, SmpError code);

    /// Which slot the next boot runs, and how that boot is classified.
    [[nodiscard]] std::size_t next_boot_slot() const noexcept;

    /// Applies `boot_set_next()`'s rules; returns `ImageError::Ok` on success.
    [[nodiscard]] ImageError set_next_boot_slot(std::size_t slot, bool confirm);

    std::reference_wrapper<FakeTransport> transport_;
    ServerConfig config_;

    std::vector<std::vector<std::byte>> slots_{2};
    SwapType swap_ = SwapType::None;
    Session session_;

    std::vector<Pending> pending_;
    std::vector<Header> requests_;
    std::size_t consumed_ = 0;
    std::size_t dropped_ = 0;
    std::uint64_t bytes_written_ = 0;

    std::optional<std::uint64_t> forced_offset_;
    std::optional<ImageError> forced_failure_;
    std::optional<Operation> forced_failure_op_;
    std::optional<bool> last_reset_force_;
    bool drop_next_ = false;
    bool reset_busy_ = false;
    bool reset_requested_ = false;
};

} // namespace smply::test

#endif // SMPLY_TESTS_SERVER_SIMULATOR_HPP
