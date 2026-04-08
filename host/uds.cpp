#include "uds.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace waywallen::ipc {

namespace {

[[nodiscard]] std::system_error errno_error(const char* what) {
    return std::system_error { errno, std::generic_category(), what };
}

} // namespace

int connect_uds(const std::string& path) {
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        throw std::invalid_argument { "uds path too long: " + path };
    }

    int sock = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0) throw errno_error("socket(AF_UNIX)");

    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

    if (::connect(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        int e = errno;
        ::close(sock);
        errno = e;
        throw errno_error(("connect(" + path + ")").c_str());
    }
    return sock;
}

void send_frame(int sock, const nlohmann::json& body, std::span<const int> fds) {
    if (fds.size() > kMaxFdsPerMsg) {
        throw std::invalid_argument { "send_frame: too many fds" };
    }
    std::string json_body = body.dump();
    if (json_body.size() > kMaxFrameBytes) {
        throw std::length_error { "send_frame: body exceeds kMaxFrameBytes" };
    }

    const uint32_t body_len_be = htobe32(static_cast<uint32_t>(json_body.size()));
    std::vector<uint8_t> framed(4 + json_body.size());
    std::memcpy(framed.data(), &body_len_be, 4);
    std::memcpy(framed.data() + 4, json_body.data(), json_body.size());

    iovec iov {};
    iov.iov_base = framed.data();
    iov.iov_len  = framed.size();

    msghdr msg {};
    msg.msg_iov    = &iov;
    msg.msg_iovlen = 1;

    // Ancillary buffer for SCM_RIGHTS, sized to the worst case.
    alignas(cmsghdr) uint8_t cbuf[CMSG_SPACE(sizeof(int) * kMaxFdsPerMsg)] {};
    if (!fds.empty()) {
        msg.msg_control    = cbuf;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * fds.size());
        cmsghdr* cmsg      = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level   = SOL_SOCKET;
        cmsg->cmsg_type    = SCM_RIGHTS;
        cmsg->cmsg_len     = CMSG_LEN(sizeof(int) * fds.size());
        std::memcpy(CMSG_DATA(cmsg), fds.data(), sizeof(int) * fds.size());
    }

    while (true) {
        ssize_t sent = ::sendmsg(sock, &msg, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) continue;
            throw errno_error("sendmsg");
        }
        if (static_cast<size_t>(sent) != framed.size()) {
            throw std::runtime_error { "send_frame: short sendmsg" };
        }
        return;
    }
}

RecvResult recv_frame(int sock) {
    RecvResult out;

    // Single recvmsg large enough for header + body. Matches the Rust
    // side, which assumes the sender always fuses header and body into
    // a single sendmsg — our send_frame does exactly that.
    std::vector<uint8_t> buf(kMaxFrameBytes + 4);
    alignas(cmsghdr) uint8_t cbuf[CMSG_SPACE(sizeof(int) * kMaxFdsPerMsg)] {};

    iovec iov {};
    iov.iov_base = buf.data();
    iov.iov_len  = buf.size();

    msghdr msg {};
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = cbuf;
    msg.msg_controllen = sizeof(cbuf);

    ssize_t n;
    while (true) {
        n = ::recvmsg(sock, &msg, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw errno_error("recvmsg");
        }
        break;
    }
    if (n == 0) throw std::runtime_error { "recv_frame: peer closed" };
    if (n < 4) throw std::runtime_error { "recv_frame: short header" };

    uint32_t body_len_be = 0;
    std::memcpy(&body_len_be, buf.data(), 4);
    const uint32_t body_len = be32toh(body_len_be);
    if (body_len > kMaxFrameBytes) {
        throw std::length_error { "recv_frame: body exceeds kMaxFrameBytes" };
    }
    if (4 + body_len > static_cast<size_t>(n)) {
        throw std::runtime_error { "recv_frame: truncated frame" };
    }

    // Harvest any SCM_RIGHTS fds.
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
         cmsg         = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            const size_t data_len = cmsg->cmsg_len - CMSG_LEN(0);
            const size_t n_fds    = data_len / sizeof(int);
            const int*   raw_fds  = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
            for (size_t i = 0; i < n_fds; i++) out.fds.push_back(raw_fds[i]);
        }
    }

    out.body = nlohmann::json::parse(buf.data() + 4, buf.data() + 4 + body_len);
    return out;
}

} // namespace waywallen::ipc
