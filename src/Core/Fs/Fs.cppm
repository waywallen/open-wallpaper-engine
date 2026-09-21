module;
#include <rstd/macro.hpp>

export module wescene.vfs;
import rstd;

using ::alloc::string::String;
using ::alloc::sync::Arc;
using ::alloc::vec::Vec;
using namespace rstd::prelude;
using namespace rstd::literals;

export namespace owe::fs
{

using Path            = rstd::ref<rstd::path::Path>;
using MountHandle     = Arc<rstd::dyn<struct MountFs>>;
using ReadRange       = rstd::io::ReadRange;
using WriteSeekHandle = rstd::io::WriteSeekHandle;

struct FileMetadata {
    u64  len {};
    bool is_file { false };
    bool is_directory { false };
    bool readonly { false };
};

struct WriteOptions {
    bool create { false };
    bool create_new { false };
    bool truncate { false };
    bool append { false };
};

struct MountId {
    u64 value {};

    friend bool operator==(MountId lhs, MountId rhs) noexcept { return lhs.value == rhs.value; }
};

struct MountFs {
    using Trait                  = MountFs;
    static constexpr bool direct = false;

    template<typename Self, typename Delegate = void>
    struct Api {
        using Trait = MountFs;

        auto open_read(Path path) const -> rstd::io::Result<ReadRange> {
            return rstd::trait_call<0>(this, path);
        }

        auto open_write(Path path, WriteOptions options) const
            -> rstd::io::Result<WriteSeekHandle> {
            return rstd::trait_call<1>(this, path, options);
        }

        auto metadata(Path path) const -> rstd::io::Result<FileMetadata> {
            return rstd::trait_call<2>(this, path);
        }
    };

    template<typename T>
    using Funcs = rstd::TraitFuncs<&T::open_read, &T::open_write, &T::metadata>;
};

namespace detail
{

auto error(rstd::io::error::ErrorKind::Entity kind) -> rstd::io::error::Error {
    return rstd::io::error::Error::from_kind(rstd::io::error::ErrorKind { kind });
}

auto normalize(Path path, bool rooted) -> rstd::io::Result<rstd::path::PathBuf> {
    auto components = path.components();
    auto output     = rstd::path::PathBuf::make();

    if (rooted) {
        auto first = components.next();
        if (first.is_none() || ! first->is_root_dir()) {
            return rstd::Err(error(rstd::io::error::ErrorKind::InvalidInput));
        }
        output = rstd::path::PathBuf::from("/"_str);
    }

    while (true) {
        auto component = components.next();
        if (component.is_none()) break;
        if (component->is_parent_dir() || component->is_root_dir()) {
            return rstd::Err(error(rstd::io::error::ErrorKind::InvalidInput));
        }
        if (! component->is_normal()) continue;
        output.push(Path(component->as_os_str()));
    }
    return rstd::Ok(rstd::move(output));
}

auto normalize_global(Path path) -> rstd::io::Result<rstd::path::PathBuf> {
    return normalize(path, true);
}

auto normalize_relative(Path path) -> rstd::io::Result<rstd::path::PathBuf> {
    return normalize(path, false);
}

bool is_mount_point(Path path) {
    auto components = path.components();
    auto root       = components.next();
    return root.is_some() && root->is_root_dir() && components.next().is_some();
}

bool is_not_found(const rstd::io::error::Error& error) {
    return error.kind().code == rstd::io::error::ErrorKind::NotFound;
}

bool is_readonly(const rstd::io::error::Error& error) {
    auto kind = error.kind().code;
    return kind == rstd::io::error::ErrorKind::ReadOnlyFilesystem ||
           kind == rstd::io::error::ErrorKind::Unsupported;
}

} // namespace detail

auto resolve_beneath(Path root, Path path) -> rstd::io::Result<rstd::path::PathBuf> {
    auto  output     = rstd_try(detail::normalize_global(root));
    auto  components = path.components();
    usize depth {};

    while (true) {
        auto component = components.next();
        if (component.is_none()) break;
        if (component->is_parent_dir()) {
            if (depth != usize()) {
                output.pop();
                --depth;
            }
            continue;
        }
        if (! component->is_normal()) continue;
        output.push(Path(component->as_os_str()));
        ++depth;
    }
    return Ok(rstd::move(output));
}

class PhysicalFs {
public:
    PhysicalFs(const PhysicalFs&)                        = delete;
    auto operator=(const PhysicalFs&) -> PhysicalFs&     = delete;
    PhysicalFs(PhysicalFs&&) noexcept                    = default;
    auto operator=(PhysicalFs&&) noexcept -> PhysicalFs& = default;

