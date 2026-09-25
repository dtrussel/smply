// SPDX-License-Identifier: Apache-2.0

#include "pty_stub.hpp"

#include "stub_device/device_link.hpp"
#include "stub_device/stub_device.hpp"

#include "serial_port/serial_link.hpp"

#include "smply/bytes.hpp"
#include "smply/error.hpp"

#include <fcntl.h>
#include <poll.h>
#include <stdlib.h> // NOLINT(modernize-deprecated-headers) -- posix_openpt, mkdtemp: POSIX, not <cstdlib>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace smply::example {
namespace {

/// What a booting Zephyr device prints on its console before it answers
/// anything. None of it starts with a frame marker, so a correct client
/// ignores it (protocol-notes section 8) -- and the last line is longer than
/// any frame can be, so the adapter's line splitter must drop it, and count it.
[[nodiscard]] std::vector<std::byte> boot_output()
{
    std::string text = "*** Booting MCUboot v2.1.0 ***\r\n"
                       "I: Starting bootloader\r\n"
                       "I: Jumping to the first image slot\r\n"
                       "*** Booting Zephyr OS build v4.1.0 ***\r\n";
    text += "[00:00:00.012,000] <inf> smp_sample: a log line longer than any frame: ";
    text += std::string(120, '.');
    text += "\r\n";

    std::vector<std::byte> out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

/// Opens a pseudo-terminal pair: the master, and a slave the stub keeps open
/// itself -- so the master never sees a hang-up merely because no client has
/// opened the port yet, which would make poll() spin -- set raw, so nothing
/// written in either direction is echoed or translated before a client
/// configures it.
struct PtyPair
{
    int master = -1;
    int keeper = -1;
    std::string slave_name;
};

[[nodiscard]] bool open_pair(PtyPair& out)
{
    out.master = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (out.master < 0) {
        return false;
    }
    std::array<char, 128> name{};
    if (::grantpt(out.master) != 0 || ::unlockpt(out.master) != 0 ||
        ::ptsname_r(out.master, name.data(), name.size()) != 0 ||
        ::fcntl(out.master, F_SETFL, O_NONBLOCK) != 0 ||
        ::fcntl(out.master, F_SETFD, FD_CLOEXEC) != 0) {
        return false;
    }
    out.slave_name = name.data();
    out.keeper = ::open(out.slave_name.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (out.keeper < 0) {
        return false;
    }
    termios raw{};
    if (::tcgetattr(out.keeper, &raw) != 0) {
        return false;
    }
    ::cfmakeraw(&raw);
    return ::tcsetattr(out.keeper, TCSANOW, &raw) == 0;
}

void close_fd(int& fd) noexcept
{
    if (fd >= 0) {
        static_cast<void>(::close(fd));
        fd = -1;
    }
}

/// Points \p link at \p target atomically: a new symlink beside it, renamed
/// over the old one. A client opening the path sees the old port or the new
/// one, never neither.
[[nodiscard]] bool publish(const std::string& link, const std::string& target)
{
    const std::string staging = link + ".new";
    std::error_code ignored;
    std::filesystem::remove(staging, ignored);
    std::error_code ec;
    std::filesystem::create_symlink(target, staging, ec);
    if (ec) {
        return false;
    }
    std::filesystem::rename(staging, link, ec);
    return !ec;
}

} // namespace

PtyStub::PtyStub(StubDevice& device, ResetShape shape) : device_{&device}, shape_{shape}
{
    std::string pattern = (std::filesystem::temp_directory_path() / "serial_dfu_XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr) {
        return;
    }
    directory_ = pattern;
    link_path_ = directory_ + "/port";

    std::array<int, 2> ends{-1, -1};
    if (::pipe(ends.data()) != 0) {
        return;
    }
    wake_read_ = ends[0];
    wake_write_ = ends[1];
    static_cast<void>(::fcntl(wake_read_, F_SETFL, O_NONBLOCK));
    static_cast<void>(::fcntl(wake_write_, F_SETFL, O_NONBLOCK));

    PtyPair pair;
    if (!open_pair(pair) || !publish(link_path_, pair.slave_name)) {
        close_fd(pair.master);
        close_fd(pair.keeper);
        return;
    }
    master_ = pair.master;
    keeper_ = pair.keeper;
    ok_ = true;
    reader_ = std::thread{[this] { read_loop(); }};
}

PtyStub::~PtyStub()
{
    stop();
    const std::lock_guard<std::mutex> lock{mutex_};
    close_fd(master_);
    close_fd(keeper_);
    close_fd(retiring_master_);
    close_fd(retiring_keeper_);
    close_fd(wake_read_);
    close_fd(wake_write_);
    if (!directory_.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }
}

void PtyStub::stop() noexcept
{
    stop_.store(true);
    wake();
    if (reader_.joinable()) {
        reader_.join();
    }
}

void PtyStub::wake() const noexcept
{
    if (wake_write_ >= 0) {
        const std::byte token{1};
        const ssize_t ignored = ::write(wake_write_, &token, 1); // see serial_port_posix.cpp
        static_cast<void>(ignored);
    }
}

void PtyStub::deliver(std::vector<std::byte> message)
{
    const std::vector<std::byte> framed = transport::frame_message(ConstBytes{message});
    const std::lock_guard<std::mutex> lock{mutex_};
    write_locked(framed);
}

void PtyStub::device_resetting(Error /*reason*/)
{
    if (shape_ == ResetShape::Uart) {
        return; // a UART stays open; the device just goes quiet
    }
    // A USB CDC port vanishes. The next one is allocated *before* the old one
    // is released, so it is guaranteed a different /dev/pts/N -- the rename
    // is the part of O7 worth exercising. The symlink goes too, as a by-id
    // link does while the device is off the bus: a client retrying the path
    // meanwhile gets "no such port", which is what it must learn to retry.
    PtyPair next;
    const std::lock_guard<std::mutex> lock{mutex_};
    std::error_code ignored;
    std::filesystem::remove(link_path_, ignored);
    if (!open_pair(next)) {
        close_fd(next.master);
        close_fd(next.keeper);
        return;
    }
    // The reader owns closing: it may be inside poll() on the old master.
    retiring_master_ = master_;
    retiring_keeper_ = keeper_;
    master_ = next.master;
    keeper_ = next.keeper;
    next_slave_ = next.slave_name;
    wake();
}

void PtyStub::device_booted()
{
    const std::lock_guard<std::mutex> lock{mutex_};
    if (shape_ == ResetShape::Cdc) {
        if (!next_slave_.empty()) {
            static_cast<void>(publish(link_path_, next_slave_));
            next_slave_.clear();
        }
        return;
    }
    write_locked(boot_output());
}

void PtyStub::write_locked(const std::vector<std::byte>& bytes)
{
    if (master_ < 0) {
        return;
    }
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t put = ::write(master_, bytes.data() + done, bytes.size() - done);
        if (put > 0) {
            done += static_cast<std::size_t>(put);
            continue;
        }
        if (put < 0 && errno != EAGAIN && errno != EINTR) {
            return; // the pty has gone; so has the answer
        }
        pollfd writable{master_, POLLOUT, 0};
        if (::poll(&writable, 1, 1000) <= 0) {
            return; // nobody is reading; a real UART would drop it too
        }
    }
}

void PtyStub::read_loop()
{
    // The device's receive side, modelled on Zephyr's: the netbuf holds the
    // serial length prefix and CRC as well as the packet, so a declared length
    // above buf_size - 2 cannot fit (protocol-notes section 9, A25).
    transport::SerialInbound rx{StubDevice::kBufSize - 2};
    std::array<std::byte, 512> buffer{};

    while (!stop_.load()) {
        int master = -1;
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            if (retiring_master_ >= 0) {
                close_fd(retiring_master_);
                close_fd(retiring_keeper_);
                rx.reset(); // half a frame from the old port is not the new one's
            }
            master = master_;
        }

        std::array<pollfd, 2> fds{};
        fds[0].fd = master; // a negative descriptor is ignored by poll()
        fds[0].events = POLLIN;
        fds[1].fd = wake_read_;
        fds[1].events = POLLIN;
        if (::poll(fds.data(), fds.size(), -1) < 0 && errno != EINTR) {
            return;
        }
        if ((fds[1].revents & POLLIN) != 0) {
            std::array<std::byte, 64> sink{};
            while (::read(wake_read_, sink.data(), sink.size()) > 0) {
            }
        }
        if ((fds[0].revents & POLLIN) == 0) {
            continue;
        }
        const ssize_t got = ::read(master, buffer.data(), buffer.size());
        if (got <= 0) {
            continue;
        }
        rx.feed(ConstBytes{buffer.data(), static_cast<std::size_t>(got)}, [this](ConstBytes p) {
            device_->submit(std::vector<std::byte>{p.begin(), p.end()});
        });
    }
}

} // namespace smply::example
