module;

#include <dlfcn.h>

module wescene.types;
import rstd;

using namespace rstd::prelude;
using namespace rstd::literals;

namespace owe
{

ref<str> ToString(const ImageType& type) {
#define IMG(x) \
    case ImageType::x: return #x ""_str;
    switch (type) {
        IMG(UNKNOWN);
        IMG(BMP);
        IMG(ICO);
        IMG(JPEG);
        IMG(JNG);
        IMG(PNG);
        IMG(VIDEO);
    default:
        rstd::io::eprintln { "[ERROR] Not valid image type: {}", static_cast<int>(type) };
        return ""_str;
    }
#undef IMG
}

ref<str> ToString(const TextureFormat& format) {
#define FMT(x) \
    case TextureFormat::x: return #x ""_str;
    switch (format) {
        FMT(RGBA8);
        FMT(BC1);
        FMT(BC2);
        FMT(BC3);
        FMT(RGB8);
        FMT(RG8);
        FMT(R8);
        FMT(D32F);
    default:
        rstd::io::eprintln { "[ERROR] Not valid tex format: {}", static_cast<int>(format) };
        return ""_str;
    }
#undef FMT
}

} // namespace owe

namespace utils
{

DynamicLibrary::DynamicLibrary() = default;
DynamicLibrary::DynamicLibrary(const char* filename) { Open(filename); }
DynamicLibrary::~DynamicLibrary() { Close(); }
DynamicLibrary::DynamicLibrary(DynamicLibrary&& o) noexcept
    : handle(rstd::exchange(o.handle, nullptr)) {}
DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& o) noexcept {
    Close();
    handle = rstd::exchange(o.handle, nullptr);
    return *this;
}
bool DynamicLibrary::Open(const char* filename) {
    handle = dlopen(filename, RTLD_NOW);
    return IsOpen();
}
bool DynamicLibrary::IsOpen() const { return handle != nullptr; }
void DynamicLibrary::Close() {
    if (IsOpen()) {
        dlclose(handle);
        handle = nullptr;
    }
}
void* DynamicLibrary::GetSymbolAddr(const char* name) const {
    return reinterpret_cast<void*>(dlsym(handle, name));
}

} // namespace utils
