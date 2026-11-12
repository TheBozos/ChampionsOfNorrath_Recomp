// con: Champions of Norrath on BozoEngine in a window, on the host's clock. The game's data is the staged folder's
// SLUS_205.65 and disc.iso unless --elf and --iso say otherwise.
//
//   con [--iso image] [--elf SLUS_205.65] [--card-dir dir] [--width w --height h] [--fullscreen] [--no-vsync]
//       [--render-scale s] [--capture-dir dir] [--capture-frames n,n,...] [--quit-after n] [--pad-script [n=]file]...
//       [--print] [--no-sound] [--audio-latency ms] [--audio-stats] [--wav file]
#include "con_audio.h"
#include "con_functions.h"
#include "con_world.h"

#include "bozo/app/app.h"
#include "bozo/app/audio_device.h"
#include "bozo/app/determinism.h"
#include "bozo/app/device_pad.h"
#include "bozo/app/input_bindings.h"
#include "bozo/app/video_settings.h"
#include "bozo/app/window.h"
#include "bozo/audio/wav.h"
#include "bozo/ee/dispatcher.h"
#include "bozo/host/free_host.h"
#include "bozo/port.h"
#include "bozo/render/renderer.h"

#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>

#ifndef BOZO_RUN_DEFAULT_ELF
#define BOZO_RUN_DEFAULT_ELF ""
#endif

