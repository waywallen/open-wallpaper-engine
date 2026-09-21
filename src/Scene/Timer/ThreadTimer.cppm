export module wescene.timer:thread_timer;
import rstd;

using namespace rstd::prelude;
using rstd::sync::Condvar;
using rstd::sync::Mutex;
using rstd::thread::JoinHandle;
using rstd::time::Duration;

export namespace owe
{
class ThreadTimer {
public:
    using Callback = Box<dyn<FnMut<void()>>>;
    explicit ThreadTimer(Callback callback);
    ~ThreadTimer();
    void Start();
    void Stop();
    bool Running() const;
    void SetInterval(Duration);

private:
    struct State {
        bool     running {};
        Duration interval { Duration::from_millis(u64(1)) };
    };
    Callback                 m_callback;
    Mutex<empty>             m_op_mutex { empty {} };
    Option<JoinHandle<void>> m_timer_thread;
    Mutex<State>             m_state { State {} };
    Condvar                  m_condition;
};
} // namespace owe
