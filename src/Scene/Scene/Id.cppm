export module wescene.scene:id;
export import vrento.scene_identity;
import rstd;

using namespace rstd::prelude;

export namespace owe
{

template<typename Tag>
using SceneResourceId = vrento::SceneId<Tag>;

using SceneNodeIdTag = vrento::NodeIdTag;
struct SceneEffectIdTag;
using SceneMaterialIdTag     = vrento::MaterialIdTag;
using SceneMeshIdTag         = vrento::GeometryIdTag;
using SceneDrawItemIdTag     = vrento::DrawIdTag;
using SceneTextureIdTag      = vrento::TextureIdTag;
using SceneRenderTargetIdTag = vrento::RenderTargetIdTag;
using SceneCameraIdTag       = vrento::CameraIdTag;

using SceneNodeId         = SceneResourceId<SceneNodeIdTag>;
using SceneEffectId       = SceneResourceId<SceneEffectIdTag>;
using SceneMaterialId     = SceneResourceId<SceneMaterialIdTag>;
using SceneMeshId         = SceneResourceId<SceneMeshIdTag>;
using SceneDrawItemId     = SceneResourceId<SceneDrawItemIdTag>;
using SceneTextureId      = SceneResourceId<SceneTextureIdTag>;
using SceneRenderTargetId = SceneResourceId<SceneRenderTargetIdTag>;
using SceneCameraId       = SceneResourceId<SceneCameraIdTag>;

} // namespace owe
