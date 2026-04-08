#pragma once
#include <array>
#include <atomic>
#include "Core/NoCopyMove.hpp"

namespace wallpaper
{

template<typename T>
class TripleSwapchain : NoCopy, NoMove {
public:
    virtual ~TripleSwapchain() = default;

    T* eatFrame() {
        if (! dirty().exchange(false)) return nullptr;
        presented() = ready().exchange(presented());
        return presented();
    }
    void renderFrame() {
        inprogress() = ready().exchange(inprogress());
        dirty().exchange(true);
    }
    T* getInprogress() { return inprogress(); }

    // Snapshot the three slot pointers. At construction time each atomic
    // points at a distinct backing slot, so this lets a host program
    // enumerate all three handles up front (e.g. to send their FDs in a
    // single IPC `BindBuffers` message) without waiting for frames to
    // cycle through the ring. The pointers returned are stable for the
    // lifetime of the swapchain even though which one is "ready" at any
    // moment shifts around.
    std::array<T*, 3> snapshot_all_slots() {
        return { presented().load(), ready().load(), inprogress().load() };
    }

    virtual uint width() const  = 0;
    virtual uint height() const = 0;

protected:
    TripleSwapchain() = default;

    virtual std::atomic<T*>& presented()  = 0;
    virtual std::atomic<T*>& ready()      = 0;
    virtual std::atomic<T*>& inprogress() = 0;

private:
    std::atomic<bool>& dirty() { return m_dirty; };
    std::atomic<bool>  m_dirty { false };
};

} // namespace wallpaper