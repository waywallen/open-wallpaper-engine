export module waywallen.bridge_session;

import rstd;
export import waywallen.bridge;

using namespace rstd::prelude;
using rstd::collections::BTreeSet;
using rstd::sync::Arc;
using rstd::sync::Mutex;

export namespace ww_wescene
{

class BridgeSession final {
    struct Adopted {};

public:
    BridgeSession(Adopted, ww_pool_t* pool, int send_socket)
        : m_pool(pool), m_send_socket(send_socket) {}
    static Option<Arc<BridgeSession>> Adopt(ww_pool_t* pool, int control_socket);

    ~BridgeSession();

    BridgeSession(const BridgeSession&)            = delete;
    BridgeSession& operator=(const BridgeSession&) = delete;

    bool valid() const noexcept { return m_pool != nullptr && m_send_socket >= 0; }

    int advertiseCaps(rstd::uint32_t width, rstd::uint32_t height, rstd::uint32_t mem_hints);
    int applyDirective(const ww_pool_directive_t& directive);
    int getExtent(rstd::uint32_t& out_width, rstd::uint32_t& out_height);
    int tryAcquireAnyForRender(ww_pool_slot_acquire_result_t& out_result);
    int submitAcquiredSlot(const ww_pool_slot_identity_t& identity, int producer_sync_fd,
                           ww_pool_slot_submit_result_t& out_result);
    int tryRepublishLatest(ww_pool_republish_result_t& out_result);
    int waitRepublishLatest(ww_pool_cancel_fn cancel, void* userdata,
                            ww_pool_republish_result_t& out_result);
    int abortAcquiredSlot(const ww_pool_slot_identity_t& identity);
    int sendBindFailed(const waywallen_bind_failure_t& failure);
    int sendClearColor(float r, float g, float b, float a);
    int setEventSubscriptions(rstd::uint64_t revision, const BTreeSet<String>& kinds);

private:
    ww_pool_t*         m_pool { nullptr };
    int                m_send_socket { -1 };
    Mutex<rstd::empty> m_send_mutex;
};

class BridgeSubscriptionController final {
public:
    explicit BridgeSubscriptionController(Arc<BridgeSession> session)
        : m_session(rstd::move(session)) {}

    bool replace(BTreeSet<String> kinds);
    bool set(ref<str> kind, bool enabled);
    void applied(const waywallen_event_subscription_result_t& event);
    bool acceptsAudio(rstd::uint64_t revision) const;

private:
    struct State {
        BTreeSet<String> desired { BTreeSet<String>::make() };
        rstd::uint64_t   next_revision { 1 };
        rstd::uint64_t   sent_revision { 0 };
        rstd::uint64_t   applied_revision { 0 };
        BTreeSet<String> applied { BTreeSet<String>::make() };
        bool             dirty { false };
    };
    bool sendLocked(State& state);

    Arc<BridgeSession>   m_session;
    mutable Mutex<State> m_state;
};

} // namespace ww_wescene
