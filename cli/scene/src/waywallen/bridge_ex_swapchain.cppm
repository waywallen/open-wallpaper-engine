module;
#include <rstd/enum.hpp>

export module owe.cli.scene.waywallen.bridge_ex_swapchain;

import rstd;
import wescene.vulkan;
import owe.cli.waywallen.bridge_producer_core;

using namespace rstd::prelude;
using rstd::sync::Arc;
using rstd::sync::atomic::Atomic;
using rstd::sync::atomic::Ordering;

export namespace ww_wescene
{

class BridgeSurfaceError {
    RSTD_ENUM(BridgeSurfaceError, (Acquire, (BridgeSlotAcquireStatus status; rstd::int32_t code;)),
              (Submit, (BridgeSlotCompletionStatus status; rstd::int32_t code;)),
              (Abort, (BridgeSlotCompletionStatus status; rstd::int32_t code;)))
};

class BridgeExSwapchain : public owe::ExSwapchain {
public:
    static constexpr rstd::uint32_t kMaxSlots = BridgeProducerCore::kMaxSlots;

    explicit BridgeExSwapchain(Arc<BridgeSession> session);
    ~BridgeExSwapchain() override;

    void queueDirective(const ww_pool_directive_t& directive) { m_core.queueDirective(directive); }
    void setRenderTargetCapabilities(const waywallen_render_target_capabilities_t& caps) {
        m_core.setRenderTargetCapabilities(caps);
    }
    void queueRenderTarget(const waywallen_render_target_config_t& config) {
        m_core.queueRenderTarget(config);
    }
    bool requestFrame() {
        const bool wake = m_core.requestFrame();
        if (wake) m_frame_request_wake.store(true, Ordering::Release);
        return wake;
    }
    void cancelFrameWait() { m_core.cancelFrameWait(); }
    bool hasPendingDirective() const { return m_core.hasPendingDirective(); }

    void setOnFirstNegotiated(FirstNegotiatedCallback cb) {
        m_core.setOnFirstNegotiated(rstd::move(cb));
    }

    template<typename Callback>
        requires requires(Callback cb) { cb(); }
    void setOnFirstNegotiated(Callback cb) {
        m_core.setOnFirstNegotiated(rstd::move(cb));
    }

    void poll() override;

    owe::FrameSurfaceAcquireResult acquireRenderTarget() override;

    unsigned width() const override { return m_core.width(); }
    unsigned height() const override { return m_core.height(); }
    VkFormat format() const override { return m_core.format(); }

    bool ready() const override { return m_core.ready(); }

    void setOnReadyChanged(rstd::Option<owe::ExSwapchainReadyCallback> cb) override {
        if (! cb) {
            m_core.setOnReadyChanged(rstd::None());
            return;
        }
        m_core.setOnReadyChanged([cb = rstd::move(cb)](const BridgeReadyEvent& e) {
            (*cb)->operator()(owe::ExSwapchainReadyEvent {
                .ready  = e.ready,
                .width  = e.width,
                .height = e.height,
                .format = e.format,
            });
        });
    }

private:
    owe::FrameSurfaceCompletionResult CompleteRendered(owe::FrameSurfaceIdentity identity,
                                                       int producer_sync_fd) override;
    owe::FrameSurfaceCompletionResult
    AbortRenderTarget(owe::FrameSurfaceIdentity identity) override;

    BridgeProducerCore               m_core;
    rstd::Option<BridgeSlotIdentity> m_pending_identity;
    Atomic<bool>                     m_frame_request_wake { false };
    bool                             m_skip_acquire_in_poll { false };
};

} // namespace ww_wescene

export namespace rstd
{
template<>
struct Impl<fmt::Display, ww_wescene::BridgeSurfaceError>
    : ImplBase<ww_wescene::BridgeSurfaceError> {
    auto fmt(fmt::Formatter& f) const -> bool {
        const auto& e = this->self();
        if (e.is_Acquire()) {
            const auto& v = e.as_Acquire();
            return f.write_fmt(
                fmt::Arguments::make("bridge acquire status={} code={}", int(v.status), v.code));
        }
        if (e.is_Submit()) {
            const auto& v = e.as_Submit();
            return f.write_fmt(
                fmt::Arguments::make("bridge submit status={} code={}", int(v.status), v.code));
        }
        const auto& v = e.as_Abort();
        return f.write_fmt(
            fmt::Arguments::make("bridge abort status={} code={}", int(v.status), v.code));
    }
};
template<>
struct Impl<fmt::Debug, ww_wescene::BridgeSurfaceError> : ImplBase<ww_wescene::BridgeSurfaceError> {
    auto fmt(fmt::Formatter& f) const -> bool { return as<fmt::Display>(this->self()).fmt(f); }
};
template<>
struct Impl<error::Error, ww_wescene::BridgeSurfaceError>
    : ImplBase<ww_wescene::BridgeSurfaceError> {
    auto source() const noexcept -> Option<error::ErrorRef> { return None(); }
};
} // namespace rstd
