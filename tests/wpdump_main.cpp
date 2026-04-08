// wpdump <workshop_dir> [output.json]
//
// Re-runs DumpWorkshop on a single workshop and writes the result as
// pretty JSON. Used to seed tests/fixtures/<category>/<version>/<id>.json
// during version test authoring.

#include <cstdio>
#include <fstream>
#include <string>

#include "dump.hpp"

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: %s <workshop_dir> [output.json]\n",
                     argc > 0 ? argv[0] : "wpdump");
        return 2;
    }
    const std::string workshop_dir = argv[1];
    std::string       err;
    auto              snapshot = wallpaper::testing::DumpWorkshop(workshop_dir, err);
    if (! err.empty()) {
        std::fprintf(stderr, "wpdump: %s\n", err.c_str());
        return 1;
    }
    std::string text = snapshot.dump(2);
    text.push_back('\n');
    if (argc == 3) {
        std::ofstream out(argv[2]);
        if (! out) {
            std::fprintf(stderr, "wpdump: cannot open %s for writing\n", argv[2]);
            return 1;
        }
        out << text;
    } else {
        std::fwrite(text.data(), 1, text.size(), stdout);
    }
    return 0;
}
