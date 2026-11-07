#include "con_textures.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <utility>

namespace con::textures
{
    namespace
    {
        constexpr uint16_t kVq = 0x0001, kTiles = 0x0100;
        constexpr uint32_t kHeader = 0x40, kBlock = 16;
        constexpr uint32_t kPaletteBytes = 0x400, kSuccessors = 0x400, kCodeCount = 0xc00;
        constexpr int kMaxCode = 16, kFastBits = 8;

        struct Bytes
        {
            const uint8_t *data;
            size_t size;
            bool has(size_t offset, size_t count) const { return offset <= size && count <= size - offset; }
            uint16_t u16(size_t offset) const
            {
                uint16_t value;
                std::memcpy(&value, data + offset, 2);
                return value;
            }
            uint32_t u32(size_t offset) const
            {
                uint32_t value;
                std::memcpy(&value, data + offset, 4);
                return value;
            }
        };

        class VqBits
        {
        public:
            VqBits(Bytes cell, size_t offset) : cell_(cell), at_(offset) {}
            uint32_t peek()
            {
                while (count_ < 32) {
                    const uint64_t byte = at_ < cell_.size ? cell_.data[at_] : 0;
                    ++at_;
                    buffer_ |= byte << (56 - count_);
                    count_ += 8;
                }
                return uint32_t(buffer_ >> 32);
            }
            void skip(int bits)
            {
                buffer_ = bits >= 64 ? 0 : buffer_ << bits;
                count_ -= bits;
            }
            uint32_t raw(int bits)
            {
                if (!bits)
                    return 0;
                const uint32_t value = peek() >> (32 - bits);
                skip(bits);
                return value;
            }

        private:
            Bytes cell_;
            size_t at_;
            uint64_t buffer_ = 0;
            int count_ = 0;
        };

        struct VqHuffman
        {
            int shortest = 0;
            std::vector<std::pair<uint32_t, int32_t>> levels;

            bool read(Bytes cell, size_t &offset)
            {
                if (!cell.has(offset, 4))
                    return false;
                shortest = int32_t(cell.u32(offset));
                offset += 4;
                for (;;) {
                    if (!cell.has(offset, 8))
                        return false;
                    const uint32_t limit = cell.u32(offset);
                    const int32_t index = int32_t(cell.u32(offset + 4));
                    offset += 8;
                    if (index == -1)
                        break;
                    levels.emplace_back(limit, index);
                }
                return shortest >= 0 && !levels.empty() && shortest + int(levels.size()) - 1 <= 32;
            }
            int32_t decode(VqBits &bits) const
            {
                const uint32_t window = bits.peek();
                size_t level = 0;
                for (size_t i = 0; i < levels.size(); ++i)
                    if (levels[i].first <= window)
                        level = i;
                const auto &[limit, index] = levels[level];
                const int length = shortest + int(level);
                bits.skip(length);
                return index + int32_t((uint64_t(window - limit)) >> (32 - length));
            }
        };

        uint32_t fieldBits(uint32_t count)
        {
            uint32_t bits = 0;
            for (; count; count >>= 1)
                ++bits;
            return std::max(bits, 1u);
        }

        uint32_t clutOrder(uint32_t index)
        {
            return (index & ~0x18u) | (index & 0x08u) << 1 | (index & 0x10u) >> 1;
        }
    }

    struct Cell::Tables
    {
        std::array<uint32_t, 256> palette{};
        VqHuffman vectors;
        std::vector<std::array<uint8_t, 4>> codebook;

        const uint8_t *successors = nullptr;
        std::vector<uint16_t> symbols;
        std::array<int32_t, kMaxCode + 1> first{}, last{};
        std::array<std::pair<uint16_t, uint8_t>, 1u << kFastBits> fast{};

        bool readVq(Bytes cell, uint32_t offset)
        {
            if (!cell.has(offset, 4))
                return false;
            const uint32_t colours = cell.u16(offset), count = cell.u16(offset + 2);
            size_t at = size_t(offset) + 4;
            if (colours > palette.size() || !cell.has(at, colours * 4u))
                return false;
            std::memcpy(palette.data(), cell.data + at, colours * 4u);
            at += colours * 4u;
            VqHuffman texels;
            if (!texels.read(cell, at) || !vectors.read(cell, at))
                return false;
            VqBits bits(cell, at);
            codebook.resize(count);
            for (auto &vector : codebook)
                for (auto &texel : vector) {
                    const int32_t symbol = texels.decode(bits);
                    if (symbol < 0 || symbol > 255)
                        return false;
                    texel = uint8_t(symbol);
                }
            return true;
        }

