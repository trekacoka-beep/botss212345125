#include "macro.hpp"

#include <Geode/loader/Mod.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace macro {

namespace {

class Reader {
public:
    Reader(std::uint8_t const* data, std::size_t size) : m_data(data), m_size(size) {}

    bool ok() const { return m_ok; }
    std::size_t pos() const { return m_pos; }
    std::size_t size() const { return m_size; }

    bool bytes(void* out, std::size_t count) {
        if (!m_ok || m_pos + count > m_size) { m_ok = false; return false; }
        std::memcpy(out, m_data + m_pos, count);
        m_pos += count;
        return true;
    }

    bool skip(std::size_t count) {
        if (!m_ok || m_pos + count > m_size) { m_ok = false; return false; }
        m_pos += count;
        return true;
    }

    bool u64(std::uint64_t& out) {
        out = 0;
        int shift = 0;
        while (m_ok) {
            if (m_pos >= m_size || shift > 63) { m_ok = false; return false; }
            std::uint8_t byte = m_data[m_pos++];
            out |= (std::uint64_t)(byte & 0x7F) << shift;
            shift += 7;
            if ((byte & 0x80) == 0) return true;
        }
        return false;
    }

    bool str(std::string& out) {
        std::uint64_t length = 0;
        if (!u64(length)) return false;
        if (length > 0xFFFF || m_pos + length > m_size) { m_ok = false; return false; }
        out.assign(reinterpret_cast<char const*>(m_data + m_pos), (std::size_t)length);
        m_pos += (std::size_t)length;
        return true;
    }

    bool f32(float& out) {
        std::uint8_t raw[4];
        if (!bytes(raw, 4)) return false;
        std::uint32_t bits = ((std::uint32_t)raw[0] << 24) | ((std::uint32_t)raw[1] << 16) |
                             ((std::uint32_t)raw[2] << 8) | (std::uint32_t)raw[3];
        std::memcpy(&out, &bits, 4);
        return true;
    }

    bool f64(double& out) {
        std::uint8_t raw[8];
        if (!bytes(raw, 8)) return false;
        std::uint64_t bits = 0;
        for (int i = 0; i < 8; ++i) bits = (bits << 8) | raw[i];
        std::memcpy(&out, &bits, 8);
        return true;
    }

