#include "setup/prepare.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>

namespace disc = starlet::disc;

namespace {
std::string read_text(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw disc::DiscError("cannot read " + path + ": " + std::strerror(errno));
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 3 || argc > 4) {
        std::fprintf(stderr, "usage: %s GAMEFILES_DIR PORT_DIR [MANIFEST]\n", argv[0]);
        return 2;
    }
    const std::string gamefiles = argv[1], port = argv[2];
    try {
        disc::Manifest manifest;
        const auto game = sunshine::setup::supported_game(argc == 4 ? &manifest : nullptr);
        if (disc::is_prepared(port, game)) {
            std::puts("Game files are already prepared.");
            return 0;
        }
        if (argc == 4) manifest = disc::parse_manifest(read_text(argv[3]));
        const auto path = disc::find_disc_image(gamefiles, disc::formats::all, game.title);
        std::printf("Reading %s\n", path.substr(path.find_last_of('/') + 1).c_str());
        const auto image = disc::open_disc_image(path);
        disc::SetupOptions options;
        int shown = -1;
        options.progress = [&](std::uint64_t done, std::uint64_t total) {
            const int percent = total ? static_cast<int>(done * 100 / total) : 100;
            if (percent == shown) return;
            shown = percent;
            std::printf("Extracting game files: %d%% (%llu of %llu MiB)\n", percent,
                        static_cast<unsigned long long>(done >> 20), static_cast<unsigned long long>(total >> 20));
        };
        const auto result = disc::install(*image, port, game, options);
        std::printf("Prepared %llu files (%llu MiB).\n", static_cast<unsigned long long>(result.files),
                    static_cast<unsigned long long>(result.bytes >> 20));
        return 0;
    } catch (const std::exception& error) {
        std::printf("ERROR: %s\n", error.what());
    }
    return 1;
}
