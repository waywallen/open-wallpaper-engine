module;

#include "effolkronium/random.hpp"

export module wescene.core;
import rstd;

using namespace rstd::prelude;

// NoCopyMove (global scope, matches the original NoCopyMove.hpp)
export struct NoCopy {
protected:
    NoCopy()  = default;
    ~NoCopy() = default;

    NoCopy(const NoCopy&)            = delete;
    NoCopy& operator=(const NoCopy&) = delete;
};

export struct NoMove {
protected:
    NoMove()  = default;
    ~NoMove() = default;

    NoMove(NoMove&&)            = delete;
    NoMove& operator=(NoMove&&) = delete;
};

export namespace owe
{

using rstd::i16;
using rstd::i32;
using rstd::i64;
using rstd::i8;
using rstd::isize;
using rstd::u16;
using rstd::u32;
using rstd::u64;
using rstd::u8;
using rstd::usize;

using idx = isize;

inline isize Ptr2Int(void* p) noexcept { return isize(reinterpret_cast<rstd::intptr_t>(p)); }

template<typename T>
class spanone {
public:
    using value_type = T;
    using size_type  = usize;
    using reference  = T&;
    using pointer    = T*;

    constexpr spanone(reference value) noexcept: ptr { &value } {}
    constexpr pointer   data() const noexcept { return ptr; }
    constexpr size_type size() const noexcept { return usize(1); }
    constexpr reference operator[](usize index) const noexcept { return ptr[index]; }
    constexpr pointer   begin() const noexcept { return ptr; }
    constexpr pointer   end() const noexcept { return ptr + 1; }
    constexpr pointer   cbegin() const noexcept { return ptr; }
    constexpr pointer   cend() const noexcept { return ptr + 1; }

private:
    pointer ptr;
};

// Visitors
namespace visitor
{

template<class... Ts>
struct overload : Ts... {
    using Ts::operator()...;
};
template<class... Ts>
overload(Ts...) -> overload<Ts...>;

struct EqualVisitor {
    using result_type = bool;

    template<typename T, typename U>
    bool operator()(const T&, const U&) const {
        return false;
    }

    template<typename T>
    bool operator()(const T& v1, const T& v2) const {
        return v1 == v2;
    }
};

} // namespace visitor

// Random
using Random = effolkronium::random_thread_local;

} // namespace owe
