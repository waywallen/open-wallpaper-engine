module;

export module viewer.web:presenter;

import rstd;
import weweb;
import viewer.glfw_vulkan;

using viewer::glfw::GLFWwindow;

using namespace rstd::prelude;

export namespace weweb
{

// Abstract present backend: takes CEF DMA-BUF frames and shows them in a
// GLFW window. Two implementations: VulkanBlitter (default) and
// EglPresenter (selected via the WebViewer --presenter flag).
class Presenter;

struct PresenterObject {
    using Trait                  = PresenterObject;
    static constexpr bool direct = false;
    template<typename Self, typename = void>
    struct Api {
        using Trait = PresenterObject;
        auto AsPresenter() -> Presenter& { return rstd::trait_call<0>(this); }
    };
    template<typename T>
    using Funcs = rstd::TraitFuncs<&T::AsPresenter>;
};

class Presenter {
public:
    virtual ~Presenter() = default;
    template<typename T>
    static auto Make() -> Box<dyn<PresenterObject>>;

    virtual bool Init(GLFWwindow* window) = 0;
    virtual void Shutdown()               = 0;

    virtual rstd::uint32_t Width() const  = 0;
    virtual rstd::uint32_t Height() const = 0;

    virtual bool Resize()                               = 0;
    virtual bool AcceptDmaBuf(const DmaBufFrame& frame) = 0;
    virtual bool AcceptCpuPaint(const CpuPaintFrame& /*frame*/) { return false; }
    virtual bool RenderFrame() = 0;
};

} // namespace weweb

export namespace rstd
{
template<typename T>
    requires requires(T& value) { static_cast<weweb::Presenter&>(value); }
struct Impl<weweb::PresenterObject, T> : ImplBase<T> {
    auto AsPresenter() -> weweb::Presenter& { return this->self(); }
};
} // namespace rstd

template<typename T>
auto weweb::Presenter::Make() -> Box<dyn<PresenterObject>> {
    auto owner = Box<T>::make();
    return Box<dyn<PresenterObject>>::from_raw(
        dyn<PresenterObject>::from_ptr(rstd::move(owner).into_raw()));
}
