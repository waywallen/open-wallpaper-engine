module;

export module wescene.pkg.parse:sound_parser;
import rstd;
import wavsen.audio;
import wescene.fs;
import wescene.scene;
import wescene.pkg.scene_obj;

using namespace rstd::prelude;

export namespace owe
{

using rstd::sync::Arc;
using SceneSoundUnmountCallback = Arc<rstd::dyn<rstd::Fn<void(wavsen::audio::SoundStreamId)>>>;
using SceneSoundPosition        = rstd::sync::Mutex<array<float, 3>>;

class SceneSoundMountState {
public:
    bool IsCanceled();
    bool Attach(wavsen::audio::SoundStreamId);
    auto Cancel() -> rstd::Option<wavsen::audio::SoundStreamId>;

private:
    struct State {
        bool                                       canceled { false };
        rstd::Option<wavsen::audio::SoundStreamId> id;
    };
    rstd::sync::Mutex<State> m_state { State {} };
};

using SceneSoundMountCallback = Arc<rstd::dyn<
    rstd::Fn<bool(Box<rstd::dyn<wavsen::audio::SoundStreamObject>>, Arc<SceneSoundMountState>)>>>;

struct ScenePreparedSound {
    Arc<rstd::dyn<SceneSoundControl>>                control;
    Box<rstd::dyn<wavsen::audio::SoundStreamObject>> stream;
    Arc<SceneSoundMountState>                        mount;
    Option<Arc<SceneSoundPosition>>                  position;
};

class SoundParser {
public:
    static auto Prepare(const wpscene::SoundObject&, fs::VFS&, Scene*,
                        Option<SceneSoundUnmountCallback> = {}) -> ScenePreparedSound;
    static void TrackPosition(Scene&, SceneNode&, Arc<SceneSoundPosition>, bool orthographic);
};

} // namespace owe