    bool boolean(bool& out) {
        std::uint8_t raw = 0;
        if (!bytes(&raw, 1)) return false;
        out = raw != 0;
        return true;
    }

private:
    std::uint8_t const* m_data = nullptr;
    std::size_t m_size = 0;
    std::size_t m_pos = 0;
    bool m_ok = true;
};

std::vector<std::uint8_t> readFile(std::filesystem::path const& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::filesystem::path macroFolder() {
    namespace fs = std::filesystem;
    std::error_code ec;

    fs::path configRoot = geode::Mod::get()->getConfigDir();
    fs::path folder = configRoot.parent_path() / "bogdoner.click-indicators" / "macros";
    if (!fs::exists(folder, ec)) {
        folder = configRoot / "macros";
    }
    return folder;
}

bool readHeader(std::filesystem::path const& path, Entry& out) {
    auto raw = readFile(path);
    if (raw.size() < 16) return false;

    Reader reader(raw.data(), raw.size());
    char magic[3] = {};
    std::uint64_t version = 0, gameVersion = 0, seed = 0, coins = 0, botVersion = 0, levelId = 0;
    std::string inputTag, author, description, botName, levelName;
    float duration = 0.f;
    double framerate = 240.0;
    bool ldm = false, platformer = false;

    bool good = reader.bytes(magic, 3) && std::memcmp(magic, "GDR", 3) == 0 &&
                reader.u64(version) && reader.str(inputTag) && reader.str(author) && reader.str(description) &&
                reader.f32(duration) && reader.u64(gameVersion) && reader.f64(framerate) &&
                reader.u64(seed) && reader.u64(coins) && reader.boolean(ldm) && reader.boolean(platformer) &&
                reader.str(botName) && reader.u64(botVersion) && reader.u64(levelId) && reader.str(levelName);
    if (!good) return false;

    out.path = path;
    out.levelId = (std::uint32_t)levelId;
    out.levelName = levelName;
    std::error_code ec;
    out.time = std::filesystem::last_write_time(path, ec);
    return true;
}

} // namespace

std::string load(std::filesystem::path const& path, Data& out) {
    auto raw = readFile(path);
    if (raw.size() < 16) return "file is too small";

    Reader reader(raw.data(), raw.size());
    char magic[3] = {};
    if (!reader.bytes(magic, 3) || std::memcmp(magic, "GDR", 3) != 0) {
        return "not a GDR macro";
    }

    std::uint64_t version = 0, gameVersion = 0, seed = 0, coins = 0, botVersion = 0, levelId = 0;
    std::string inputTag, author, description, botName, levelName;
    float duration = 0.f;
    double framerate = 240.0;
    bool ldm = false, platformer = false;

    if (!reader.u64(version) || !reader.str(inputTag) || !reader.str(author) || !reader.str(description)) return "broken header";
    if (!reader.f32(duration) || !reader.u64(gameVersion) || !reader.f64(framerate)) return "broken header";
    if (!reader.u64(seed) || !reader.u64(coins) || !reader.boolean(ldm) || !reader.boolean(platformer)) return "broken header";
    if (!reader.str(botName) || !reader.u64(botVersion) || !reader.u64(levelId) || !reader.str(levelName)) return "broken header";

    std::uint64_t extensionSize = 0;
    if (!reader.u64(extensionSize) || !reader.skip((std::size_t)extensionSize)) return "broken extension block";

    std::uint64_t deathCount = 0;
    if (!reader.u64(deathCount)) return "broken death block";
    for (std::uint64_t i = 0; i < deathCount; ++i) {
        std::uint64_t delta = 0;
        if (!reader.u64(delta)) return "broken death block";
    }

    std::uint64_t totalInputs = 0, player1Inputs = 0;
    if (!reader.u64(totalInputs) || !reader.u64(player1Inputs)) return "broken input block";

    double fps = framerate > 1.0 ? framerate : 240.0;
    out.name = path.stem().string();
    out.framerate = fps;
    out.events.clear();
    out.events.reserve((std::size_t)totalInputs);

    std::uint64_t frame = 0, remainingP1 = player1Inputs;

    while (reader.ok() && reader.pos() < reader.size()) {
        std::uint64_t packed = 0;
        if (!reader.u64(packed)) break;

        std::uint64_t delta = 0;
        int button = 1;
        bool down = false;
        bool player2 = remainingP1 == 0;

        if (platformer) {
            delta = packed >> 3;
            button = (int)((packed >> 1) & 3);
            down = (packed & 1) != 0;
        } else {
            delta = packed >> 1;
            down = (packed & 1) != 0;
        }

        frame += delta;

        Event event;
        event.time = (float)((double)frame / fps);
        event.button = button;
        event.down = down;
        event.player2 = player2;
        out.events.push_back(event);

        if (remainingP1 > 0) {
            --remainingP1;
            if (remainingP1 == 0) frame = 0;
        }
    }

    if (out.events.empty()) return "no inputs in macro";

    std::stable_sort(out.events.begin(), out.events.end(), [](Event const& a, Event const& b) {
        return a.time < b.time;
    });
    return {};
}

std::vector<Entry> list() {
    namespace fs = std::filesystem;
    std::error_code ec;

    std::vector<Entry> entries;
    fs::path folder = macroFolder();
    if (!fs::exists(folder, ec)) return entries;

    for (auto const& item : fs::directory_iterator(folder, ec)) {
        if (!item.is_regular_file(ec)) continue;
        if (item.path().extension() != ".gdr2") continue;
        Entry entry;
        if (readHeader(item.path(), entry)) entries.push_back(std::move(entry));
    }

    std::sort(entries.begin(), entries.end(), [](Entry const& a, Entry const& b) {
        return lower(a.path.stem().string()) < lower(b.path.stem().string());
    });
    return entries;
}

std::filesystem::path resolve(std::string const& preferred) {
    namespace fs = std::filesystem;
    std::error_code ec;

    fs::path folder = macroFolder();
    if (!preferred.empty()) {
        fs::path direct(preferred);
        if (fs::exists(direct, ec)) return direct;
        fs::path inside = folder / preferred;
        if (fs::exists(inside, ec)) return inside;
    }

    fs::path newest;
    fs::file_time_type newestTime{};
    if (fs::exists(folder, ec)) {
        for (auto const& entry : fs::directory_iterator(folder, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            if (entry.path().extension() != ".gdr2") continue;
            auto time = entry.last_write_time(ec);
            if (newest.empty() || time > newestTime) {
                newest = entry.path();
                newestTime = time;
            }
        }
    }
    return newest;
}

std::filesystem::path pickForLevel(int levelId, std::string const& levelName) {
    namespace fs = std::filesystem;
    std::error_code ec;

    auto entries = list();
    if (entries.empty()) return {};

    if (levelId > 0) {
        for (auto const& entry : entries) {
            if ((int)entry.levelId == levelId) return entry.path;
        }
    }

    std::string wanted = lower(levelName);
    if (!wanted.empty()) {
        for (auto const& entry : entries) {
            if (lower(entry.levelName) == wanted) return entry.path;
        }
    }

    auto best = std::max_element(entries.begin(), entries.end(), [](Entry const& a, Entry const& b) {
        return a.time < b.time;
    });
    return best != entries.end() ? best->path : std::filesystem::path{};
}

} // namespace macro