int main(int argc, char **argv)
{
    bozo::app::Identity identity;
    identity.title = "Champions of Norrath";
    identity.folder = "CoN_Recomp";
    identity.xdgFolder = "con_recomp";
    identity.bootElf = "SLUS_205.65";
    identity.dataFolder = "game";
    bozo::app::setIdentity(identity);
    bozo::app::startLogging();
    bozo::app::installCrashHandler();

    bozo::host::FreeOptions opt;
    opt.elf = BOZO_RUN_DEFAULT_ELF;
    opt.realTime = true;
    opt.quiet = true;
    bozo::app::WindowOptions window;
    window.title = "Champions of Norrath (BozoEngine)";
    bozo::app::AudioOptions audio;
    bool sound = true;
    std::string wavPath;
    int width = 0, height = 0;
    bool fullscreen = false;
    float renderScale = 0.0f;
    for (int k = 1; k < argc; ++k)
    {
        const std::string a = argv[k];
        auto value = [&]() -> std::string { return k + 1 < argc ? argv[++k] : ""; };
        if (a == "--elf")
            opt.elf = value();
        else if (a == "--iso")
            opt.iso = value();
        else if (a == "--card-dir")
            opt.cardDir = value();
        else if (a == "--pad-script")
        {
            std::string v = value();
            int player = 1;
            if (v.size() > 2 && v[1] == '=' && v[0] >= '1' && v[0] <= '4')
            {
                player = v[0] - '0';
                v = v.substr(2);
            }
            (player == 1 ? opt.padScript : opt.playerScripts[static_cast<size_t>(player - 2)]) = v;
        }
        else if (a == "--width")
            width = std::stoi(value());
        else if (a == "--height")
            height = std::stoi(value());
        else if (a == "--fullscreen")
            fullscreen = true;
        else if (a == "--no-vsync")
            window.vsync = false;
        else if (a == "--render-scale")
            renderScale = std::stof(value());
        else if (a == "--capture-dir")
            window.captureDir = value();
        else if (a == "--capture-frames")
        {
            std::stringstream list(value());
            for (std::string item; std::getline(list, item, ',');)
                if (!item.empty())
                    window.captureFrames.push_back(std::stoull(item));
        }
        else if (a == "--quit-after")
            window.quitAfterFrames = std::stoull(value());
        else if (a == "--print")
            opt.printGameOutput = true;
        else if (a == "--no-sound")
            sound = false;
        else if (a == "--audio-latency")
            audio.latencyMs = std::stod(value());
        else if (a == "--audio-stats")
            audio.stats = true;
        else if (a == "--wav")
            wavPath = value();
        else
        {
            std::fprintf(stderr, "usage: con [--iso image] [--elf path] [--card-dir dir] [--width w] [--height h] [--fullscreen] "
                                 "[--no-vsync] [--render-scale s] [--capture-dir dir] [--capture-frames n,n,...] [--quit-after n] "
                                 "[--pad-script [n=]file]... [--print] [--no-sound] [--audio-latency ms] [--audio-stats] [--wav file]\n");
            return 2;
        }
    }
    bozo::app::video::loadAtStartup(window.width, window.height);
    {
        bozo::app::video::Settings &v = bozo::app::video::edited();
        if (width > 0 && height > 0)
            v.width = width, v.height = height;
        if (fullscreen)
            v.mode = bozo::app::video::WindowMode::Borderless;
        if (renderScale > 0.0f)
            v.renderScale = renderScale;
    }
    window.videoSettings = true;
    std::error_code ec;
    if (opt.elf.empty() || !std::filesystem::is_regular_file(opt.elf, ec))
    {
        std::string searched;
        const std::filesystem::path found = bozo::app::findGameData(&searched);
        if (found.empty())
        {
            std::fprintf(stderr, "con: no ELF (--elf); looked for the game data at\n%s", searched.c_str());
            return 2;
        }
        opt.elf = found.string();
    }
    if (opt.cardDir.empty() && !bozo::app::userDirectory().empty())
        opt.cardDir = bozo::app::userDirectory().string();
    if (opt.iso.empty())
    {
        const std::filesystem::path staged = std::filesystem::path(opt.elf).parent_path() / "disc.iso";
        if (std::filesystem::is_regular_file(staged, ec))
            opt.iso = staged.string();
    }
    if (opt.iso.empty())
    {
        std::fprintf(stderr, "con: no disc image (--iso, or disc.iso beside the ELF)\n");
        return 2;
    }
    bozo::det::setEnabled(false);

    const bozo::port::Port *port = bozo::port::port();
    bozo::port::Registry registry;
    port->install(registry);
    const std::vector<uint32_t> missing = bozo::port::undeclared(registry, con_external_table, con_external_count);
    if (!missing.empty())
    {
        std::fprintf(stderr, "%zu registered overrides are not externals of the generated code (regenerate)\n", missing.size());
        return 2;
    }

    bozo::host::FreeHost host(opt);
    bozo::ee::Dispatcher d(host);
    d.setFunctions(con_function_table, con_function_count, con_resume_table, con_resume_count);
    std::string error;
    if (!host.setUp(d, con_external_table, con_external_count, &registry, error))
    {
        std::fprintf(stderr, "con: %s\n", error.c_str());
        return 2;
    }
    con::audio::attach(host.libraries());
    bozo::app::DevicePad pad;
    bool scripted = !opt.padScript.empty();
    for (const std::string &script : opt.playerScripts)
        scripted = scripted || !script.empty();
    if (!scripted) {
        host.libraries().pad.input = &pad;
        host.libraries().cd.hostClock = true;
    }
    const std::string pads = scripted ? std::string("the scripts") : "the PC's devices (" + bozo::app::input::configPath() + ")";
    std::fprintf(stderr, "con: user folder %s, memory cards in %s, pads from %s\n", bozo::app::userDirectory().string().c_str(),
                 opt.cardDir.empty() ? "memory" : opt.cardDir.c_str(), pads.c_str());
    std::fprintf(stderr, "con: %s, %zu overrides, ELF %s, disc %s\n", port->title, registry.functions().size(), opt.elf.c_str(),
                 opt.iso.c_str());

    bozo::app::AudioDevice device(audio);
    bozo::audio::WavWriter wav;
    if (!wavPath.empty() && !wav.open(wavPath))
        std::fprintf(stderr, "con: cannot write %s\n", wavPath.c_str());
    if (sound)
        device.open();
    if (device.isOpen() || wav.isOpen())
        host.onAudio = [&](uint64_t, const int16_t *samples, size_t count) {
            device.push(samples, count);
            wav.write(samples, count);
        };

    bozo::render::Renderer::instance().setEnabled(true);
    con::world::setDiscImage(opt.iso);

    bozo::app::Game game;
    game.run = [&]
    {
        const uint64_t frames = host.run();
        if (device.isOpen())
            device.close();
        wav.close();
        std::fprintf(stderr, "con: the game ended after %llu frames: %s\n", static_cast<unsigned long long>(frames),
                     host.endReason().empty() ? "every thread ended" : host.endReason().c_str());
        return host.endReason().rfind("FAULT", 0) == 0 ? 1 : 0;
    };
    game.stop = [&] { host.requestStop(); };
    game.framesEnded = [&] { return host.framesEnded(); };
    return bozo::app::runWindow(window, game, &pad);
}
