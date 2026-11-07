// Prints a hash of every texture of a level, decoded from the disc image, to compare with tools/con_tex.py hash.
//   con-tex-check <disc image> <level>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

#include "con_textures.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: con-tex-check <disc image> <level>\n");
        return 2;
    }
    con::textures::Level level;
    std::string error;
    if (!level.open(argv[1], argv[2], error)) {
        std::fprintf(stderr, "con-tex-check: %s\n", error.c_str());
        return 1;
    }
    uint64_t textures = 0, failed = 0;
    double decoding = 0;
    for (const int32_t id : level.cellIds()) {
        const auto *cell = level.cell(id);
        if (!cell) {
            std::fprintf(stderr, "con-tex-check: cell %d cannot be read\n", id);
            return 1;
        }
        for (uint32_t i = 0; i < cell->count(); ++i) {
            const auto start = std::chrono::steady_clock::now();
            const auto texture = level.texture(id, i);
            decoding += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            ++textures;
            if (!texture) {
                std::printf("%d %u failed\n", id, i);
                ++failed;
                continue;
            }
            uint64_t hash = 0xcbf29ce484222325ull;
            for (const uint32_t texel : texture->rgba)
                for (int shift = 0; shift < 32; shift += 8)
                    hash = (hash ^ ((texel >> shift) & 0xffu)) * 0x100000001b3ull;
            std::printf("%d %u %u %u %016llx\n", id, i, texture->width, texture->height, (unsigned long long)hash);
        }
    }
    std::fprintf(stderr, "%s: %llu textures, %llu failed, %.2f s decoding\n", argv[2], (unsigned long long)textures,
                 (unsigned long long)failed, decoding);
    return failed ? 1 : 0;
}