    static auto make(Path root, bool create) -> rstd::io::Result<PhysicalFs> {
        if (create) rstd_try(rstd::fs::create_dir_all(root));

        auto metadata = rstd_try(rstd::fs::metadata(root));
        if (! metadata.is_dir()) {
            return Err(detail::error(rstd::io::error::ErrorKind::NotADirectory));
        }

        auto canonical = rstd_try(rstd::fs::canonicalize(root));
        return Ok(PhysicalFs(rstd::move(canonical)));
    }

    auto open_read(Path path) const -> rstd::io::Result<ReadRange> {
        auto resolved = rstd_try(resolve_existing(path));
        auto opened   = rstd_try(rstd::fs::File::open(resolved.as_path()));
        auto info     = rstd_try(opened.metadata());
        if (! info.is_file()) {
            return Err(detail::error(rstd::io::error::ErrorKind::IsADirectory));
        }

        auto source = rstd::io::SharedReadAt::make(rstd::move(opened));
        return ReadRange::make(rstd::move(source), u64(), info.len());
    }

    auto open_write(Path path, WriteOptions options) const -> rstd::io::Result<WriteSeekHandle> {
        auto resolved = rstd_try(resolve_write(path, options.create || options.create_new));

        auto open = rstd::fs::File::options();
        open.write(! options.append)
            .append(options.append)
            .create(options.create)
            .create_new(options.create_new)
            .truncate(options.truncate);
        auto file = rstd_try(open.open(resolved.as_path()));
        return Ok(WriteSeekHandle::make(rstd::move(file)));
    }

    auto metadata(Path path) const -> rstd::io::Result<FileMetadata> {
        auto resolved = rstd_try(resolve_existing(path));
        auto value    = rstd_try(rstd::fs::metadata(resolved.as_path()));
        return Ok(FileMetadata { .len          = value.len(),
                                 .is_file      = value.is_file(),
                                 .is_directory = value.is_dir(),
                                 .readonly     = value.permissions().readonly() });
    }

private:
    explicit PhysicalFs(rstd::path::PathBuf root): m_root(rstd::move(root)) {}

    auto local_path(Path path) const -> rstd::io::Result<rstd::path::PathBuf> {
        auto local = rstd_try(detail::normalize_relative(path));
        return Ok(m_root.join(local.as_path()));
    }

    auto ensure_in_root(rstd::path::PathBuf path) const -> rstd::io::Result<rstd::path::PathBuf> {
        if (! path.as_path().starts_with(m_root.as_path())) {
            return rstd::Err(detail::error(rstd::io::error::ErrorKind::PermissionDenied));
        }
        return rstd::Ok(rstd::move(path));
    }

    auto resolve_existing(Path path) const -> rstd::io::Result<rstd::path::PathBuf> {
        auto full      = rstd_try(local_path(path));
        auto canonical = rstd_try(rstd::fs::canonicalize(full.as_path()));
        return ensure_in_root(rstd::move(canonical));
    }

