// The particle systems drawn natively: each live particle of drawPS's lists becomes a camera-facing sprite.
#include "con_particles.h"

#include "con_textures.h"

#include "bozo/context.h"
#include "bozo/render/renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace con::particles
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x1ffffffu;
        constexpr uint32_t kDrawPS = 0x00185c38u, kDrawSingleParticles = 0x001820f8u, kDrawLines = 0x00184cd8u;
        constexpr uint32_t kLines = 0x006f4880u, kLineSize = 0x24u, kMaxLines = 0x80u;
        constexpr int32_t kLineCount = -0xff4;
        constexpr float kLineNear = 20.0f, kLineGuardX = 1400.0f, kLineGuardY = 1000.0f;
        constexpr uint32_t kView = 0x0037ef00u, kFaceRecords = 0x006ec880u;
        constexpr int32_t kFaceCount = -0xff8;
        constexpr uint16_t kRotated = 0x1, kAdditive = 0x2;
        constexpr size_t kMaxFaceTextures = 256;
        constexpr uint32_t kMainSystem = 0x0037f730u, kViewProjection = 0x0037ef30u, kLifetimes = 0x0037f720u;
        constexpr uint32_t kPages = 0x00780080u, kPageData = 0x00700080u, kTypes = 0x00780300u, kNow = 0x004869f4u;
        constexpr uint32_t kAtlasMap = 0x006ec800u, kTrailMap = 0x006ec810u;
        constexpr uint32_t kTrails = 0x00780b00u, kTrailRows = 0x0037f930u;
        constexpr uint8_t kTrailPairs = 0x2, kTrailAdditive = 0x4, kTrailMultiply = 0x8;
        constexpr float kGuardX = 800.0f, kGuardY = 400.0f;
        constexpr uint32_t kLocalMatrix = 0x100u;
        constexpr float kColourBias = 8388608.0f;

        template <typename T>
        T guest(const uint8_t *rdram, uint32_t address)
        {
            T value;
            std::memcpy(&value, rdram + (address & kRamMask), sizeof(T));
            return value;
        }

        struct Batch
        {
            bozo::render::Material material;
            std::vector<bozo::render::Vertex> vertices;
            std::shared_ptr<const bozo::render::Texture> texture;
        };

        std::vector<Batch> g_batches;
        con::textures::Mapped g_atlas, g_trail;

        using Matrix = std::array<float, 16>;

        Matrix systemMatrix(const uint8_t *rdram, uint32_t system)
        {
            Matrix view;
            std::memcpy(view.data(), rdram + kViewProjection, sizeof(view));
            if (system == kMainSystem)
                return view;
            std::array<float, 12> local;
            std::memcpy(local.data(), rdram + ((system + kLocalMatrix) & kRamMask), sizeof(local));
            Matrix out{};
            for (uint32_t row = 0; row < 4; ++row)
                for (uint32_t j = 0; j < 4; ++j) {
                    float sum = row == 3 ? view[12 + j] : 0.0f;
                    for (uint32_t k = 0; k < 3; ++k)
                        sum += local[row * 3 + k] * view[k * 4 + j];
                    out[row * 4 + j] = sum;
                }
            return out;
        }

        uint8_t channel(float value, bool biased)
        {
            const float c = std::floor(biased ? value - kColourBias : value);
            return uint8_t(std::clamp(c, 0.0f, 255.0f));
        }

        struct Particle
        {
            std::array<float, 3> origin{}, velocity{};
            float age = 0.0f;
            uint32_t type = 0;

            std::array<float, 3> at(float t, float gravity) const
            {
                return {origin[0] + velocity[0] * t, origin[1] + velocity[1] * t, origin[2] + velocity[2] * t + gravity * t * t};
            }
        };

        bool read(const uint8_t *rdram, uint32_t slot, uint16_t now, int32_t lifetime, Particle &out)
        {
            const uint16_t age = uint16_t(now - guest<uint16_t>(rdram, slot + 6));
            if (int32_t(age) > lifetime)
                return false;
            for (uint32_t k = 0; k < 3; ++k) {
                out.origin[k] = float(guest<int16_t>(rdram, slot + k * 2));
                out.velocity[k] = float(guest<int16_t>(rdram, slot + 8 + k * 2)) / 4096.0f;
            }
            out.age = float(age);
            out.type = kTypes + ((guest<uint16_t>(rdram, slot + 14) >> 2) & 31u) * 0x40u;
            return true;
        }

        std::array<float, 4> project(const Matrix &m, const std::array<float, 3> &p)
        {
            std::array<float, 4> clip{};
            for (uint32_t j = 0; j < 4; ++j)
                clip[j] = p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j] + m[12 + j];
            return clip;
        }

        uint32_t colourAt(const uint8_t *rdram, uint32_t type, float t)
        {
            uint32_t colour = 0;
            for (uint32_t c = 0; c < 4; ++c)
                colour |= uint32_t(channel(guest<float>(rdram, type + (8 + c) * 4) + guest<float>(rdram, type + (12 + c) * 4) * t, true)) << (c * 8);
            return colour;
        }

        void sprite(const uint8_t *rdram, const Matrix &m, const Particle &p, float width, float height,
                    std::vector<bozo::render::Vertex> &out)
        {
            const auto field = [&](uint32_t i) { return guest<float>(rdram, p.type + i * 4); };
            const auto clip = project(m, p.at(p.age, field(2)));
            if (clip[3] <= 1.0f)
                return;
            const float size = field(7) + field(3) * p.age;
            const uint32_t colour = colourAt(rdram, p.type, p.age);
            if (size <= 0.0f || !(colour >> 24))
                return;
            const float iw = 1.0f / clip[3];
            const float cx = clip[0] * iw, cy = clip[1] * iw, cz = clip[2] * iw, hx = 2.0f * size * iw, hy = size * iw;
            const float u0 = float(guest<int32_t>(rdram, p.type)) / (16.0f * width),
                        v0 = float(guest<int32_t>(rdram, p.type + 4)) / (16.0f * height),
                        u1 = float(guest<int32_t>(rdram, p.type + 16)) / (16.0f * width),
                        v1 = float(guest<int32_t>(rdram, p.type + 20)) / (16.0f * height);
            const auto corner = [&](float sx, float sy, float u, float v) {
                bozo::render::Vertex vertex;
                vertex.x = cx + sx * hx;
                vertex.y = cy + sy * hy;
                vertex.z = cz;
                vertex.u = u;
                vertex.v = v;
                vertex.rgba = colour;
                return vertex;
            };
            const auto a = corner(-1, -1, u0, v0), b = corner(1, -1, u1, v0), c = corner(1, 1, u1, v1), d = corner(-1, 1, u0, v1);
            out.insert(out.end(), {a, b, c, a, c, d});
        }

        void segment(const std::array<float, 3> &head, const std::array<float, 3> &tail, uint32_t headColour, uint32_t tailColour,
                     std::vector<bozo::render::Vertex> &out)
        {
            const float dx = (tail[0] - head[0]) * 0.5f, dy = tail[1] - head[1], length = std::sqrt(dx * dx + dy * dy);
            const float nx = length > 0.0f ? -dy / length : 0.0f, ny = length > 0.0f ? dx / length : 1.0f;
            const auto end = [&](const std::array<float, 3> &at, uint32_t colour, float side) {
                bozo::render::Vertex vertex;
                vertex.x = at[0] + side * nx;
                vertex.y = at[1] + side * ny * 0.5f;
                vertex.z = at[2];
                vertex.rgba = colour;
                return vertex;
            };
            const auto a = end(head, headColour, -1), b = end(head, headColour, 1), c = end(tail, tailColour, 1), d = end(tail, tailColour, -1);
            out.insert(out.end(), {a, b, c, a, c, d});
        }

        void line(const uint8_t *rdram, const Matrix &m, const Particle &p, std::vector<bozo::render::Vertex> &out)
        {
            const float gravity = guest<float>(rdram, p.type), lag = guest<float>(rdram, p.type + 12);
            const auto head = project(m, p.at(p.age, gravity)), tail = project(m, p.at(p.age - lag, gravity));
            if (head[3] <= 1.0f || tail[3] <= 1.0f)
                return;
            const uint32_t colour = colourAt(rdram, p.type, p.age);
            if (!(colour >> 24))
                return;
            segment({head[0] / head[3], head[1] / head[3], head[2] / head[3]}, {tail[0] / tail[3], tail[1] / tail[3], tail[2] / tail[3]},
                    colour, colour, out);
        }

        template <typename Emit>
        void drawList(const uint8_t *rdram, uint32_t list, uint16_t now, int32_t lifetime, const Emit &emit)
        {
            const uint32_t head = guest<uint32_t>(rdram, list), tail = guest<uint32_t>(rdram, list + 4);
            const uint32_t first = guest<uint32_t>(rdram, list + 8), count = guest<uint32_t>(rdram, list + 12);
            uint32_t page = head;
            Particle particle;
            for (uint32_t guard = 0; page && guard < 4096; ++guard) {
                const uint32_t begin = page == head ? first : 0, end = page == tail ? std::min(count, 0x100u) : 0x100u;
                const uint32_t data = kPageData + ((page - kPages) << 10);
                for (uint32_t n = begin; n < end; ++n)
                    if ((n & 31u) && (n & 31u) != 31u && read(rdram, data + n * 16u, now, lifetime, particle))
                        emit(particle);
                if (page == tail)
                    break;
                page = guest<uint32_t>(rdram, page);
            }
        }

        bozo::render::Material untexturedLines()
        {
            bozo::render::Material material;
            material.tex0 = 0;
            material.textured = false;
            material.directColour = true;
            material.blend = bozo::render::Material::Blend::Additive;
            material.depthWrite = false;
            material.nearest = true;
            return material;
        }

        // P_Line's lines (web strands and the like): pulled towards the camera by their width, kept as one-pixel lines.
        void drawLines(uint8_t *rdram, R5900Context *ctx, Runtime *)
        {
            ctx->pc = getRegU32(ctx, 31);
            const uint32_t count = guest<uint32_t>(rdram, getRegU32(ctx, 28) + uint32_t(kLineCount));
            if (!count)
                return;
            Matrix m;
            std::memcpy(m.data(), rdram + kViewProjection, sizeof(m));
            const std::array<float, 3> forward{guest<float>(rdram, kView + 0x8), guest<float>(rdram, kView + 0x14), guest<float>(rdram, kView + 0x20)};
            Batch batch;
            batch.material = untexturedLines();
            for (uint32_t i = 0; i < count && i < kMaxLines; ++i) {
                const uint32_t record = kLines + i * kLineSize;
                const float width = guest<float>(rdram, record + 0x20);
                std::array<std::array<float, 3>, 2> ends;
                bool visible = true;
                for (uint32_t e = 0; e < 2 && visible; ++e) {
                    std::array<float, 3> p;
                    for (uint32_t k = 0; k < 3; ++k)
                        p[k] = guest<float>(rdram, record + e * 0xc + k * 4) - forward[k] * width;
                    const auto clip = project(m, p);
                    const float w = clip[3] + width;
                    visible = !(w < kLineNear) && !(w * kLineGuardX < std::fabs(clip[0])) && !(w * kLineGuardY < std::fabs(clip[1]));
                    ends[e] = {clip[0] / w, clip[1] / w, clip[2] / w};
                }
                if (visible)
                    segment(ends[0], ends[1], guest<uint32_t>(rdram, record + 0x18), guest<uint32_t>(rdram, record + 0x1c), batch.vertices);
            }
            if (!batch.vertices.empty())
                g_batches.push_back(std::move(batch));
            const uint32_t zero = 0;
            std::memcpy(rdram + ((getRegU32(ctx, 28) + uint32_t(kLineCount)) & kRamMask), &zero, sizeof(zero));
        }

        void drawPS(uint8_t *rdram, R5900Context *ctx, Runtime *)
        {
            ctx->pc = getRegU32(ctx, 31);
            const auto *texture = con::textures::mapped(rdram, getRegU32(ctx, 28), kAtlasMap, g_atlas);
            if (!texture)
                return;
            const uint32_t system = getRegU32(ctx, 5);
            const Matrix m = systemMatrix(rdram, system);
            const uint16_t now = guest<uint16_t>(rdram, kNow);
            const float width = float(texture->width), height = float(texture->height);
            for (uint32_t pass = 0; pass < 3; ++pass) {
                Batch batch;
                if (pass == 2) {
                    batch.material = untexturedLines();
                } else {
                    batch.material.tex0 = texture->key;
                    batch.material.blend = pass ? bozo::render::Material::Blend::Additive : bozo::render::Material::Blend::Alpha;
                    batch.material.depthWrite = false;
                    batch.material.nearest = true;
                }
                for (uint32_t k = 0; k < 4; ++k)
                    drawList(rdram, system + pass * 0x40u + k * 0x10u, now, guest<int32_t>(rdram, kLifetimes + k * 4u),
                             [&](const Particle &p) {
                                 if (pass == 2)
                                     line(rdram, m, p, batch.vertices);
                                 else
                                     sprite(rdram, m, p, width, height, batch.vertices);
                             });
                if (pass != 2)
                    batch.texture = g_atlas.texture;
                if (!batch.vertices.empty())
                    g_batches.push_back(std::move(batch));
            }
        }

        std::unordered_map<uint64_t, std::shared_ptr<const bozo::render::Texture>> g_faceTextures;

        std::shared_ptr<const bozo::render::Texture> faceTexture(const uint8_t *rdram, uint32_t lump)
        {
            const uint64_t key = con::textures::lumpKey(rdram, lump);
            if (const auto found = g_faceTextures.find(key); found != g_faceTextures.end())
                return found->second;
            auto texture = std::make_shared<bozo::render::Texture>();
            if (!con::textures::decodeLump(rdram, lump, *texture) || !texture->width || !texture->height)
                return nullptr;
            texture->key = key;
            texture->version = 1;
            if (g_faceTextures.size() >= kMaxFaceTextures)
                g_faceTextures.clear();
            g_faceTextures[key] = texture;
            return texture;
        }

        void faceSprite(const uint8_t *rdram, uint32_t record, const bozo::render::Texture &texture,
                        std::vector<bozo::render::Vertex> &out)
        {
            Matrix m;
            std::memcpy(m.data(), rdram + kViewProjection, sizeof(m));
            const float radius = guest<float>(rdram, record + 0x10), size = guest<float>(rdram, record + 0xc);
            std::array<float, 3> p{};
            for (uint32_t k = 0; k < 3; ++k)
                p[k] = guest<float>(rdram, record + k * 4) - guest<float>(rdram, kView + 8 + k * 12) * radius;
            const auto clip = project(m, p);
            const float depth = clip[3] + radius;
            if (depth < 20.0f || std::fabs(clip[0]) > depth * 700.0f || std::fabs(clip[1]) > depth * 300.0f)
                return;
            const float x = clip[0] / depth, y = clip[1] / depth, z = clip[2] / clip[3], half = size / depth;
            const uint32_t colour = guest<uint32_t>(rdram, record + 0x14);
            const float u0 = 1.0f / float(texture.width), v0 = 1.0f / float(texture.height);
            const float u1 = float(texture.width - 1) / float(texture.width), v1 = float(texture.height - 1) / float(texture.height);
            const auto vertex = [&](float vx, float vy, float u, float v) {
                bozo::render::Vertex out;
                out.x = vx;
                out.y = vy;
                out.z = z;
                out.u = u;
                out.v = v;
                out.rgba = colour;
                return out;
            };
            if (guest<uint16_t>(rdram, record + 0x1e) & kRotated) {
                const float angle = float(guest<int16_t>(rdram, record + 0x1c)) * (6.28318531f / 65536.0f);
                const float c = std::cos(angle) * half, s = std::sin(angle) * half;
                const auto a = vertex(x - 2 * c, y - s, u0, v0), b = vertex(x + 2 * s, y - c, u1, v0),
                           d = vertex(x - 2 * s, y + c, u0, v1), e = vertex(x + 2 * c, y + s, u1, v1);
                out.insert(out.end(), {a, b, d, b, d, e});
                return;
            }
            const auto a = vertex(x - 2 * half, y - half, u0, v0), b = vertex(x + 2 * half, y - half, u1, v0),
                       c = vertex(x + 2 * half, y + half, u1, v1), d = vertex(x - 2 * half, y + half, u0, v1);
            out.insert(out.end(), {a, b, c, a, c, d});
        }

        void drawSingleParticles(uint8_t *rdram, R5900Context *ctx, Runtime *)
        {
            ctx->pc = getRegU32(ctx, 31);
            const uint32_t pass = getRegU32(ctx, 4);
            const int32_t count = int32_t(guest<uint32_t>(rdram, getRegU32(ctx, 28) + uint32_t(kFaceCount)));
            std::vector<std::pair<uint64_t, size_t>> open;
            for (int32_t i = 0; i < count && i < 0x400; ++i) {
                const uint32_t record = kFaceRecords + uint32_t(i) * 0x20u;
                const uint32_t lump = guest<uint32_t>(rdram, record + 0x18);
                const uint16_t flags = guest<uint16_t>(rdram, record + 0x1e);
                if (!lump || (flags >> 2 & 1u) != pass)
                    continue;
                std::memset(rdram + ((record + 0x18) & kRamMask), 0, 4);
                const auto texture = faceTexture(rdram, lump);
                if (!texture)
                    continue;
                const uint64_t group = uint64_t(lump) << 16 | flags;
                auto found = std::find_if(open.begin(), open.end(), [&](const auto &o) { return o.first == group; });
                if (found == open.end()) {
                    Batch batch;
                    batch.texture = texture;
                    batch.material.tex0 = texture->key;
                    batch.material.blend = flags & kAdditive ? bozo::render::Material::Blend::Additive : bozo::render::Material::Blend::Alpha;
                    batch.material.depthWrite = false;
                    g_batches.push_back(std::move(batch));
                    found = open.insert(open.end(), {group, g_batches.size() - 1});
                }
                faceSprite(rdram, record, *texture, g_batches[found->second].vertices);
            }
            for (auto it = open.rbegin(); it != open.rend(); ++it)
                if (g_batches[it->second].vertices.empty())
                    g_batches.erase(g_batches.begin() + std::ptrdiff_t(it->second));
        }
    }

    void drawTrails(const uint8_t *rdram, const R5900Context *ctx)
    {
        const auto *texture = con::textures::mapped(rdram, getRegU32(ctx, 28), kTrailMap, g_trail);
        if (!texture)
            return;
        Matrix m;
        std::memcpy(m.data(), rdram + kViewProjection, sizeof(m));
        const float texelU = 1.0f / float(texture->width), texelV = 1.0f / float(texture->height);
        struct Edge
        {
            std::array<float, 4> clip{};
            bool visible = false;
            float u = 0.0f;
            uint32_t alpha = 0;
        };
        for (uint32_t slot = 0; slot < 64; ++slot) {
            const uint32_t e = kTrails + slot * 0x410u;
            if (!guest<int16_t>(rdram, e + 0x400))
                continue;
            const uint32_t read = guest<uint8_t>(rdram, e + 0x404), write = guest<uint8_t>(rdram, e + 0x405);
            if (read == write)
                continue;
            const uint8_t flags = guest<uint8_t>(rdram, e + 0x407);
            const bool pairs = flags & kTrailPairs;
            const int32_t row = guest<int8_t>(rdram, e + 0x406);
            const float lifetime = float(guest<int16_t>(rdram, e + 0x408)), clock = float(guest<int16_t>(rdram, e + 0x402));
            if (lifetime <= 0.0f)
                continue;
            const float top = guest<float>(rdram, kTrailRows + uint32_t(row) * 4u);
            const float v0 = (top + 0.5f) * texelV, v1 = (guest<float>(rdram, kTrailRows + uint32_t(row + 1) * 4u) - 0.5f) * texelV;
            const float width = float(guest<uint8_t>(rdram, e + 0x40f)) * 40.0f;
            const uint32_t colour = guest<uint32_t>(rdram, e + 0x40a);
            const auto point = [&](uint32_t index) {
                const uint32_t at = e + (index & 0x7fu) * 8u;
                std::array<float, 3> p{};
                for (uint32_t k = 0; k < 3; ++k)
                    p[k] = float(guest<int16_t>(rdram, at + k * 2)) * 0.5f;
                return std::make_pair(project(m, p), guest<int16_t>(rdram, at + 6));
            };
            const auto u = [&](int16_t stamp) { return (clock - float(stamp)) * (128.0f / lifetime) * texelU; };
            Batch batch;
            batch.texture = g_trail.texture;
            batch.material.tex0 = texture->key;
            batch.material.blend = flags & kTrailMultiply ? bozo::render::Material::Blend::DestinationBySource
                                   : flags & kTrailAdditive ? bozo::render::Material::Blend::Additive
                                                            : bozo::render::Material::Blend::Alpha;
            batch.material.depthWrite = false;
            const uint32_t start = read & 0xfeu, count = (write - start) & 0x7fu;
            std::vector<std::array<bozo::render::Vertex, 2>> edges;
            std::vector<bool> drawn;
            const auto vertex = [&](const std::array<float, 4> &c, float ox, float oy, float tu, float tv, uint32_t rgba) {
                bozo::render::Vertex out;
                out.x = c[0] / c[3] + ox;
                out.y = c[1] / c[3] + oy;
                out.z = c[2] / c[3];
                out.u = tu;
                out.v = tv;
                out.rgba = rgba;
                return out;
            };
            if (pairs) {
                for (uint32_t k = 0; k + 1 < count; k += 2) {
                    const auto [base, stamp] = point(start + k);
                    const auto [tip, alpha] = point(start + k + 1);
                    const bool visible = base[3] > 1.0f && tip[3] > 1.0f;
                    const uint32_t rgba = (colour & 0x00ffffffu) | uint32_t(uint16_t(alpha) & 0xffu) << 24;
                    edges.push_back(visible ? std::array<bozo::render::Vertex, 2>{vertex(base, 0, 0, u(stamp), v0, rgba),
                                                                                   vertex(tip, 0, 0, u(stamp), v1, rgba)}
                                            : std::array<bozo::render::Vertex, 2>{});
                    drawn.push_back(visible);
                }
            } else {
                std::vector<std::pair<std::array<float, 4>, int16_t>> points;
                for (uint32_t k = 0; k < count; ++k)
                    points.push_back(point(start + k));
                for (uint32_t k = 0; k < count; ++k) {
                    const auto &[c, stamp] = points[k];
                    const auto &other = points[k ? k - 1 : std::min(k + 1, count - 1)].first;
                    const bool visible = c[3] > 1.0f && other[3] > 1.0f;
                    if (!visible) {
                        edges.push_back({});
                        drawn.push_back(false);
                        continue;
                    }
                    float dx = (c[0] / c[3] - other[0] / other[3]) / kGuardX, dy = (c[1] / c[3] - other[1] / other[3]) / kGuardY;
                    if (!k) {
                        dx = -dx;
                        dy = -dy;
                    }
                    const float length = std::sqrt(dx * dx + dy * dy);
                    const float scale = length > 0.0f ? width / (c[3] * length) : 0.0f;
                    const float ox = 2.0f * dy * scale, oy = -dx * scale;
                    edges.push_back({vertex(c, ox, oy, u(stamp), v0, colour), vertex(c, -ox, -oy, u(stamp), v1, colour)});
                    drawn.push_back(true);
                }
            }
            for (size_t k = 1; k < edges.size(); ++k)
                if (drawn[k] && drawn[k - 1]) {
                    const auto &a = edges[k - 1], &b = edges[k];
                    batch.vertices.insert(batch.vertices.end(), {a[0], a[1], b[0], a[1], b[0], b[1]});
                }
            if (!batch.vertices.empty())
                g_batches.push_back(std::move(batch));
        }
    }

    void install(bozo::port::Registry &runtime)
    {
        runtime.registerFunction(kDrawPS, &drawPS);
        runtime.registerFunction(kDrawSingleParticles, &drawSingleParticles);
        runtime.registerFunction(kDrawLines, &drawLines);
    }

    void submit()
    {
        static constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        auto &renderer = bozo::render::Renderer::instance();
        for (const Batch &batch : g_batches)
            renderer.draw(batch.material, kIdentity, batch.vertices.data(), uint32_t(batch.vertices.size()), batch.texture);
    }

    size_t pending() { return g_batches.size(); }

    void clear() { g_batches.clear(); }
}
