#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include "macro.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

using namespace geode::prelude;

namespace {

struct Sample {
    float time = 0.f;   // macro time
    float x = 0.f;      // player x
};

std::vector<macro::Event> g_events;
size_t g_index = 0;
bool   g_enabled = false;
float  g_offset = 0.f;          // macro time at the attempt start
bool   g_offsetOverride = false;
float  g_offsetOverrideValue = 0.f;
bool   g_needOffset = true;
bool   g_recording = false;     // sample times are meaningful for this attempt
float  g_macroTime = 0.f;
float  g_levelTime = 0.f;
int    g_levelId = 0;
std::vector<Sample> g_map;
bool   g_f9Down = false;

// Fake deaths before a chosen percent, so the run looks human.
int    g_dieBefore = 0;          // percent threshold, 0 = off
float  g_deathTarget = 0.f;      // percent where this attempt dies
bool   g_diedThisAttempt = false;
std::mt19937 g_rng{std::random_device{}()};

bool keyPressed(int vk, bool& state) {
#ifdef _WIN32
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = down && !state;
    state = down;
    return edge;
#else
    return false;
#endif
}

std::filesystem::path mapPath(int levelId) {
    return Mod::get()->getConfigDir() / "maps" / (std::to_string(levelId) + ".txt");
}

void loadMap(int levelId) {
    g_map.clear();
    if (levelId == 0) return;

    std::ifstream file(mapPath(levelId));
    if (!file) return;

    Sample sample;
    while (file >> sample.time >> sample.x) {
        g_map.push_back(sample);
    }
}

void saveMap(int levelId) {
    if (levelId == 0 || g_map.empty()) return;

    std::error_code ec;
    std::filesystem::create_directories(mapPath(levelId).parent_path(), ec);

    std::ofstream file(mapPath(levelId), std::ios::trunc);
    if (!file) return;
    for (auto const& sample : g_map) {
        file << sample.time << ' ' << sample.x << '\n';
    }
}

void recordSample(float macroTime, float x) {
    if (!g_recording) return;
    if (!g_map.empty() && x <= g_map.back().x + 0.5f) return;   // keep it monotonic
    if (g_map.size() > 200000) return;
    g_map.push_back({macroTime, x});
}

float offsetForX(float x) {
    if (g_offsetOverride) return g_offsetOverrideValue;
    if (g_map.empty()) return 0.f;

    for (size_t i = 0; i < g_map.size(); ++i) {
        if (g_map[i].x >= x - 1.f) {
            if (i == 0) return g_map[i].time;
            auto const& a = g_map[i - 1];
            auto const& b = g_map[i];
            float span = b.x - a.x;
            if (span <= 0.001f) return b.time;
            float k = (x - a.x) / span;
            return a.time + (b.time - a.time) * k;
        }
    }
    return g_map.back().time;
}

void loadMacroForLevel(int levelId, std::string const& levelName) {
    std::string preferred = Mod::get()->getSettingValue<std::string>("macro-file");
    std::filesystem::path path;
    if (!preferred.empty()) {
        path = macro::resolve(preferred);
    } else {
        path = macro::pickForLevel(levelId, levelName);
    }
    if (path.empty()) {
        log::warn("Stream Bot: no macro found");
        g_events.clear();
        return;
    }

    macro::Data data;
    auto error = macro::load(path, data);
    if (!error.empty()) {
        log::warn("Stream Bot: {} ({})", error, path.string());
        g_events.clear();
        return;
    }

    g_events = data.events;
    g_index = 0;
    log::info("Stream Bot: loaded '{}' ({} events)", data.name, (int)g_events.size());
}

void reloadConfig() {
    g_enabled = Mod::get()->getSettingValue<bool>("bot-enabled");
    double override = Mod::get()->getSettingValue<double>("start-offset");
    g_offsetOverride = override >= 0.0;
    g_offsetOverrideValue = (float)override;
    g_dieBefore = (int)Mod::get()->getSettingValue<int64_t>("die-before-percent");
}

void fire(macro::Event const& event, PlayLayer* layer) {
    layer->handleButton(event.down, event.button, !event.player2);
}

// Picks the percent where this attempt should die. The distribution is biased
// towards small values, so most attempts die early and once in a while a run
// goes far - like a human learning a level.
void rollDeathTarget() {
    g_diedThisAttempt = false;
    if (g_dieBefore <= 0) {
        g_deathTarget = 0.f;
        return;
    }

    constexpr float kFloor = 2.f;
    float limit = (float)g_dieBefore;
    if (limit <= kFloor) {
        g_deathTarget = limit;
    } else {
        std::uniform_real_distribution<float> dist(0.f, 1.f);
        float u = dist(g_rng);
        g_deathTarget = kFloor + (limit - kFloor) * std::pow(u, 2.6f);
    }

    log::info("Stream Bot: this attempt will die at {:.1f}% (limit {}%)", g_deathTarget, g_dieBefore);
}

} // namespace

$on_mod(Loaded) {
    reloadConfig();
    log::info("Stream Bot loaded, F9 toggles the bot");
}

class $modify(StreamBotPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        g_levelId = level ? (int)level->m_levelID : 0;
        reloadConfig();
        loadMacroForLevel(g_levelId, level ? std::string(level->m_levelName) : std::string{});
        loadMap(g_levelId);

        g_index = 0;
        g_levelTime = 0.f;
        g_needOffset = true;
        g_recording = false;
        return true;
    }

    void startGame() {
        PlayLayer::startGame();
        g_levelTime = 0.f;
        g_index = 0;
        g_needOffset = true;
        g_recording = false;
        reloadConfig();
        rollDeathTarget();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        g_levelTime = 0.f;
        g_index = 0;
        g_needOffset = true;
        g_recording = false;
        rollDeathTarget();
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        if (keyPressed(VK_F9, g_f9Down)) {
            g_enabled = !g_enabled;
            log::info("Stream Bot: {}", g_enabled ? "enabled" : "disabled");
        }

        g_levelTime += dt;

        float playerX = this->m_player1 ? this->m_player1->getPositionX() : 0.f;

        if (g_needOffset) {
            g_offset = offsetForX(playerX);
            g_macroTime = g_offset;
            g_needOffset = false;

            // Only trust samples when we know the macro time of this attempt:
            // either it starts at the beginning of the level, or we mapped it.
            g_recording = (g_offset > 0.01f) || (playerX < 2000.f);
            log::info("Stream Bot: offset {:.2f}s at x={:.0f} (recording={})", g_offset, playerX, g_recording);
        }

        g_macroTime += dt;
        recordSample(g_macroTime, playerX);

        if (!g_enabled || g_events.empty()) return;

        while (g_index < g_events.size()) {
            auto const& event = g_events[g_index];
            if (event.time > g_macroTime) break;

            // Do not fire clicks that are far in the past: if the offset is
            // wrong we would just spam the game.
            if (event.time >= g_macroTime - 0.2f) {
                fire(event, this);
            }
            ++g_index;
        }

        // Fake deaths before the configured percent.
        if (g_enabled && g_dieBefore > 0 && !g_diedThisAttempt && this->m_player1) {
            float percent = this->getCurrentPercent();
            if (percent >= g_deathTarget) {
                g_diedThisAttempt = true;
                log::info("Stream Bot: fake death at {:.1f}%", percent);
                this->destroyPlayer(this->m_player1, nullptr);
            }
        }
    }

    void onExit() {
        saveMap(g_levelId);
        PlayLayer::onExit();
    }
};