    auto resolve_write(Path path, bool create) const -> rstd::io::Result<rstd::path::PathBuf> {
        auto full = rstd_try(local_path(path));

        auto metadata = rstd::fs::metadata(full.as_path());
        if (metadata.is_ok()) {
            auto canonical = rstd_try(rstd::fs::canonicalize(full.as_path()));
            return ensure_in_root(rstd::move(canonical));
        }
        auto metadata_error = rstd::move(metadata).unwrap_err_unchecked();
        if (! detail::is_not_found(metadata_error) || ! create) {
            return rstd::Err(rstd::move(metadata_error));
        }

        auto parent = full.as_path().parent();
        if (parent.is_none()) {
            return rstd::Err(detail::error(rstd::io::error::ErrorKind::InvalidInput));
        }

        auto existing = rstd::path::PathBuf::from(*parent);
        while (true) {
            auto exists = rstd::fs::metadata(existing.as_path());
            if (exists.is_ok()) break;
            auto error = rstd::move(exists).unwrap_err_unchecked();
            if (! detail::is_not_found(error) || ! existing.pop()) {
                return rstd::Err(rstd::move(error));
            }
        }
        auto canonical = rstd_try(rstd::fs::canonicalize(existing.as_path()));
        rstd_try(ensure_in_root(rstd::move(canonical)));

        rstd_try(rstd::fs::create_dir_all(*parent));
        auto parent_canonical = rstd_try(rstd::fs::canonicalize(*parent));
        rstd_try(ensure_in_root(rstd::move(parent_canonical)));
        return Ok(rstd::move(full));
    }

    rstd::path::PathBuf m_root;
};

class VFS {
public:
    VFS(): m_state(State { .mounts = Vec<MountedFs>::make(), .next_id = u64(1) }) {}

    VFS(const VFS&)                    = delete;
    auto operator=(const VFS&) -> VFS& = delete;

    auto Snapshot() const -> rstd::sync::Arc<VFS> {
        auto result   = rstd::sync::Arc<VFS>::make();
        auto state    = result->m_state.lock().unwrap_unchecked();
        state->mounts = snapshot();
        for (const auto& mount : state->mounts) {
            if (mount.id.value >= state->next_id) state->next_id = mount.id.value + u64(1);
        }
        return result;
    }

    auto mount(Path mount_point, MountHandle fs, rstd::ref<rstd::str> name = {})
        -> rstd::io::Result<MountId> {
        auto normalized = rstd_try(detail::normalize_global(mount_point));
        if (! detail::is_mount_point(normalized.as_path())) {
            return Err(detail::error(rstd::io::error::ErrorKind::InvalidInput));
        }

        auto state = m_state.lock().unwrap_unchecked();
        if (state->next_id == u64::MAX) {
            return rstd::Err(detail::error(rstd::io::error::ErrorKind::Other));
        }
        auto id = MountId { state->next_id++ };
        state->mounts.push(MountedFs { .id          = id,
                                       .name        = String::make(name),
                                       .mount_point = rstd::move(normalized),
                                       .fs          = rstd::move(fs) });
        return Ok(id);
    }

    bool unmount(MountId id) {
        auto state = m_state.lock().unwrap_unchecked();
        for (usize i = state->mounts.len(); i > usize(); --i) {
            if (state->mounts[i - usize(1)].id == id) {
                state->mounts.remove(i - usize(1));
                return true;
            }
        }
        return false;
    }

    bool is_mounted(rstd::ref<rstd::str> name) const {
        auto state = m_state.lock().unwrap_unchecked();
        for (usize i {}; i < state->mounts.len(); ++i) {
            if (state->mounts[i].name == name) return true;
        }
        return false;
    }

    auto open_read(Path path) const -> rstd::io::Result<ReadRange> {
        auto normalized = rstd_try(detail::normalize_global(path));
        auto mounts     = snapshot();
        for (usize i = mounts.len(); i > usize(); --i) {
            auto& mount = mounts[i - usize(1)];
            auto  local = normalized.as_path().strip_prefix(mount.mount_point.as_path());
            if (local.is_none()) continue;
            auto opened = mount.fs->open_read(*local);
            if (opened.is_ok()) return opened;
            auto error = rstd::move(opened).unwrap_err_unchecked();
            if (! detail::is_not_found(error)) return rstd::Err(rstd::move(error));
        }
        return rstd::Err(detail::error(rstd::io::error::ErrorKind::NotFound));
    }

