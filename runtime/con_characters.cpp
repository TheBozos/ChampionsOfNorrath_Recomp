// Characters and other posed models drawn natively: each model's meshes decoded once into rest positions with their joint
// weights, skinned on the CPU every draw by the pose the game built, drawn with the model-to-screen matrix it built.
#include "con_characters.h"

#include "con_lights.h"
#include "con_world.h"
#include "con_textures.h"

#include "bozo/context.h"
#include "bozo/render/renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace con::characters
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x1ffffffu, kMaxJoints = 64, kEnvironmentProgram = 0x44;
        // matMakeCharViewMatrix scales the depth row by 16 x 2^46 over matMakeViewMatrix's.
        constexpr float kCharacterDepth = 1.0f / 1125899906842624.0f;

        template <typename T>
        T guest(const uint8_t *rdram, uint32_t address)
        {
            T value;
            std::memcpy(&value, rdram + (address & kRamMask), sizeof(T));
            return value;
        }

        struct Skinned
        {
            std::array<float, 3> position{};
            std::array<float, 3> normal{};
            std::array<uint8_t, 4> joint{};
            std::array<float, 4> weight{};
            uint8_t joints = 0;
        };

        struct Corner
        {
            uint32_t vertex = 0;
            float u = 0, v = 0;
            bool environment = false;
        };

        struct Model
        {
            std::vector<Skinned> vertices;
            std::vector<Corner> corners;
            std::vector<uint32_t> meshOfCorner;
        };

        struct Unpack
        {
            uint32_t command = 0, count = 0, data = 0;
        };

        // One packet: rest positions, weights, then the strip the slot rows build, as triangles of vertex indices.
        void decodePacket(const uint8_t *rdram, const std::vector<Unpack> &u, const Unpack &uv, uint32_t mesh, bool environment,
                          Model &model)
        {
            if (u.size() < 6)
                return;
            const Unpack &positions = u[0], &rows = u[2];
            const Unpack *weights = nullptr, *copies = nullptr, *tags = nullptr;
            for (size_t i = 3; i < u.size(); ++i) {
                const uint32_t kind = u[i].command & 0xf;
                if (kind == 0xe && !weights && !copies)
                    weights = &u[i];
                else if (kind == 0xd && !copies)
                    copies = &u[i];
                else if (kind == 0xc && copies && !tags)
                    tags = &u[i];
            }
            if (!copies || !tags)
                return;
            const uint32_t count = positions.count, base = uint32_t(model.vertices.size());
            const Unpack &normals = u[1];
            for (uint32_t i = 0; i < count; ++i) {
                Skinned s;
                for (int k = 0; k < 3; ++k)
                    s.position[size_t(k)] = float(guest<int16_t>(rdram, positions.data + i * 6 + uint32_t(k) * 2)) / 16.0f;
                if (i < normals.count)
                    for (int k = 0; k < 3; ++k)
                        s.normal[size_t(k)] = float(guest<int8_t>(rdram, normals.data + i * 3 + uint32_t(k))) / 127.0f;
                model.vertices.push_back(s);
            }
            if (weights) {
                uint32_t entry = 0;
                for (uint32_t i = 0; i < count && entry < weights->count;) {
                    const uint32_t at = weights->data + entry * 4;
                    const uint8_t j1 = guest<uint8_t>(rdram, at), w1 = guest<uint8_t>(rdram, at + 1);
                    const uint8_t j2 = guest<uint8_t>(rdram, at + 2), w2 = guest<uint8_t>(rdram, at + 3);
                    ++entry;
                    if (j2 == 0xff) {
                        for (uint32_t n = 0; n < w2 && i < count; ++n, ++i) {
                            Skinned &s = model.vertices[base + i];
                            s.joint[0] = uint8_t(j1 / 4);
                            s.weight[0] = 1.0f;
                            s.joints = 1;
                        }
                        continue;
                    }
                    Skinned &s = model.vertices[base + i];
                    s.joint[0] = uint8_t(j1 / 4);
                    s.weight[0] = float(w1) / 255.0f;
                    s.joint[1] = uint8_t(j2 / 4);
                    s.weight[1] = float(w2) / 255.0f;
                    s.joints = 2;
                    if (w1 + w2 != 255 && entry < weights->count) {
                        const uint32_t more = weights->data + entry * 4;
                        ++entry;
                        s.joint[2] = uint8_t(guest<uint8_t>(rdram, more) / 4);
                        s.weight[2] = float(guest<uint8_t>(rdram, more + 1)) / 255.0f;
                        s.joints = 3;
                        if (guest<uint8_t>(rdram, more + 2) != 0xff) {
                            s.joint[3] = uint8_t(guest<uint8_t>(rdram, more + 2) / 4);
                            s.weight[3] = float(guest<uint8_t>(rdram, more + 3)) / 255.0f;
                            s.joints = 4;
                        }
                    }
                    ++i;
                }
            }
            const uint32_t tag = guest<uint32_t>(rdram, tags->data), prim = guest<uint32_t>(rdram, tags->data + 4);
            const uint32_t strip = tag & 0x7fff, registers = prim >> 28;
            if (!registers || strip > uv.count || rows.count < count + 2)
                return;
            std::vector<int32_t> slot(strip, -1);
            std::vector<uint8_t> restart(strip, 0);
            const auto copy = [&](uint32_t source, uint32_t target) {
                if (source == target)
                    return;
                const uint32_t from = source / registers, to = (target & 0x7fff) / registers;
                if (from < strip && to < strip) {
                    slot[to] = slot[from];
                    restart[to] = (target & 0x8000) != 0;
                }
            };
            for (uint32_t r = 0; r < count; ++r) {
                const uint32_t row = rows.data + (r + 2) * 6;
                const uint16_t a = guest<uint16_t>(rdram, row);
                copy(guest<uint16_t>(rdram, row + 2), guest<uint16_t>(rdram, row + 4));
                const uint32_t to = (a & 0x7fffu) / registers;
                if (to < strip) {
                    slot[to] = int32_t(r);
                    restart[to] = (a & 0x8000) != 0;
                }
            }
            for (uint32_t r = 1; r < copies->count; ++r) {
                const uint32_t row = copies->data + r * 8;
                copy(guest<uint16_t>(rdram, row), guest<uint16_t>(rdram, row + 2));
                copy(guest<uint16_t>(rdram, row + 4), guest<uint16_t>(rdram, row + 6));
            }
            for (uint32_t p = 2; p < strip; ++p) {
                if (restart[p] || slot[p] < 0 || slot[p - 1] < 0 || slot[p - 2] < 0)
                    continue;
                if (slot[p] == slot[p - 1] || slot[p - 1] == slot[p - 2] || slot[p] == slot[p - 2])
                    continue;
                for (const uint32_t q : {p - 2, p - 1, p}) {
                    Corner c;
                    c.vertex = base + uint32_t(slot[q]);
                    c.u = float(guest<uint16_t>(rdram, uv.data + q * 4)) / 16.0f;
                    c.v = float(guest<uint16_t>(rdram, uv.data + q * 4 + 2)) / 16.0f;
                    c.environment = environment;
                    model.corners.push_back(c);
                    model.meshOfCorner.push_back(mesh);
                }
            }
        }

        void decodeMesh(const uint8_t *rdram, uint32_t at, uint32_t end, uint32_t mesh, Model &model)
        {
            std::vector<Unpack> unpacks;
            bool called = false;
            uint32_t program = 0;
            while (at + 4 <= end) {
                const uint32_t code = guest<uint32_t>(rdram, at);
                at += 4;
                const uint32_t command = (code >> 24) & 0x7f, number = (code >> 16) & 0xff, immediate = code & 0xffff;
                if (command >= 0x60) {
                    const uint32_t components = ((command >> 2) & 3) + 1, bits = 32u >> (command & 3);
                    const uint32_t count = number ? number : 256;
                    const Unpack block{command, count, at};
                    at += ((components * bits * count + 31) / 32) * 4;
                    if (called) {
                        decodePacket(rdram, unpacks, block, mesh, program == kEnvironmentProgram, model);
                        unpacks.clear();
                        called = false;
                    } else {
                        unpacks.push_back(block);
                    }
                } else if (command == 0x14 || command == 0x15 || command == 0x17) {
                    called = true;
                    program = immediate;
                } else if (command == 0x50 || command == 0x51) {
                    at += (immediate ? immediate : 0x10000u) * 16;
                } else if (command == 0x20) {
                    at += 4;
                } else if (command == 0x30 || command == 0x31) {
                    at += 16;
                } else if (command == 0x04) {
                    unpacks.clear();
                    called = false;
                }
            }
        }

        std::unique_ptr<Model> build(const uint8_t *rdram, uint32_t vif)
        {
            auto model = std::make_unique<Model>();
            const int32_t meshes = guest<int8_t>(rdram, vif + 0x12);
            const auto meshAt = [&](int32_t i) {
                const uint32_t offset = guest<uint32_t>(rdram, vif + 0x28 + uint32_t(i) * 4);
                return offset < vif ? vif + offset : offset;
            };
            for (int32_t i = 0; i < meshes && i < 64; ++i)
                decodeMesh(rdram, meshAt(i), meshAt(i + 1), uint32_t(i), *model);
            return model;
        }

        struct Joint
        {
            float r[3][3];
            float t[3];
        };

        struct Draw
        {
            std::vector<bozo::render::Vertex> vertices;
            std::shared_ptr<const bozo::render::Texture> texture;
            bozo::render::Material material;
            std::array<float, 16> matrix{};
        };

        std::unordered_map<uint64_t, std::unique_ptr<Model>> g_models;
        std::vector<Draw> g_draws;
        uint32_t g_generation = 0;

        struct Painted
        {
            std::shared_ptr<const bozo::render::Texture> texture;
            bozo::render::Material material;
        };

        std::unordered_map<uint64_t, Painted> g_painted;

        const Painted *textureAt(const uint8_t *rdram, uint32_t tex, uint64_t alpha)
        {
            uint64_t key = (0x43484152ull << 32 | tex) ^ alpha * 0x9e3779b97f4a7c15ull;
            for (uint32_t i = 0; i < 16; ++i)
                key = (key ^ rdram[(tex + 0x80 + i * 32) & kRamMask]) * 0x100000001b3ull;
            if (const auto found = g_painted.find(key); found != g_painted.end())
                return &found->second;
            bozo::render::Texture source;
            if (!con::textures::decodeLump(rdram, tex, source) || !source.width || !source.height)
                return nullptr;
            Painted painted;
            auto texture = std::make_shared<bozo::render::Texture>(std::move(source));
            con::textures::modelBlend(alpha, *texture, painted.material);
            texture->key = key;
            texture->version = 1;
            painted.material.tex0 = key;
            painted.texture = texture;
            if (g_painted.size() > 2048)
                g_painted.clear();
            return &(g_painted[key] = std::move(painted));
        }
    }

    namespace
    {
        using Joints = std::array<Joint, kMaxJoints>;
        using con::lights::Lights;
        using con::lights::Vec3;

        struct Parent
        {
            Joints joints{};
            std::array<float, 16> matrix{};
            Lights lights;
        };

        std::unordered_map<uint32_t, Parent> g_parents;

        Joints identity()
        {
            Joints joints{};
            for (auto &j : joints)
                j.r[0][0] = j.r[1][1] = j.r[2][2] = 1.0f;
            return joints;
        }

        Joints readPose(const uint8_t *rdram, uint32_t state)
        {
            Joints joints = identity();
            const uint32_t pose = state ? guest<uint32_t>(rdram, state) : 0;
            if (!pose)
                return joints;
            const uint32_t count = std::min<uint32_t>(uint32_t(std::max<int16_t>(guest<int16_t>(rdram, pose + 8), 0)), kMaxJoints);
            for (uint32_t j = 0; j < count; ++j) {
                const uint32_t at = pose + 0x10 + j * 0x18;
                for (uint32_t row = 0; row < 3; ++row) {
                    for (uint32_t c = 0; c < 3; ++c)
                        joints[j].r[row][c] = float(guest<int16_t>(rdram, at + row * 8 + c * 2)) / 4096.0f;
                    joints[j].t[row] = float(guest<int16_t>(rdram, at + row * 8 + 6)) / 64.0f;
                }
            }
            return joints;
        }

        void emit(const uint8_t *rdram, uint32_t gp, uint32_t vif, uint32_t tex, uint64_t shown, const Joints &joints,
                  const std::array<float, 16> &matrix, const Lights &lights);
    }

    void draw(const uint8_t *rdram, const R5900Context *ctx)
    {
        const uint32_t vif = getRegU32(ctx, 18), tex = getRegU32(ctx, 30), state = getRegU32(ctx, 19);
        Parent &parent = g_parents[vif];
        parent.joints = readPose(rdram, state);
        const uint32_t stack = getRegU32(ctx, 29);
        std::memcpy(parent.matrix.data(), rdram + ((stack + 0x20u) & kRamMask), sizeof(parent.matrix));
        for (const uint32_t i : {2u, 6u, 10u, 14u})
            parent.matrix[i] *= kCharacterDepth;
        parent.lights = con::lights::forCharacter(rdram, getRegU32(ctx, 28),
                                 {guest<float>(rdram, stack), guest<float>(rdram, stack + 4), guest<float>(rdram, stack + 8)});
        emit(rdram, getRegU32(ctx, 28), vif, tex, getRegU64(ctx, 21), parent.joints, parent.matrix, parent.lights);
    }

    // A part worn or held by the model drawn last with that parent: worn parts (attach slot -1) take the parent's joints,
    // held ones its joint matrix times the parent's attach record, for every vertex.
    void drawAttachment(const uint8_t *rdram, const R5900Context *ctx)
    {
        const uint32_t part = getRegU32(ctx, 16), tex = getRegU32(ctx, 21), parentVif = getRegU32(ctx, 6);
        const auto found = g_parents.find(parentVif);
        if (!part || found == g_parents.end())
            return;
        const Parent &parent = found->second;
        const int8_t slot = guest<int8_t>(rdram, part + 0x13);
        if (slot < 0) {
            emit(rdram, getRegU32(ctx, 28), part, tex, getRegU64(ctx, 18), parent.joints, parent.matrix, parent.lights);
            return;
        }
        const uint32_t table = parentVif + uint32_t(guest<uint8_t>(rdram, parentVif + 0x22)) * 16u + uint32_t(slot) * 0x34u;
        const int32_t jointIndex = std::clamp<int32_t>(guest<int32_t>(rdram, table), 0, int32_t(kMaxJoints) - 1);
        const Joint &j = parent.joints[size_t(jointIndex)];
        Joint attached{};
        float a[3][3], o[3];
        for (uint32_t row = 0; row < 3; ++row) {
            for (uint32_t c = 0; c < 3; ++c)
                a[row][c] = guest<float>(rdram, table + 4 + (row * 3 + c) * 4);
            o[row] = guest<float>(rdram, table + 0x28 + row * 4);
        }
        for (int row = 0; row < 3; ++row) {
            for (int c = 0; c < 3; ++c)
                attached.r[row][c] = j.r[row][0] * a[0][c] + j.r[row][1] * a[1][c] + j.r[row][2] * a[2][c];
            attached.t[row] = j.r[row][0] * o[0] + j.r[row][1] * o[1] + j.r[row][2] * o[2] + j.t[row];
        }
        Joints joints;
        joints.fill(attached);
        emit(rdram, getRegU32(ctx, 28), part, tex, getRegU64(ctx, 18), joints, parent.matrix, parent.lights);
    }

    namespace
    {
    void emit(const uint8_t *rdram, uint32_t gp, uint32_t vif, uint32_t tex, uint64_t shown, const Joints &joints,
              const std::array<float, 16> &matrix, const Lights &lights)
    {
        if (!vif || !tex)
            return;
        uint64_t identity = 0xcbf29ce484222325ull ^ vif;
        for (uint32_t i = 0; i < 0x30; ++i)
            identity = (identity ^ rdram[(vif + 0x10 + i) & kRamMask]) * 0x100000001b3ull;
        auto &model = g_models[identity];
        if (!model)
            model = build(rdram, vif);
        if (model->corners.empty())
            return;
        const Painted *painted = textureAt(rdram, tex, guest<uint64_t>(rdram, vif + 0x18));
        if (!painted)
            return;
        const auto &texture = painted->texture;

        Vec3 length{};
        for (uint32_t k = 0; k < 3; ++k) {
            const float l = std::sqrt(matrix[k] * matrix[k] + matrix[4 + k] * matrix[4 + k] + matrix[8 + k] * matrix[8 + k]);
            length[k] = l > 0.0f ? 0.99f / l : 0.0f;
        }
        std::vector<Vec3> posed(model->vertices.size());
        std::vector<uint32_t> lit(model->vertices.size());
        std::vector<std::array<float, 2>> reflected(model->vertices.size());
        for (size_t i = 0; i < model->vertices.size(); ++i) {
            const Skinned &s = model->vertices[i];
            Vec3 out{}, normal{};
            const uint8_t used = s.joints ? s.joints : 1;
            for (uint8_t k = 0; k < used; ++k) {
                const Joint &j = joints[std::min<uint32_t>(s.joint[k], kMaxJoints - 1)];
                const float w = s.joints ? s.weight[k] : 1.0f;
                for (int row = 0; row < 3; ++row) {
                    out[size_t(row)] += w * (j.r[row][0] * s.position[0] + j.r[row][1] * s.position[1] +
                                             j.r[row][2] * s.position[2] + j.t[row]);
                    normal[size_t(row)] += w * (j.r[row][0] * s.normal[0] + j.r[row][1] * s.normal[1] + j.r[row][2] * s.normal[2]);
                }
            }
            posed[i] = out;
            Vec3 camera{};
            for (uint32_t k = 0; k < 3; ++k)
                camera[k] = (matrix[k] * normal[0] + matrix[4 + k] * normal[1] + matrix[8 + k] * normal[2]) * length[k];
            lit[i] = con::lights::shade(lights, camera);
            reflected[i] = {192.0f + 64.0f * camera[0], 192.0f + 64.0f * camera[1]};
        }

        Draw d;
        d.texture = texture;
        d.material = painted->material;
        d.matrix = matrix;
        d.vertices.reserve(model->corners.size());
        const float width = float(texture->width), height = float(texture->height);
        for (size_t i = 0; i < model->corners.size(); i += 3) {
            if (!(shown >> model->meshOfCorner[i] & 1))
                continue;
            for (size_t k = 0; k < 3; ++k) {
                const Corner &c = model->corners[i + k];
                bozo::render::Vertex v;
                v.x = posed[c.vertex][0];
                v.y = posed[c.vertex][1];
                v.z = posed[c.vertex][2];
                v.u = c.u / width;
                v.v = c.v / height;
                v.rgba = lit[c.vertex];
                d.vertices.push_back(v);
            }
        }
        const auto environment = con::world::environment(rdram, gp);
        Draw e;
        if (environment) {
            e.texture = environment;
            e.material.tex0 = environment->key;
            e.material.gsBlend = true;
            e.material.blendA = 0;
            e.material.blendB = 1;
            e.material.blendC = 2;
            e.material.blendD = 1;
            e.material.blendFix = guest<uint8_t>(rdram, vif + 0x20);
            e.matrix = matrix;
            for (size_t i = 0; i < model->corners.size(); i += 3) {
                if (!(shown >> model->meshOfCorner[i] & 1) || !model->corners[i].environment)
                    continue;
                for (size_t k = 0; k < 3; ++k) {
                    const Corner &c = model->corners[i + k];
                    bozo::render::Vertex v;
                    v.x = posed[c.vertex][0];
                    v.y = posed[c.vertex][1];
                    v.z = posed[c.vertex][2];
                    v.u = reflected[c.vertex][0] / float(environment->width);
                    v.v = reflected[c.vertex][1] / float(environment->height);
                    v.rgba = lit[c.vertex];
                    e.vertices.push_back(v);
                }
            }
        }
        if (!e.vertices.empty())
            g_draws.push_back(std::move(e));
        if (!d.vertices.empty())
            g_draws.push_back(std::move(d));
    }
    }

    void submit()
    {
        auto &renderer = bozo::render::Renderer::instance();
        for (const Draw &d : g_draws)
            renderer.draw(d.material, d.matrix.data(), d.vertices.data(), uint32_t(d.vertices.size()), d.texture);
    }

    size_t pending() { return g_draws.size(); }

    void clear()
    {
        g_draws.clear();
        g_parents.clear();
        if (++g_generation % 3600 == 0)
            g_models.clear();
    }
}