        bool readTiles(Bytes cell, uint32_t offset)
        {
            if (!cell.has(offset, kCodeCount + 4))
                return false;
            for (uint32_t i = 0; i < palette.size(); ++i)
                palette[i] = cell.u32(size_t(offset) + clutOrder(i) * 4);
            successors = cell.data + offset + kSuccessors;
            const uint32_t count = cell.u32(size_t(offset) + kCodeCount);
            const size_t at = size_t(offset) + kCodeCount + 4;
            const size_t firstAt = at + size_t(count) * 2, lastAt = firstAt + 18 * 4;
            if (!cell.has(lastAt, 17 * 4))
                return false;
            symbols.resize(count);
            std::memcpy(symbols.data(), cell.data + at, size_t(count) * 2);
            for (int k = 0; k <= kMaxCode; ++k) {
                first[k] = int32_t(cell.u32(firstAt + size_t(k) * 4));
                last[k] = int32_t(cell.u32(lastAt + size_t(k) * 4));
            }
            for (uint32_t prefix = 0; prefix < fast.size(); ++prefix)
                for (int k = 1; k <= kFastBits; ++k) {
                    const int32_t top = int32_t(prefix >> (kFastBits - k));
                    if (last[k] >= top) {
                        const int64_t symbol = int64_t(top) + first[k];
                        if (symbol < 0 || symbol >= int64_t(count))
                            return false;
                        fast[prefix] = {symbols[size_t(symbol)], uint8_t(k)};
                        break;
                    }
                }
            return true;
        }

        bool tile(Bytes cell, uint32_t offset, uint8_t *texels) const
        {
            static constexpr int kNeighbours[5] = {-1, -16, -17, -15, -2};
            uint32_t position = 0;
            uint8_t previous = 0;
            for (int i = 0; i < 256; ++i) {
                const size_t word = size_t(offset) + (position >> 4) * 2;
                if (!cell.has(word, 4))
                    return false;
                const uint32_t window = uint32_t(cell.u16(word)) << 16 | cell.u16(word + 2);
                const uint32_t top = (window << (position & 15)) >> 16;
                uint32_t symbol = fast[top >> 8].first;
                int length = fast[top >> 8].second;
                if (!length) {
                    length = kFastBits + 1;
                    while (length <= kMaxCode && last[length] < int32_t(top >> (16 - length)))
                        ++length;
                    if (length > kMaxCode)
                        return false;
                    const int64_t at = int64_t(top >> (16 - length)) + first[length];
                    if (at < 0 || at >= int64_t(symbols.size()))
                        return false;
                    symbol = symbols[size_t(at)];
                }
                position += uint32_t(length);
                uint8_t value;
                if (symbol < 0x100) {
                    value = uint8_t(symbol);
                } else if (symbol < 0x105) {
                    const int from = i + kNeighbours[symbol - 0x100];
                    if (from < 0)
                        return false;
                    value = texels[from];
                } else if (symbol < 0x10d) {
                    value = successors[size_t(previous) * 8 + (symbol - 0x105)];
                } else {
                    return false;
                }
                texels[i] = value;
                previous = value;
            }
            return true;
        }
    };