    auto open_write(Path path, WriteOptions options) const -> rstd::io::Result<WriteSeekHandle> {
        if (options.create_new) {
            auto existing = metadata(path);
            if (existing.is_ok()) {
                return rstd::Err(detail::error(rstd::io::error::ErrorKind::AlreadyExists));
            }
            auto error = rstd::move(existing).unwrap_err_unchecked();
            if (! detail::is_not_found(error)) return rstd::Err(rstd::move(error));
        }

        auto normalized = rstd_try(detail::normalize_global(path));
        auto mounts     = snapshot();

        if (! options.create_new) {
            auto existing_options       = options;
            existing_options.create     = false;
            existing_options.create_new = false;
            for (usize i = mounts.len(); i > usize(); --i) {
                auto& mount = mounts[i - usize(1)];
                auto  local = normalized.as_path().strip_prefix(mount.mount_point.as_path());
                if (local.is_none()) continue;
                auto opened = mount.fs->open_write(*local, existing_options);
                if (opened.is_ok()) return opened;
                auto error = rstd::move(opened).unwrap_err_unchecked();
                if (! detail::is_not_found(error)) return rstd::Err(rstd::move(error));
            }
        }

        if (! options.create && ! options.create_new) {
            return rstd::Err(detail::error(rstd::io::error::ErrorKind::NotFound));
        }
        for (usize i = mounts.len(); i > usize(); --i) {
            auto& mount = mounts[i - usize(1)];
            auto  local = normalized.as_path().strip_prefix(mount.mount_point.as_path());
            if (local.is_none()) continue;
            auto opened = mount.fs->open_write(*local, options);
            if (opened.is_ok()) return opened;
            auto error = rstd::move(opened).unwrap_err_unchecked();
            if (! detail::is_readonly(error) && ! detail::is_not_found(error)) {
                return rstd::Err(rstd::move(error));
            }
        }
        return rstd::Err(detail::error(rstd::io::error::ErrorKind::ReadOnlyFilesystem));
    }

    auto metadata(Path path) const -> rstd::io::Result<FileMetadata> {
        auto normalized = rstd_try(detail::normalize_global(path));
        auto mounts     = snapshot();
        for (usize i = mounts.len(); i > usize(); --i) {
            auto& mount = mounts[i - usize(1)];
            auto  local = normalized.as_path().strip_prefix(mount.mount_point.as_path());
            if (local.is_none()) continue;
            auto result = mount.fs->metadata(*local);
            if (result.is_ok()) return result;
            auto error = rstd::move(result).unwrap_err_unchecked();
            if (! detail::is_not_found(error)) return rstd::Err(rstd::move(error));
        }
        return rstd::Err(detail::error(rstd::io::error::ErrorKind::NotFound));
    }

private:
    struct MountedFs {
        MountId             id;
        String              name;
        rstd::path::PathBuf mount_point;
        MountHandle         fs;

        auto clone() const -> MountedFs {
            return MountedFs {
                .id = id, .name = name.clone(), .mount_point = mount_point.clone(), .fs = fs.clone()
            };
        }
    };

    struct State {
        Vec<MountedFs> mounts;
        u64            next_id;
    };

    auto snapshot() const -> Vec<MountedFs> {
        auto state  = m_state.lock().unwrap_unchecked();
        auto result = Vec<MountedFs>::with_capacity(state->mounts.len());
        for (usize i {}; i < state->mounts.len(); ++i) {
            result.push(state->mounts[i].clone());
        }
        return result;
    }

    rstd::sync::Mutex<State> m_state;
};

} // namespace owe::fs

namespace rstd
{

template<>
struct Impl<owe::fs::MountFs, owe::fs::PhysicalFs> : ImplBase<owe::fs::PhysicalFs> {
    auto open_read(owe::fs::Path path) const -> io::Result<io::ReadRange> {
        return this->self().open_read(path);
    }

    auto open_write(owe::fs::Path path, owe::fs::WriteOptions options) const
        -> io::Result<io::WriteSeekHandle> {
        return this->self().open_write(path, options);
    }

    auto metadata(owe::fs::Path path) const -> io::Result<owe::fs::FileMetadata> {
        return this->self().metadata(path);
    }
};

} // namespace rstd

export namespace owe::fs
{

auto make_physical_fs(Path root, bool create = false) -> rstd::io::Result<MountHandle> {
    auto fs = PhysicalFs::make(root, create);
    if (fs.is_err()) return rstd::Err(rstd::move(fs).unwrap_err_unchecked());
    return rstd::Ok(MountHandle::make(rstd::move(fs).unwrap_unchecked()));
}

} // namespace owe::fs
