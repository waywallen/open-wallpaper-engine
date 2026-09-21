export module weweb:manifest;

import rstd;
import wescene.json;

using namespace rstd::prelude;
using rstd::path::Path;

export namespace weweb
{

struct WebManifest {
    String         title;
    String         entry_html;
    owe::Json      user_props;
    Option<String> preview;
};

Option<WebManifest> LoadWebManifest(ref<Path> workshop_dir);

} // namespace weweb
