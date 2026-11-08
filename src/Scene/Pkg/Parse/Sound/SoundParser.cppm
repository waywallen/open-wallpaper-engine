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
};

class SoundParser {
public:
    static Arc<rstd::dyn<SceneSoundControl>> Parse(const wpscene::SoundObject&, fs::VFS&,
                                                   wavsen::audio::SoundManager&, Scene*,
                                                   rstd::Option<SceneSoundUnmountCallback> = {});
    static auto PrepareAsync(const wpscene::SoundObject&, fs::VFS&, Scene*,
                             SceneSoundUnmountCallback) -> ScenePreparedSound;
};

} // namespace owe