    Cell::Cell(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
    Cell::~Cell() = default;

    uint32_t Cell::count() const
    {
        const Bytes cell{bytes_.data(), bytes_.size()};
        return cell.has(0, 4) ? cell.u32(0) : 0;
    }

    bool Cell::decode(uint32_t index, bozo::render::Texture &out)
    {
        const Bytes cell{bytes_.data(), bytes_.size()};
        if (index >= count())
            return false;
        const size_t header = size_t(index + 1) * kHeader;
        if (!cell.has(header, kHeader))
            return false;
        const uint32_t width = cell.u16(header), height = cell.u16(header + 2);
        const uint16_t flags = cell.u16(header + 8);
        const uint32_t payload = cell.u32(header + 0x10);
        if (!(flags & (kVq | kTiles)) || !cell.has(payload, 4))
            return false;
        const uint32_t shared = cell.u32(payload);

        auto &slot = tables_[shared];
        if (!slot) {
            auto tables = std::make_unique<Tables>();
            if (!((flags & kVq) ? tables->readVq(cell, shared) : tables->readTiles(cell, shared))) {
                tables_.erase(shared);
                return false;
            }
            slot = std::move(tables);
        }
        const Tables &tables = *slot;

        out.width = width;
        out.height = height;
        out.rgba.assign(size_t(width) * height, tables.palette[0]);

        if (flags & kVq) {
            const uint32_t xBits = fieldBits((width + kBlock - 1) / kBlock), yBits = fieldBits((height + kBlock - 1) / kBlock);
            VqBits bits(cell, size_t(payload) + 4);
            for (;;) {
                const uint32_t x0 = bits.raw(xBits), y0 = bits.raw(yBits), x1 = bits.raw(xBits), y1 = bits.raw(yBits);
                if (x1 < x0 || (x1 == x0 && y1 <= y0))
                    return true;
                for (uint32_t blockY = y0; blockY < y1; ++blockY)
                    for (uint32_t blockX = x0; blockX < x1; ++blockX) {
                        const uint32_t left = blockX * kBlock, top = blockY * kBlock;
                        const uint32_t right = std::min(left + kBlock, width), bottom = std::min(top + kBlock, height);
                        VqBits reference(cell, 0);
                        VqBits *source = &bits;
                        if (bits.raw(1)) {
                            const uint32_t at = bits.raw(24);
                            reference = VqBits(cell, at >> 3);
                            reference.raw(int(at & 7));
                            reference.raw(1);
                            source = &reference;
                        }
                        for (uint32_t y = top; y < bottom; y += 2)
                            for (uint32_t x = left; x < right; x += 2) {
                                const int32_t vector = tables.vectors.decode(*source);
                                if (vector < 0 || size_t(vector) >= tables.codebook.size())
                                    return false;
                                const auto &texels = tables.codebook[size_t(vector)];
                                for (uint32_t k = 0; k < 4; ++k) {
                                    const uint32_t px = x + (k & 1), py = y + (k >> 1);
                                    if (px < right && py < bottom)
                                        out.rgba[size_t(py) * width + px] = tables.palette[texels[k]];
                                }
                            }
                    }
            }
        }

        size_t at = size_t(payload) + 4;
        for (;;) {
            if (!cell.has(at, 1))
                return false;
            if (cell.data[at] == 0xff)
                return true;
            if (!cell.has(at, 4))
                return false;
            const int x0 = int8_t(cell.data[at]), y0 = int8_t(cell.data[at + 1]);
            const int x1 = int8_t(cell.data[at + 2]), y1 = int8_t(cell.data[at + 3]);
            at += 4;
            if (x0 < 0 || y0 < 0)
                return false;
            for (int blockY = y0; blockY <= y1; ++blockY)
                for (int blockX = x0; blockX <= x1; ++blockX) {
                    if (!cell.has(at, 4))
                        return false;
                    uint8_t texels[256];
                    if (!tables.tile(cell, cell.u32(at), texels))
                        return false;
                    at += 4;
                    const uint32_t left = uint32_t(blockX) * kBlock, top = uint32_t(blockY) * kBlock;
                    for (uint32_t y = top; y < std::min(top + kBlock, height); ++y)
                        for (uint32_t x = left; x < std::min(left + kBlock, width); ++x)
                            out.rgba[size_t(y) * width + x] = tables.palette[texels[(y - top) * kBlock + (x - left)]];
                }
        }
    }

    bool Level::open(const std::string &image, const std::string &name, std::string &error)
    {
        static std::atomic<uint64_t> serial{0};
        if (!disc_.open(image, error))
            return false;
        const std::string path = "BG/DATA/" + name + ".TEX";
        uint32_t bytes = 0;
        if (!disc_.findFile(path, firstSector_, bytes, 0) && !disc_.findFile(path, firstSector_, bytes, 1)) {
            error = path + " is not on the disc";
            return false;
        }
        std::vector<uint8_t> directory(32 * bozo::libs::kSectorBytes);
        if (!disc_.read(firstSector_, 32, directory.data())) {
            error = "cannot read " + path;
            return false;
        }
        const Bytes view{directory.data(), std::min<size_t>(directory.size(), bytes)};
        for (size_t at = 4;; at += 12) {
            if (!view.has(at, 12)) {
                error = path + ": no end to its directory";
                return false;
            }
            const int32_t id = int32_t(view.u32(at));
            if (id == -1 || id == -2)
                break;
            extents_[id] = {view.u32(at + 4), view.u32(at + 8)};
            cellIds_.push_back(id);
        }
        serial_ = ++serial;
        return true;
    }

    Cell *Level::cell(int32_t id)
    {
        if (const auto found = cells_.find(id); found != cells_.end())
            return &found->second;
        const auto extent = extents_.find(id);
        if (extent == extents_.end() || extent->second.offset % bozo::libs::kSectorBytes)
            return nullptr;
        const uint32_t sectors = (extent->second.size + bozo::libs::kSectorBytes - 1) / bozo::libs::kSectorBytes;
        std::vector<uint8_t> bytes(size_t(sectors) * bozo::libs::kSectorBytes);
        if (!disc_.read(firstSector_ + extent->second.offset / bozo::libs::kSectorBytes, sectors, bytes.data()))
            return nullptr;
        bytes.resize(extent->second.size);
        return &cells_.try_emplace(id, std::move(bytes)).first->second;
    }

    std::shared_ptr<const bozo::render::Texture> Level::texture(int32_t cellId, uint32_t index)
    {
        static std::atomic<uint64_t> version{0};
        Cell *source = cell(cellId);
        auto texture = std::make_shared<bozo::render::Texture>();
        if (!source || !source->decode(index, *texture))
            return nullptr;
        texture->key = serial_ << 48 | uint64_t(uint32_t(cellId)) << 24 | index;
        texture->version = ++version;
        return texture;
    }
}
