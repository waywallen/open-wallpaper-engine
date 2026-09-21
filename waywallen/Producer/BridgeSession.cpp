module;

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

module waywallen.bridge_session;

import rstd;
import waywallen.bridge;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::collections::BTreeSet;
using rstd::ffi::CStr;
using rstd::ffi::CString;
using rstd::sync::Arc;

namespace ww_wescene
{

Option<Arc<BridgeSession>> BridgeSession::Adopt(ww_pool_t* pool, int control_socket) {
    if (pool == nullptr || control_socket < 0) return {};
    int send_socket = ::fcntl(control_socket, F_DUPFD_CLOEXEC, 0);
    if (send_socket < 0) return {};
    return Some(Arc<BridgeSession>::make(Adopted {}, pool, send_socket));
}

BridgeSession::~BridgeSession() {
    if (m_pool != nullptr) ww_bridge_pool_destroy(m_pool);
    if (m_send_socket >= 0) ::close(m_send_socket);
}

int BridgeSession::advertiseCaps(uint32_t width, uint32_t height, uint32_t mem_hints) {
    auto lock = m_send_mutex.lock().unwrap();
    return ww_bridge_pool_advertise_caps(m_pool, m_send_socket, width, height, mem_hints);
}

int BridgeSession::applyDirective(const ww_pool_directive_t& directive) {
    auto lock = m_send_mutex.lock().unwrap();
    return ww_bridge_pool_apply_directive(m_pool, m_send_socket, &directive);
}

int BridgeSession::getExtent(uint32_t& out_width, uint32_t& out_height) {
    return ww_bridge_pool_get_extent(m_pool, &out_width, &out_height);
}

int BridgeSession::tryAcquireAnyForRender(ww_pool_slot_acquire_result_t& out_result) {
    return ww_bridge_pool_try_acquire_any_for_render(m_pool, &out_result);
}

int BridgeSession::submitAcquiredSlot(const ww_pool_slot_identity_t& identity, int producer_sync_fd,
                                      ww_pool_slot_submit_result_t& out_result) {
    auto lock = m_send_mutex.lock().unwrap();
    return ww_bridge_pool_submit_acquired_slot(
        m_pool, m_send_socket, &identity, producer_sync_fd, &out_result);
}

int BridgeSession::tryRepublishLatest(ww_pool_republish_result_t& out_result) {
    auto lock = m_send_mutex.lock().unwrap();
    return ww_bridge_pool_try_republish_latest(m_pool, m_send_socket, &out_result);
}

int BridgeSession::waitRepublishLatest(ww_pool_cancel_fn cancel, void* userdata,
                                       ww_pool_republish_result_t& out_result) {
    auto lock = m_send_mutex.lock().unwrap();
    return ww_bridge_pool_wait_republish_latest(
        m_pool, m_send_socket, cancel, userdata, &out_result);
}

int BridgeSession::abortAcquiredSlot(const ww_pool_slot_identity_t& identity) {
    return ww_bridge_pool_abort_acquired_slot(m_pool, &identity);
}

int BridgeSession::sendBindFailed(const waywallen_bind_failure_t& failure) {
    auto lock = m_send_mutex.lock().unwrap();
    return ww_bridge_send_bind_failed(m_send_socket, &failure);
}

int BridgeSession::sendClearColor(float r, float g, float b, float a) {
    auto lock = m_send_mutex.lock().unwrap();
    return ww_bridge_send_report_state_clear_color(m_send_socket, r, g, b, a);
}

int BridgeSession::setEventSubscriptions(uint64_t revision, const BTreeSet<String>& kinds) {
    Vec<CString> owners;
    Vec<char*>   raw;
    owners.reserve(kinds.len());
    raw.reserve(kinds.len());
    for (auto kind : kinds.iter()) {
        auto value = CString::make(Vec<u8>::from(kind->as_str().as_bytes()));
        if (value.is_err()) return -EINVAL;
        owners.push(value.unwrap());
        raw.push(const_cast<char*>(owners.last().unwrap()->as_ptr()));
    }
    const waywallen_event_subscription_t subscription {
        .revision = revision,
        .kinds = {
            .count = static_cast<uint32_t>(raw.len().to_primitive()),
            .data = raw.is_empty() ? nullptr : raw.data(),
        },
    };
    auto lock = m_send_mutex.lock().unwrap();
    return ww_bridge_set_event_subscriptions(m_send_socket, &subscription);
}

bool BridgeSubscriptionController::replace(BTreeSet<String> kinds) {
    auto state = m_state.lock().unwrap();
    bool same  = state->desired.len() == kinds.len() && kinds.iter().all([&](auto kind) {
        return state->desired.contains(kind->as_str());
    });
    if (! same) {
        state->desired = rstd::move(kinds);
        state->dirty   = true;
    }
    return ! state->dirty || sendLocked(*state);
}

bool BridgeSubscriptionController::set(ref<str> kind, bool enabled) {
    auto state   = m_state.lock().unwrap();
    bool present = state->desired.contains(kind);
    if (enabled == present) return ! state->dirty || sendLocked(*state);
    if (enabled)
        state->desired.insert(String::make(kind));
    else
        state->desired.remove(kind);
    state->dirty = true;
    return sendLocked(*state);
}

bool BridgeSubscriptionController::sendLocked(State& state) {
    const uint64_t revision = state.next_revision++;
    if (m_session->setEventSubscriptions(revision, state.desired) != 0) return false;
    state.sent_revision = revision;
    state.dirty         = false;
    return true;
}

void BridgeSubscriptionController::applied(const waywallen_event_subscription_result_t& event) {
    auto state = m_state.lock().unwrap();
    if (event.status != WAYWALLEN_EVENT_SUBSCRIPTION_STATUS_APPLIED ||
        event.revision < state->applied_revision || event.revision > state->sent_revision)
        return;
    state->applied_revision = event.revision;
    state->applied.clear();
    for (uint32_t index = 0; index < event.kinds.count; ++index) {
        if (event.kinds.data[index])
            state->applied.insert(
                String::make(CStr::from_ptr(event.kinds.data[index]).to_str().unwrap()));
    }
}

bool BridgeSubscriptionController::acceptsAudio(uint64_t revision) const {
    auto state = m_state.lock().unwrap();
    return revision == state->applied_revision && state->applied.contains("audio"_str);
}

} // namespace ww_wescene
