// CoN's textures, decoded at run time from the <LEVEL>.TEX files of the game's disc image. Format: tools/con_tex.py.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "bozo/libs/cd.h"
#include "bozo/render/renderer.h"
#include "bozo/render/texture.h"

namespace con::textures
{
    class Cell
    {
    public:
        explicit Cell(std::vector<uint8_t> bytes);
        ~Cell();
        uint32_t count() const;
        // RGBA with the GS's alpha, as the renderer takes it. Texels no rectangle covers take palette entry 0.
        bool decode(uint32_t index, bozo::render::Texture &out);

    private:
        struct Tables;
        std::vector<uint8_t> bytes_;
        std::map<uint32_t, std::unique_ptr<Tables>> tables_;
    };

    // A lump's texture as the game holds it in memory: its 0x80-byte header, then the upload the file stores.
    bool decodeLump(const uint8_t *rdram, uint32_t address, bozo::render::Texture &out);

    uint64_t lumpKey(const uint8_t *rdram, uint32_t address);
    uint32_t findResource(const uint8_t *rdram, uint32_t gp, const char *lumpName, const char *name);

    // A ResourceMap's texture once the game has resolved it for the current lump generation, decoded when its lump changes.
    struct Mapped
    {
        std::shared_ptr<const bozo::render::Texture> texture;
        uint32_t lump = 0;
    };
    const bozo::render::Texture *mapped(const uint8_t *rdram, uint32_t gp, uint32_t map, Mapped &cache);

    // A model's GS ALPHA register value (its .vif header +0x18) as the renderer's blend: the texture's alpha is turned the
    // way that blend reads it.
    void modelBlend(uint64_t alpha, bozo::render::Texture &texture, bozo::render::Material &material);

    class Level
    {
    public:
        bool open(const std::string &image, const std::string &name, std::string &error);
        const std::vector<int32_t> &cellIds() const { return cellIds_; }
        Cell *cell(int32_t id);
        std::shared_ptr<const bozo::render::Texture> texture(int32_t cellId, uint32_t index);

    private:
        struct Extent
        {
            uint32_t offset = 0, size = 0;
        };
        bozo::libs::DiscImage disc_;
        uint32_t firstSector_ = 0;
        uint64_t serial_ = 0;
        std::vector<int32_t> cellIds_;
        std::map<int32_t, Extent> extents_;
        std::map<int32_t, Cell> cells_;
    };
}
