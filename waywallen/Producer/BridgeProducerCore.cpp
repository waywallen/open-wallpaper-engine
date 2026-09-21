module;

#include <cerrno>
#include <unistd.h>

module waywallen.bridge_producer_core;

import rstd;

import vvk;
import waywallen.bridge;
import waywallen.bridge_session;

using namespace rstd::prelude;
using rstd::io::eprint;
using rstd::sync::Arc;
using rstd::sync::atomic::Ordering;

namespace ww_wescene
{

namespace
{

// fourcc → VkFormat for the producer-side image view.
//
// DRM fourcc names channels MSB→LSB in a little-endian uint32; VkFormat
// names channels in memory byte order. Hence the byte-for-byte equiv:
//   DRM_FORMAT_ABGR8888  (mem: R G B A) ↔ VK_FORMAT_R8G8B8A8_UNORM
//   DRM_FORMAT_ARGB8888  (mem: B G R A) ↔ VK_FORMAT_B8G8R8A8_UNORM
//
// X variants share the byte layout of their A counterpart — the 4th
// byte is "ignored" rather than meaningful alpha. The producer's blit
// preserves logical RGBA channels regardless, and a wallpaper renders
// alpha=1.0 anyway, so X is folded onto the A variant.
//
// Mirrors waywallen/bridge/src/pool_vulkan.c::s_vk_fourcc_table and
// waywallen-display/src/backend_vulkan.c::s_vk_fourcc_table — the
// three sides must agree on the same advertised set since the
// daemon negotiates by exact fourcc match.
VkFormat fourcc_to_vk_format(uint32_t fourcc) {
    switch (fourcc) {
    case WW_DRM_FORMAT_ABGR8888: return VK_FORMAT_R8G8B8A8_UNORM;
    case WW_DRM_FORMAT_XBGR8888: return VK_FORMAT_R8G8B8A8_UNORM;
    case WW_DRM_FORMAT_ARGB8888: return VK_FORMAT_B8G8R8A8_UNORM;
    case WW_DRM_FORMAT_XRGB8888: return VK_FORMAT_B8G8R8A8_UNORM;
    case WW_DRM_FORMAT_RGBA8888: return VK_FORMAT_R8G8B8A8_UNORM;
    case WW_DRM_FORMAT_BGRA8888: return VK_FORMAT_B8G8R8A8_UNORM;
    case WW_DRM_FORMAT_RGBX8888: return VK_FORMAT_R8G8B8A8_UNORM;
    case WW_DRM_FORMAT_BGRX8888: return VK_FORMAT_B8G8R8A8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}

} // namespace

BridgeProducerCore::BridgeProducerCore(Arc<BridgeSession> session)
    : m_session(rstd::move(session)) {}

BridgeProducerCore::~BridgeProducerCore() = default;

void BridgeProducerCore::setOnFirstNegotiated(FirstNegotiatedCallback cb) {
    Option<FirstNegotiatedCallback> previous;
    {
        auto callbacks = m_callbacks.lock().unwrap();
        if (callbacks->first_done) return;
        previous         = callbacks->first.take();
        callbacks->first = Some(rstd::move(cb));
    }
}

void BridgeProducerCore::setOnReadyChanged(Option<ReadyChangedCallback> cb) {
    Option<ReadyChangedCallback> previous;
    {
        auto callbacks   = m_callbacks.lock().unwrap();
        previous         = callbacks->ready.take();
        callbacks->ready = rstd::move(cb);
    }
}

void BridgeProducerCore::markSessionLost() {
    m_session_lost     = true;
    m_slot_count       = 0;
    m_have_pending     = false;
    m_pending_identity = {};
}

void BridgeProducerCore::queueDirective(const ww_pool_directive_t& directive) {
    {
        auto pending = m_pending_directive.lock().unwrap();
        *pending     = directive;
        m_pending_valid.store(true, Ordering::Release);
    }
}

bool BridgeProducerCore::requestFrame() {
    const uint64_t previous = m_frame_request_revision.fetch_add(1, Ordering::AcqRel);
    return previous == m_served_frame_request_revision.load(Ordering::Acquire);
}

int BridgeProducerCore::cancelRepublishWait(void* userdata) {
    auto& core = *static_cast<BridgeProducerCore*>(userdata);
    return core.m_cancel_frame_wait.load(Ordering::Acquire) ||
                   core.m_pending_valid.load(Ordering::Acquire)
               ? 1
               : 0;
}

bool BridgeProducerCore::republishRequestedFrame(bool wait_for_release) {
    const uint64_t revision = m_frame_request_revision.load(Ordering::Acquire);
    if (revision == m_served_frame_request_revision.load(Ordering::Acquire)) return false;
    if (m_session_lost || m_slot_count == 0 || m_have_pending) return false;

    ww_pool_republish_result_t result {};
    const int rc = wait_for_release
                       ? m_session->waitRepublishLatest(cancelRepublishWait, this, result)
                       : m_session->tryRepublishLatest(result);
    if (rc != 0) {
        eprint("BridgeProducerCore: republish_latest rc={}\n", rc);
        if (rc == -EPIPE) markSessionLost();
        return false;
    }
    switch (result.status) {
    case WW_POOL_REPUBLISH_PUBLISHED:
        m_served_frame_request_revision.store(revision, Ordering::Release);
        return true;
    case WW_POOL_REPUBLISH_NO_CONTENT:
    case WW_POOL_REPUBLISH_BUSY:
    case WW_POOL_REPUBLISH_CANCELLED: return false;
    case WW_POOL_REPUBLISH_SESSION_LOST: markSessionLost(); return false;
    case WW_POOL_REPUBLISH_ERROR:
        eprint("BridgeProducerCore: republish_latest error={}\n", result.error_code);
        return false;
    }
    return false;
}

void BridgeProducerCore::drainPendingDirective() {
    if (m_session_lost) {
        m_pending_valid.store(false, Ordering::Release);
        return;
    }
    if (! m_pending_valid.load(Ordering::Acquire)) return;

    ww_pool_directive_t d {};
    {
        auto pending = m_pending_directive.lock().unwrap();
        d            = *pending;
        m_pending_valid.store(false, Ordering::Release);
    }

    int rc = applyDirective(d);
    if (rc == -EPIPE) {
        markSessionLost();
        return;
    }
    if (rc == -EBUSY) {
        auto pending = m_pending_directive.lock().unwrap();
        if (! m_pending_valid.load(Ordering::Acquire)) {
            *pending = d;
            m_pending_valid.store(true, Ordering::Release);
        }
        return;
    }
    if (rc != 0) return;

    // Snapshot callbacks under the lock and invoke unlocked so the
    // handler can re-enter the core (read width/height) without
    // blocking — only the producer thread ever invokes them anyway.
    Option<FirstNegotiatedCallback> first_cb;
    Option<ReadyChangedCallback>    ready_cb;
    {
        auto callbacks = m_callbacks.lock().unwrap();
        if (! callbacks->first_done && callbacks->first) {
            first_cb              = callbacks->first.take();
            callbacks->first_done = true;
        }
        ready_cb = callbacks->ready.clone();
    }
    if (first_cb) (*first_cb)->call_once();
    if (ready_cb) {
        BridgeReadyEvent e {
            .ready  = true,
            .width  = m_width,
            .height = m_height,
            .format = m_export_format,
        };
        (*ready_cb)->operator()(e);
    }
}

int BridgeProducerCore::applyDirective(const ww_pool_directive_t& directive) {
    if (m_have_pending) return -EBUSY;

    VkFormat picked = fourcc_to_vk_format(directive.format.fourcc);
    if (picked == VK_FORMAT_UNDEFINED) {
        eprint("BridgeProducerCore: fourcc 0x{:08x} has no VkFormat mapping\n",
               directive.format.fourcc);
        waywallen_bind_failure_t failure {
            .format  = directive.format,
            .kind    = WAYWALLEN_BUFFER_ALLOCATION_FAILURE_KIND_UNSUPPORTED,
            .message = const_cast<char*>("fourcc unsupported by producer"),
        };
        m_session->sendBindFailed(failure);
        return -EINVAL;
    }
    if (directive.count == 0 || directive.count > kMaxSlots) {
        eprint("BridgeProducerCore: invalid slot count {} (cap={})\n", directive.count, kMaxSlots);
        return 1;
    }

    // Bridge's apply_directive tears down old slots and allocates new
    // ones. Producer-side has nothing to wind down — the blit dst is
    // the bridge slot directly, no cached views or intermediates.
    m_slot_count       = 0;
    m_have_pending     = false;
    m_pending_identity = {};
    // Don't publish new geometry yet — wait for apply_directive to
    // succeed. If we publish early and apply fails, the producer sees
    // format()/ready() reporting state that has no slots behind it.

    int rc = m_session->applyDirective(directive);
    if (rc < 0) {
        eprint("BridgeProducerCore: apply_directive dry-run failed: {}\n", rc);
        return rc;
    }
    if (rc > 0) {
        eprint("BridgeProducerCore: apply_directive system error: {}\n", rc);
        return rc;
    }

    // The directive carries no extent (NegotiateBuffers wire dropped
    // extent_w/h since the renderer is the authority on render extent).
    // Read the actual slot dimensions back from the bridge — they were
    // sized from the `probe_width/height` we passed into advertise_caps.
    uint32_t width  = 0;
    uint32_t height = 0;
    if (int srx = m_session->getExtent(width, height); srx != 0) {
        eprint("BridgeProducerCore: get_extent post-apply failed: {}\n", srx);
        return srx;
    }

    // Publish atomically only after apply_directive succeeded.
    m_width         = width;
    m_height        = height;
    m_fourcc        = directive.format.fourcc;
    m_export_format = picked;
    m_slot_count    = directive.count;
    return 0;
}

BridgeSlotAcquireResult BridgeProducerCore::acquireSlot() {
    BridgeSlotAcquireResult result;
    if (m_session_lost) {
        result.status     = BridgeSlotAcquireStatus::SessionLost;
        result.error_code = -EPIPE;
        return result;
    }
    if (m_slot_count == 0) return result;
    if (m_have_pending) {
        result.status     = BridgeSlotAcquireStatus::Error;
        result.error_code = -EBUSY;
        return result;
    }

    ww_pool_slot_acquire_result_t acquired {};
    if (int rc = m_session->tryAcquireAnyForRender(acquired); rc != 0) {
        if (rc == -EPIPE) markSessionLost();
        result.status =
            rc == -EPIPE ? BridgeSlotAcquireStatus::SessionLost : BridgeSlotAcquireStatus::Error;
        result.error_code = rc;
        return result;
    }

    switch (acquired.status) {
    case WW_POOL_SLOT_ACQUIRE_READY_UNUSED:
        result.status = BridgeSlotAcquireStatus::ReadyUnused;
        break;
    case WW_POOL_SLOT_ACQUIRE_READY_RELEASED:
        result.status = BridgeSlotAcquireStatus::ReadyReleased;
        break;
    case WW_POOL_SLOT_ACQUIRE_BUSY: result.status = BridgeSlotAcquireStatus::Busy; return result;
    case WW_POOL_SLOT_ACQUIRE_SESSION_LOST:
        markSessionLost();
        result.status     = BridgeSlotAcquireStatus::SessionLost;
        result.error_code = acquired.error_code;
        return result;
    case WW_POOL_SLOT_ACQUIRE_ERROR:
    default:
        result.status     = BridgeSlotAcquireStatus::Error;
        result.error_code = acquired.error_code;
        return result;
    }

    result.identity = BridgeSlotIdentity {
        .bind_generation        = acquired.identity.bind_generation,
        .slot_index             = acquired.identity.slot_index,
        .previous_release_point = acquired.identity.previous_release_point,
        .acquire_serial         = acquired.identity.acquire_serial,
    };
    result.image  = static_cast<VkImage>(acquired.slot.vk_image);
    result.width  = acquired.slot.width;
    result.height = acquired.slot.height;
    if (! result.acquired()) {
        (void)m_session->abortAcquiredSlot(acquired.identity);
        result.status     = BridgeSlotAcquireStatus::Error;
        result.error_code = -EINVAL;
        return result;
    }

    m_pending_identity = result.identity;
    m_have_pending     = true;
    return result;
}

bool BridgeProducerCore::acquireSlot(VkImage* out_image, uint32_t* out_width,
                                     uint32_t* out_height) {
    auto result = acquireSlot();
    if (! result.acquired()) return false;

    if (out_image) *out_image = result.image;
    if (out_width) *out_width = result.width;
    if (out_height) *out_height = result.height;
    return true;
}

BridgeSlotCompletionResult BridgeProducerCore::submitSlot(const BridgeSlotIdentity& identity,
                                                          int producer_sync_fd) {
    const uint64_t frame_request_revision = m_frame_request_revision.load(Ordering::Acquire);
    if (! m_have_pending) {
        if (producer_sync_fd >= 0) ::close(producer_sync_fd);
        return BridgeSlotCompletionResult {
            .status   = BridgeSlotCompletionStatus::NotPending,
            .identity = identity,
        };
    }
    if (identity != m_pending_identity) {
        if (producer_sync_fd >= 0) ::close(producer_sync_fd);
        return BridgeSlotCompletionResult {
            .status   = BridgeSlotCompletionStatus::StaleIdentity,
            .identity = identity,
        };
    }

    ww_pool_slot_identity_t bridge_identity {
        .bind_generation        = identity.bind_generation,
        .slot_index             = identity.slot_index,
        .previous_release_point = identity.previous_release_point,
        .acquire_serial         = identity.acquire_serial,
    };

    ww_pool_slot_submit_result_t submitted {};
    int rc = m_session->submitAcquiredSlot(bridge_identity, producer_sync_fd, submitted);
    if (rc != 0) {
        if (rc == -EPIPE) {
            markSessionLost();
            return BridgeSlotCompletionResult {
                .status     = BridgeSlotCompletionStatus::SessionLost,
                .identity   = identity,
                .error_code = rc,
            };
        }
        eprint("BridgeProducerCore: submit_slot({}) rc={}\n", identity.slot_index, rc);
        // Bridge contract: bridge always closes the fd. We don't dup-close.
        (void)m_session->abortAcquiredSlot(bridge_identity);
        m_have_pending     = false;
        m_pending_identity = {};
        return BridgeSlotCompletionResult {
            .status     = BridgeSlotCompletionStatus::ProtocolError,
            .identity   = identity,
            .error_code = rc,
        };
    }
    if (submitted.status == WW_POOL_SLOT_SUBMIT_SESSION_LOST) {
        markSessionLost();
        return BridgeSlotCompletionResult {
            .status     = BridgeSlotCompletionStatus::SessionLost,
            .identity   = identity,
            .error_code = submitted.error_code,
        };
    }
    if (submitted.status != WW_POOL_SLOT_SUBMIT_SUBMITTED) {
        (void)m_session->abortAcquiredSlot(bridge_identity);
        m_have_pending     = false;
        m_pending_identity = {};
        return BridgeSlotCompletionResult {
            .status     = BridgeSlotCompletionStatus::ProtocolError,
            .identity   = identity,
            .error_code = submitted.error_code,
        };
    }
    m_have_pending     = false;
    m_pending_identity = {};
    m_served_frame_request_revision.store(frame_request_revision, Ordering::Release);
    return BridgeSlotCompletionResult {
        .status   = BridgeSlotCompletionStatus::Submitted,
        .identity = identity,
    };
}

BridgeSlotCompletionResult BridgeProducerCore::abortSlot(const BridgeSlotIdentity& identity) {
    if (! m_have_pending) {
        return BridgeSlotCompletionResult {
            .status   = BridgeSlotCompletionStatus::NotPending,
            .identity = identity,
        };
    }
    if (identity != m_pending_identity) {
        return BridgeSlotCompletionResult {
            .status   = BridgeSlotCompletionStatus::StaleIdentity,
            .identity = identity,
        };
    }

    ww_pool_slot_identity_t bridge_identity {
        .bind_generation        = identity.bind_generation,
        .slot_index             = identity.slot_index,
        .previous_release_point = identity.previous_release_point,
        .acquire_serial         = identity.acquire_serial,
    };
    int rc = m_session->abortAcquiredSlot(bridge_identity);
    if (rc != 0) {
        if (rc == -EPIPE) markSessionLost();
        return BridgeSlotCompletionResult {
            .status     = rc == -EPIPE    ? BridgeSlotCompletionStatus::SessionLost
                          : rc == -ESTALE ? BridgeSlotCompletionStatus::StaleIdentity
                                          : BridgeSlotCompletionStatus::ProtocolError,
            .identity   = identity,
            .error_code = rc,
        };
    }

    m_have_pending     = false;
    m_pending_identity = {};
    return BridgeSlotCompletionResult {
        .status   = BridgeSlotCompletionStatus::Aborted,
        .identity = identity,
    };
}

void BridgeProducerCore::submitSlot(int producer_sync_fd) {
    if (! m_have_pending) {
        if (producer_sync_fd >= 0) ::close(producer_sync_fd);
        return;
    }
    (void)submitSlot(m_pending_identity, producer_sync_fd);
}

} // namespace ww_wescene
