// CoN's hardware-only output instructions: no consumer exists natively, so they are replaced before they reach MMIO.
#include "bozo/context.h"
#include "bozo/port.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace con::graphics
{
    namespace
    {
        // bozo:native kHardwareOutput
        constexpr uint32_t kHardwareOutput[] = {
            // drawInit, drawRestoreInterrupts: GS IMR and GIF_MODE.
            0x0015f914u, 0x0015f918u, 0x0015fa30u, 0x0015fa34u,
            // engineInit: T1_MODE (an hblank line counter only movies read).
            0x00190874u,
            // Native presentation owns the display: engineInit's and vblankHandler's PMODE, DISPFB, DISPLAY, SMODE2.
            0x001902d4u, 0x00190480u, 0x0019058cu, 0x00190640u, 0x0019064cu, 0x0019069cu, 0x001906a4u, 0x001906acu,
            0x001906b4u,
            0x0018e4b4u, 0x0018e4c8u, 0x0018e5b4u, 0x0018e604u, 0x0018e608u, 0x0018e60cu, 0x0018e610u, 0x0018e628u,
            0x0018e648u,
            // engineInit, drawInit, drawRestoreInterrupts: the GIF, VIF0 and VIF1 channels' interrupt bit (CHCR TIE).
            0x001907e4u, 0x001907f8u, 0x00190800u, 0x00190810u, 0x00190820u, 0x00190830u,
            0x0015f7a4u, 0x0015f7b4u, 0x0015f7c4u, 0x0015f7d4u, 0x0015f7e4u, 0x0015f7f8u,
            0x0015f940u, 0x0015f958u, 0x0015f968u, 0x0015f978u, 0x0015f988u, 0x0015f99cu,
            // lightVU0StoppedProcessing: the VU1 microcode's light constants (native draws read the game's light state).
            0x0013cebcu, 0x0013ced8u, 0x0013cedcu, 0x0013cee0u, 0x0013cee4u, 0x0013cee8u, 0x0013ceecu, 0x0013cef0u,
            0x0013cf00u,
            // reloadDMA: the frame's VIF1/GIF transfer kick; its two channel polls read an idle channel.
            0x0015eda0u, 0x0015efe4u, 0x0015f068u, 0x0015f244u, 0x0015f25cu, 0x0015f3b8u, 0x0015f3d8u, 0x0015f43cu,
            0x0015f458u, 0x0015f600u, 0x0015f61cu,
            // Loading screens, movies and the engine raster's display registers.
            0x0106c034u, 0x0106c158u, 0x0106c160u, 0x0106c164u, 0x0106c168u, 0x0106c16cu, 0x0106c170u,
            0x010b5c90u, 0x010b5c94u, 0x010b5c98u,
            0x01100770u, 0x01100cc0u, 0x01100d08u, 0x01100d2cu, 0x01100d78u,
            0x0110a580u, 0x0110ab34u, 0x0110ab88u, 0x0110ab8cu, 0x0110abc0u, 0x0110abc4u, 0x0110abc8u, 0x01112e18u,
            // putBlock: the scratchpad address of its decode-cache copy, which no native code reads.
            0x0016f750u, 0x0016f764u,
            // vblankHandler's CSR reads: the field parity comes from the native frame clock.
            0x0018e4e0u, 0x0018e658u,
        };

        constexpr auto kSorted = [] {
            auto sites = std::to_array(kHardwareOutput);
            std::sort(sites.begin(), sites.end());
            return sites;
        }();

        constexpr std::array<uint32_t, 9> kChcrReads = {0x0015f7a4u, 0x0015f7c4u, 0x0015f7e4u, 0x0015f940u, 0x0015f968u,
                                                        0x0015f988u, 0x001907e4u, 0x00190800u, 0x00190820u};

        bool instruction(uint8_t *, R5900Context *ctx, Runtime *rt, uint32_t pc)
        {
            if (!std::binary_search(kSorted.begin(), kSorted.end(), pc))
                return false;
            if (pc == 0x0018e4e0u || pc == 0x0018e658u)
                setRegU64(ctx, 3, (bozo::port::vblankCount(rt) & 1u) << 13);
            else if (std::binary_search(kChcrReads.begin(), kChcrReads.end(), pc))
                setRegU64(ctx, 3, 0);
            else if (pc == 0x0015efe4u || pc == 0x0015f068u)
                setRegU64(ctx, 2, 0);
            else if (pc == 0x0016f764u)
                setRegU64(ctx, 5, 0);
            return true;
        }

        constexpr uint32_t kDrawWaitFinished = 0x0015faa0u;

        void drawWaitFinished(uint8_t *, R5900Context *ctx, Runtime *rt)
        {
            bozo::port::frameEnd(rt, ctx);
            ctx->pc = getRegU32(ctx, 31);
        }
    }

    void install(bozo::port::Registry &runtime)
    {
        runtime.nativeGraphics(&instruction);
        runtime.registerFunction(kDrawWaitFinished, &drawWaitFinished);
    }
}
