// Workshop fixture dumper.
//
// DumpWorkshop opens <workshop_dir>/scene.pkg, walks the package + the
// loose files in workshop_dir, and produces a deterministic JSON snapshot
// covering the bits of metadata we care about for regression-testing the
// parser stack:
//
//   * pkg-level: pkg version string, file count, presence of scene.json
//   * scene.json: WPScene::FromJson result + a few derived fields
//   * textures: every .tex file under /materials/, with TEXV/TEXI/TEXB
//     version stamps and core header fields
//   * puppets: every .mdl file in the pkg, parsed via WPMdlParser, with
//     MDLV/MDLS/MDLA version stamps and bone/anim counts
//
// The same library backs both `wpdump` (CLI that writes the snapshot to
// stdout for fixture seeding) and `verify_dump` (CTest binary that
// regenerates the snapshot and compares it against a checked-in fixture).

#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace wallpaper::testing {

// Returns a json snapshot of `workshop_dir`. On failure returns a json
// object with `{"error": "..."}` and `err` is set to the same message.
nlohmann::json DumpWorkshop(const std::string& workshop_dir, std::string& err);

} // namespace wallpaper::testing
