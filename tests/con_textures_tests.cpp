#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

#include "con_textures.h"

namespace
{
    int failures = 0;

    void check(bool ok, const char *what)
    {
        if (!ok) {
            std::printf("FAILED: %s\n", what);
            ++failures;
        }
    }

    template <typename T>
    void put(std::vector<uint8_t> &out, size_t at, T value)
    {
        if (out.size() < at + sizeof(T))
            out.resize(at + sizeof(T));
        std::memcpy(out.data() + at, &value, sizeof(T));
    }

    struct MsbBits
    {
        std::vector<uint8_t> bytes;
        int used = 0;
        void add(uint32_t value, int count)
        {
            for (int i = count - 1; i >= 0; --i) {
                if (!(used % 8))
                    bytes.push_back(0);
                if (value >> i & 1)
                    bytes.back() |= uint8_t(0x80 >> (used % 8));
                ++used;
            }
        }
    };

    std::vector<uint8_t> header(uint16_t width, uint16_t height, uint16_t flags, uint32_t payload)
    {
        std::vector<uint8_t> cell(0x80);
        put<uint32_t>(cell, 0, 1);
        put<uint16_t>(cell, 0x40, width);
        put<uint16_t>(cell, 0x42, height);
        put<uint16_t>(cell, 0x48, flags);
        put<uint32_t>(cell, 0x50, payload);
        return cell;
    }

    void append(std::vector<uint8_t> &cell, const std::vector<uint8_t> &bytes)
    {
        cell.insert(cell.end(), bytes.begin(), bytes.end());
    }

