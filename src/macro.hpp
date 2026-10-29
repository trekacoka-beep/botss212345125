#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace macro {

struct Event {
    float time = 0.f;
    int   button = 1;     // 1 = jump, 2 = left, 3 = right
    bool  down = true;
    bool  player2 = false;
};

struct Data {
    std::string name;
    double framerate = 240.0;
    std::vector<Event> events;   // sorted by time, both presses and releases
};

struct Entry {
    std::filesystem::path path;
    std::uint32_t levelId = 0;
    std::string levelName;
    std::filesystem::file_time_type time{};
};

// Empty string on success, otherwise a human readable error.
std::string load(std::filesystem::path const& path, Data& out);

std::vector<Entry> list();

std::filesystem::path resolve(std::string const& preferred);

// Picks the macro whose level id (or name) matches, newest as fallback.
std::filesystem::path pickForLevel(int levelId, std::string const& levelName);

} // namespace macro
