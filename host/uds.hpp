// SPDX-License-Identifier: MIT
//
// Blocking Unix-domain-socket helpers for the waywallen-renderer host
// program. Hand-mirrored against waywallen/src/ipc/uds.rs — the frame
// format must stay in lockstep:
//
//   [u32 big-endian length] [JSON body]        + optional SCM_RIGHTS fds
//
// Length is of the JSON body only, excludes itself.

#pragma once

#include "proto.hpp"
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace waywallen::ipc {

// 1 MiB body cap, matching the Rust side.
inline constexpr size_t kMaxFrameBytes = 1u << 20;
// Max fds per message. Tracks Rust's MAX_FDS_PER_MSG.
inline constexpr size_t kMaxFdsPerMsg = 8;

// Connect a UDS to `path` (blocking). Throws std::system_error on failure.
// Returns the new socket fd (owned by the caller).
int connect_uds(const std::string& path);

// Send a JSON body as a single framed message with optional FDs attached
// as SCM_RIGHTS ancillary data.
void send_frame(int sock, const nlohmann::json& body, std::span<const int> fds = {});

// Receive one framed message plus any ancillary FDs. The returned fd
// values are caller-owned (must be close(2)'d when done).
struct RecvResult {
    nlohmann::json   body;
    std::vector<int> fds;
};
RecvResult recv_frame(int sock);

// Convenience: send a typed message. Wraps to_json + send_frame.
template<typename Msg>
void send_typed(int sock, const Msg& msg, std::span<const int> fds = {}) {
    send_frame(sock, to_json(msg), fds);
}

} // namespace waywallen::ipc