    // Two colours, vectors 0 {0, 1, 1, 0} and 1 {1, 1, 1, 1}. Texel code: 0 -> colour 0, 1 -> colour 1 (1 bit). Vector
    // code: the same. A 32 x 16 texture of two blocks, the second one a reference to the first.
    void vqTexture()
    {
        constexpr uint32_t payload = 0x80;
        std::vector<uint8_t> stream;
        MsbBits texture;
        texture.add(0, 2);
        texture.add(0, 1);
        texture.add(2, 2);
        texture.add(1, 1);
        const int firstBlock = texture.used;
        texture.add(0, 1);
        for (int i = 0; i < 64; ++i)
            texture.add(i == 0 ? 1 : 0, 1);
        texture.add(1, 1);
        texture.add(uint32_t(0x84 * 8 + firstBlock), 24);
        texture.add(1, 2);
        texture.add(0, 1);
        texture.add(0, 2);
        texture.add(0, 1);

        const uint32_t shared = payload + 4 + uint32_t(texture.bytes.size());
        auto cell = header(32, 16, 0x10d, payload);
        put<uint32_t>(cell, payload, shared);
        append(cell, texture.bytes);
        put<uint16_t>(cell, shared, 2);
        put<uint16_t>(cell, shared + 2, 2);
        put<uint32_t>(cell, shared + 4, 0x80000000u);
        put<uint32_t>(cell, shared + 8, 0x80ffffffu);
        size_t at = shared + 12;
        for (int table = 0; table < 2; ++table) {
            put<uint32_t>(cell, at, 1);
            put<uint32_t>(cell, at + 4, 0);
            put<int32_t>(cell, at + 8, 0);
            put<uint32_t>(cell, at + 12, 0);
            put<int32_t>(cell, at + 16, -1);
            at += 20;
        }
        MsbBits codebook;
        for (uint32_t bit : {0u, 1u, 1u, 0u, 1u, 1u, 1u, 1u})
            codebook.add(bit, 1);
        put<uint8_t>(cell, at, codebook.bytes[0]);

        con::textures::Cell source(cell);
        bozo::render::Texture out;
        check(source.count() == 1, "VQ count");
        check(source.decode(0, out), "VQ decodes");
        check(out.width == 32 && out.height == 16 && out.rgba.size() == 512, "VQ size");
        if (out.rgba.size() != 512)
            return;
        const uint32_t black = 0x80000000u, white = 0x80ffffffu;
        check(out.rgba[0] == white && out.rgba[1] == white, "VQ vector 1 first");
        check(out.rgba[2] == black && out.rgba[3] == white, "VQ vector 0, top row");
        check(out.rgba[32 + 2] == white && out.rgba[32 + 3] == black, "VQ vector 0, bottom row");
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x)
                check(out.rgba[size_t(y) * 32 + x + 16] == out.rgba[size_t(y) * 32 + x], "VQ reference repeats the block");
        check(!source.decode(1, out), "VQ index out of range");
    }

    // One colour: its texel code has length 0.
    void vqOneColour()
    {
        constexpr uint32_t payload = 0x80;
        MsbBits texture;
        texture.add(0, 1);
        texture.add(0, 1);
        texture.add(1, 1);
        texture.add(1, 1);
        texture.add(0, 1);
        for (int i = 0; i < 64; ++i)
            texture.add(0, 1);
        texture.add(1, 1);
        texture.add(0, 1);
        texture.add(0, 1);
        texture.add(0, 1);

        const uint32_t shared = payload + 4 + uint32_t(texture.bytes.size());
        auto cell = header(16, 16, 0x10d, payload);
        put<uint32_t>(cell, payload, shared);
        append(cell, texture.bytes);
        put<uint16_t>(cell, shared, 1);
        put<uint16_t>(cell, shared + 2, 1);
        put<uint32_t>(cell, shared + 4, 0x80123456u);
        size_t at = shared + 8;
        for (int shortest : {0, 1}) {
            put<int32_t>(cell, at, shortest);
            put<uint32_t>(cell, at + 4, 0);
            put<int32_t>(cell, at + 8, 0);
            put<uint32_t>(cell, at + 12, 0);
            put<int32_t>(cell, at + 16, -1);
            at += 20;
        }

        con::textures::Cell source(cell);
        bozo::render::Texture out;
        check(source.decode(0, out), "a zero-length code decodes");
        check(out.rgba.size() == 256 && out.rgba[255] == 0x80123456u, "a zero-length code gives its one colour");
    }

    void gifTag(std::vector<uint8_t> &out, uint64_t loops, uint64_t kind, uint64_t registers)
    {
        const size_t at = out.size();
        put<uint64_t>(out, at, loops | kind << 58 | registers << 60);
        put<uint64_t>(out, at + 8, 0x0e);
    }

    void gifRegister(std::vector<uint8_t> &out, uint64_t value, uint64_t id)
    {
        const size_t at = out.size();
        put<uint64_t>(out, at, value);
        put<uint64_t>(out, at + 8, id);
    }

    void upload(std::vector<uint8_t> &out, uint64_t format, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                const std::vector<uint8_t> &data)
    {
        gifTag(out, 3, 0, 1);
        gifRegister(out, format << 56, 0x50);
        gifRegister(out, uint64_t(x) << 32 | uint64_t(y) << 48, 0x51);
        gifRegister(out, uint64_t(width) | uint64_t(height) << 32, 0x52);
        gifTag(out, (data.size() + 15) / 16, 2, 0);
        append(out, data);
        out.resize((out.size() + 15) / 16 * 16);
    }

    void lumpFourBit()
    {
        constexpr uint32_t address = 0x1000;
        std::vector<uint8_t> packet;
        std::vector<uint8_t> colours(64);
        put<uint32_t>(colours, 3 * 4, 0x80112233u);
        put<uint32_t>(colours, 5 * 4, 0x80445566u);
        upload(packet, 0, 0, 0, 8, 2, colours);
        upload(packet, 0x14, 0, 0, 2, 1, {0x53});

        std::vector<uint8_t> ram(0x2000000);
        put<uint16_t>(ram, address, 4);
        put<uint16_t>(ram, address + 2, 2);
        put<uint16_t>(ram, address + 6, uint16_t(packet.size() / 16));
        put<uint16_t>(ram, address + 8, 0x2000);
        put<uint32_t>(ram, address + 0xc, 0x14u << 6);
        put<uint32_t>(ram, address + 0x10, 0x80);
        std::memcpy(ram.data() + address + 0x80, packet.data(), packet.size());

        bozo::render::Texture out;
        check(con::textures::decodeLump(ram.data(), address, out), "a 4-bit lump texture decodes");
        check(out.width == 4 && out.height == 2 && out.rgba[0] == 0x80112233u && out.rgba[1] == 0x80445566u, "low nibble first");

        put<uint16_t>(ram, address + 0xa, 0xa7fb);
        put<uint32_t>(ram, address + 0x10, address + 0x80);
        check(con::textures::decodeLump(ram.data(), address, out), "a relocated pointer is absolute");

        put<uint32_t>(ram, address + 0xc, 0x02u << 6);
        check(!con::textures::decodeLump(ram.data(), address, out), "a format with no decoder is refused");
    }

    std::vector<uint8_t> tileCell(const std::vector<uint32_t> &codes)
    {
        constexpr uint32_t payload = 0x80, shared = 0x100;
        auto cell = header(16, 16, 0x10c, payload);
        put<uint32_t>(cell, payload, shared);
        put<uint32_t>(cell, payload + 4, 0);
        const uint32_t stream = shared + 0xc00 + 4 + 4 * 2 + 18 * 4 + 17 * 4;
        put<uint32_t>(cell, payload + 8, stream);
        put<uint8_t>(cell, payload + 12, 0xff);
        for (uint32_t i = 0; i < 256; ++i) {
            const uint32_t stored = (i & ~0x18u) | (i & 8u) << 1 | (i & 0x10u) >> 1;
            put<uint32_t>(cell, shared + stored * 4, 0x80000000u | i);
        }
        put<uint8_t>(cell, shared + 0x400 + 5 * 8, 9);
        put<uint32_t>(cell, shared + 0xc00, 4);
        const uint16_t symbols[4] = {5, 0x100, 0x105, 0x101};
        for (int i = 0; i < 4; ++i)
            put<uint16_t>(cell, shared + 0xc04 + size_t(i) * 2, symbols[i]);
        const size_t first = shared + 0xc04 + 8, last = first + 18 * 4;
        for (int k = 0; k < 17; ++k) {
            put<int32_t>(cell, first + size_t(k) * 4, 0);
            put<int32_t>(cell, last + size_t(k) * 4, k == 2 ? 3 : -1);
        }
        put<int32_t>(cell, first + 17 * 4, 0);
        for (size_t i = 0; i < codes.size() || i < 512; ++i) {
            uint16_t word = 0;
            for (size_t bit = 0; bit < 8; ++bit)
                word = uint16_t(word << 2 | (i * 8 + bit < codes.size() ? codes[i * 8 + bit] : 0));
            put<uint16_t>(cell, stream + i * 2, word);
        }
        return cell;
    }

    void tileTexture()
    {
        std::vector<uint32_t> codes(256, 0);
        codes[1] = 2;
        codes[2] = 1;
        codes[16] = 3;
        con::textures::Cell source(tileCell(codes));
        bozo::render::Texture out;
        check(source.decode(0, out), "tiles decode");
        if (out.rgba.size() != 256)
            return check(false, "tiles size");
        const auto index = [&](int x, int y) { return out.rgba[size_t(y) * 16 + x] & 0xffu; };
        check(index(0, 0) == 5, "tile literal, palette in index order");
        check(index(1, 0) == 9, "tile successor");
        check(index(2, 0) == 9, "tile left neighbour");
        check(index(0, 1) == 5, "tile upper neighbour");
        check(index(15, 15) == 5, "tile last texel");

        std::vector<uint32_t> backwards(256, 1);
        con::textures::Cell refused(tileCell(backwards));
        check(!refused.decode(0, out), "a reference before the block is refused");

        auto truncated = tileCell(codes);
        truncated.resize(0x100 + 0xc00);
        con::textures::Cell cut(truncated);
        check(!cut.decode(0, out), "a cell cut short is refused");
    }
}

int main()
{
    vqTexture();
    vqOneColour();
    tileTexture();
    lumpFourBit();
    std::printf(failures ? "%d failed\n" : "ok\n", failures);
    return failures ? 1 : 0;
}
