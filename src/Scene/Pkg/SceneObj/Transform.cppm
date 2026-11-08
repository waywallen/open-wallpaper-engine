export module wescene.pkg.scene_obj:transform;
import rstd;
import wescene.json;

using namespace rstd::prelude;
using namespace rstd::literals;

export namespace owe::wpscene
{

inline bool ReadLayerScale(const owe::Json& json, array<float, 3>& scale, bool warn = true) {
    return owe::GetJsonVec2OrVec3(json, "scale"_str, scale, warn);
}

} // namespace owe::wpscene
