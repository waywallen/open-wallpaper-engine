export module wescene.pkg.parse:scene_stages;
import rstd;
import rstd.cppstd;
import wescene.json;
import wescene.fs;
import wescene.pkg.scene_obj;
import wescene.scene;

using namespace rstd::prelude;

export namespace owe
{

using SceneObjectVar = wpscene::SceneObject;

enum class SceneAnimationBindingIssue
{
    MissingRelation,
    NonReciprocalRelation,
    AmbiguousTarget,
    MetadataMismatch,
    Cycle,
    DuplicateName,
    UnsupportedTarget,
};

struct SceneAnimationBindingDiagnostic {
    SceneAnimationBindingIssue issue { SceneAnimationBindingIssue::MissingRelation };
    u64                        binding {};
    String                     field;
    String                     relation;
};

class SceneAnimationBindingScope {
public:
    void Clear() {
        m_tracks.clear();
        m_diagnostics.clear();
    }
    void Insert(u64 identity, SceneAnimationTrack track) {
        (void)m_tracks.insert(identity, rstd::move(track));
    }
    auto Resolve(const wpscene::FieldBindingSpec&) const -> Option<SceneAnimationTrack>;
    void Report(SceneAnimationBindingDiagnostic diagnostic) {
        m_diagnostics.push(rstd::move(diagnostic));
    }
    auto Diagnostics() const -> slice<SceneAnimationBindingDiagnostic> {
        return m_diagnostics.as_slice();
    }

private:
    rstd::collections::HashMap<u64, SceneAnimationTrack> m_tracks;
    Vec<SceneAnimationBindingDiagnostic>                 m_diagnostics;
};

auto BuildAnimationBindingScope(const wpscene::FieldBindings&) -> SceneAnimationBindingScope;
auto BuildAnimationBindingScope(const wpscene::ImageObject&) -> SceneAnimationBindingScope;

enum class TextRenderMode
{
    Direct,
    Offscreen,
};

struct TextSurfaceRequirements {
    bool has_effect { false };
    bool copy_background { false };
    bool opaque_background { false };
    bool linked_source { false };
};

constexpr auto ResolveTextRenderMode(TextSurfaceRequirements requirements) -> TextRenderMode {
    return requirements.has_effect || requirements.copy_background ||
                   requirements.opaque_background || requirements.linked_source
               ? TextRenderMode::Offscreen
               : TextRenderMode::Direct;
}

// Compatibility entry for callers with an already-parsed raw value.
Vec<SceneObjectVar> ExpandObjects(const Json&, fs::VFS&, wpscene::SceneVersion,
                                  Option<ref<rstd::json::Map>> user_props = None());

// Canonical cheap expansion path. SceneDocument owns authored object order
// and schema classification; no Scene / glslang state is constructed.
Vec<SceneObjectVar> ExpandObjects(ref<wpscene::SceneDocument>, mut_ref<fs::VFS>,
                                  Option<ref<rstd::json::Map>> user_props = None());

// Resolves the effective width/height without mutating the parsed metadata.
array<i32, 2> ResolveOrthoProjectionExtent(const wpscene::SceneMetadata&, slice<SceneObjectVar>);

} // namespace owe
