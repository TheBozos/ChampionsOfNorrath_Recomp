// CoN's two VU0 programs as native C++ on the calling context's VU0 state and VU0's data memory, from the microcode in
// the ELF (0x31b520, the MPG initVIF loads). Both are started only by evaluateAnimation.
#include "bozo/context.h"
#include "bozo/port.h"
#include "bozo/runtime.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace con::vu0
{
    namespace
    {
        constexpr uint32_t kIdentityEntry = 0x10u, kBoneMatrixEntry = 0x20u;

        enum { X, Y, Z, W };

        struct Vec
        {
            float v[4];
            float &operator[](int i) { return v[i]; }
            float operator[](int i) const { return v[i]; }
        };

        Vec &vf(R5900Context *ctx, int n) { return *reinterpret_cast<Vec *>(&ctx->vu0_vf[n]); }

        uint8_t *row(Runtime *rt, uint32_t index) { return rt->memory.vu0Data() + (index & 0xFFu) * 16u; }

        Vec load(Runtime *rt, uint32_t index)
        {
            Vec out;
            std::memcpy(out.v, row(rt, index), sizeof(out.v));
            return out;
        }

        void store(Runtime *rt, uint32_t index, const Vec &value) { std::memcpy(row(rt, index), value.v, sizeof(value.v)); }

        Vec rotated(const Vec &a) { return {{a[Y], a[Z], a[W], a[X]}}; }

        int32_t toFixed(float value, float scale)
        {
            const double scaled = std::trunc(static_cast<double>(value) * scale);
            if (scaled >= 2147483647.0)
                return INT32_MAX;
            return scaled <= -2147483648.0 ? INT32_MIN : static_cast<int32_t>(scaled);
        }

        void storeFixed(float &slot, float value, float scale)
        {
            const int32_t fixed = toFixed(value, scale);
            std::memcpy(&slot, &fixed, sizeof(fixed));
        }

        float fromFixed12(float slot)
        {
            int32_t fixed;
            std::memcpy(&fixed, &slot, sizeof(fixed));
            return static_cast<float>(fixed) / 4096.0f;
        }

        void identity(R5900Context *ctx, Runtime *rt)
        {
            vf(ctx, 1) = {{0.0f, 0.0f, 0.0f, 0.0f}};
            vf(ctx, 2) = {{1.0f, 0.0f, 0.0f, 0.0f}};
            vf(ctx, 3) = {{0.0f, 1.0f, 0.0f, 0.0f}};
            vf(ctx, 4) = {{0.0f, 0.0f, 1.0f, 0.0f}};
            for (uint32_t i = 0; i < 3u; ++i)
                store(rt, i, vf(ctx, static_cast<int>(i) + 2));
        }

        void boneMatrix(R5900Context *ctx, Runtime *rt)
        {
            const uint32_t base = ctx->vi[1];
            const Vec zero = vf(ctx, 0);
            Vec &quat = vf(ctx, 5);
            Vec &doubled = vf(ctx, 6);
            doubled[X] = quat[X] + quat[X];
            doubled[Y] = quat[Y] + quat[Y];
            doubled[Z] = quat[Z] + quat[Z];
            Vec &r0 = vf(ctx, 28);
            Vec &r1 = vf(ctx, 29);
            Vec &r2 = vf(ctx, 30);
            r0 = rotated(vf(ctx, 12));
            vf(ctx, 11) = {{1.0f, 1.0f, 1.0f, 2.0f}};
            const Vec one = vf(ctx, 11);
            Vec &scale = vf(ctx, 10);
            for (int i = 0; i < 4; ++i)
                scale[i] = fromFixed12(scale[i]);
            vf(ctx, 1) = load(rt, base);
            vf(ctx, 2) = load(rt, base + 1u);
            vf(ctx, 3) = load(rt, base + 2u);
            Vec q7, q8, q9;
            for (int i = 0; i < 4; ++i)
            {
                q7[i] = quat[i] * doubled[X];
                q8[i] = quat[i] * doubled[Y];
                q9[i] = quat[i] * doubled[Z];
            }
            vf(ctx, 7) = q7;
            vf(ctx, 8) = q8;
            vf(ctx, 9) = q9;
            r1 = rotated(r0);
            r0[X] = one[X] - q8[Y];
            r0[Y] = q8[X];
            r0[Z] = q9[X];
            r2 = rotated(r1);
            r1[X] = q8[X] + q9[W];
            r0[X] -= q9[Z];
            r0[Y] -= q9[W];
            r0[Z] += q8[W];
            r1[Y] = one[Y] - q7[X];
            r1[Z] = q9[Y];
            r2[X] = q9[X] - q8[W];
            r2[Z] = one[Z] - q7[X];
            r2[Y] = q9[Y] + q7[W];
            r1[Y] -= q9[Z];
            r1[Z] -= q7[W];
            r2[Z] -= q8[Y];

            Vec acc{};
            const auto multiply = [&](const Vec &in) {
                Vec out;
                for (int i = 0; i < 4; ++i)
                {
                    acc[i] = r0[i] * in[X] + r1[i] * in[Y] + r2[i] * in[Z];
                    out[i] = acc[i] + zero[i] * in[W];
                }
                return out;
            };
            Vec s0 = rotated(scale);
            vf(ctx, 25) = multiply(vf(ctx, 1));
            Vec s1 = rotated(s0);
            vf(ctx, 26) = multiply(vf(ctx, 2));
            Vec s2 = rotated(s1);
            vf(ctx, 27) = multiply(vf(ctx, 3));
            vf(ctx, 4) = s0;
            quat = s1;
            doubled = s2;
            for (uint32_t i = 0; i < 3u; ++i)
                store(rt, base + 3u + i, vf(ctx, 25 + static_cast<int>(i)));

            ctx->vu0_i = 4.0f;
            for (int n = 25; n <= 27; ++n)
            {
                Vec &m = vf(ctx, n);
                acc[W] = s0[W] * m[X] + s1[W] * m[Y] + s2[W] * m[Z];
                const float w = (acc[W] + m[W]) * 4.0f;
                for (int i = X; i <= Z; ++i)
                    storeFixed(m[i], m[i], 4096.0f);
                storeFixed(m[W], w, 16.0f);
            }
            std::memcpy(&ctx->vu0_acc, acc.v, sizeof(acc.v));
        }
    }

    void install(bozo::port::Registry &runtime)
    {
        runtime.vu0Program(kIdentityEntry, [](uint8_t *, R5900Context *ctx, Runtime *rt) { identity(ctx, rt); });
        runtime.vu0Program(kBoneMatrixEntry, [](uint8_t *, R5900Context *ctx, Runtime *rt) { boneMatrix(ctx, rt); });
    }
}
