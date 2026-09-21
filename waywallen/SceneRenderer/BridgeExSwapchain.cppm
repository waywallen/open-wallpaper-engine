export module waywallen.bridge_ex_swapchain;

import rstd;
import wescene.vulkan;
import waywallen.bridge_producer_core;

using rstd::sync::Arc;
using rstd::sync::atomic::Atomic;
using rstd::sync::atomic::Ordering;

export namespace ww_wescene
{

class BridgeExSwapchain : public owe::ExSwapchain {
public:
    static constexpr rstd::uint32_t kMaxSlots = BridgeProducerCore::kMaxSlots;

    explicit BridgeExSwapchain(Arc<BridgeSession> session);
    ~BridgeExSwapchain() override;

    void queueDirective(const ww_pool_directive_t& directive) { m_core.queueDirective(directive); }
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
