#include "timings.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

using namespace geode::prelude;

namespace {

// "3", "3.62", "1/2", "2 3/8" -> frames; "-" / garbage -> NaN (not a timing)
double parseWindow(matjson::Value const& v) {
    if (v.isNumber()) return v.asDouble().unwrapOr(std::nan(""));
    if (!v.isString()) return std::nan("");
    auto s = v.asString().unwrapOr("");
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    if (s.empty() || s == "-") return std::nan("");
    double whole = 0;
    auto sp = s.find(' ');
    if (sp != std::string::npos) {
        whole = std::strtod(s.substr(0, sp).c_str(), nullptr);
        s = s.substr(sp + 1);
    }
    auto sl = s.find('/');
    if (sl != std::string::npos) {
        double n = std::strtod(s.substr(0, sl).c_str(), nullptr);
        double d = std::strtod(s.substr(sl + 1).c_str(), nullptr);
        return d > 0 ? whole + n / d : std::nan("");
    }
    char* end = nullptr;
    double x = std::strtod(s.c_str(), &end);
    return (end && end != s.c_str()) ? whole + x : std::nan("");
}

Result<std::vector<fw::Timing>> readJson(std::filesystem::path const& path) {
    auto text = file::readString(path);
    if (!text) return Err("can't read the file: {}", text.unwrapErr());
    auto json = matjson::parse(text.unwrap());
    if (!json) return Err("not valid JSON");
    auto root = json.unwrap();
    // settings: gameFps (default 240) and the time unit (frames or seconds)
    double fps = 240.0;
    bool seconds = false;
    if (root.contains("settings")) {
        auto st = root["settings"];
        if (st.contains("gameFps")) fps = st["gameFps"].asDouble().unwrapOr(240.0);
        if (st.contains("timeUnit")) seconds = st["timeUnit"].asString().unwrapOr("frames") == "seconds";
    }
    if (!(fps > 0)) fps = 240.0;
    auto rows = root["frameWindows"].asArray();
    if (!rows) return Err("no \"frameWindows\" list in this file");
    std::vector<fw::Timing> out;
    int n = 0;
    for (auto const& row : rows.unwrap()) {
        n++;
        double w = parseWindow(row["frameWindow"]);
        if (std::isnan(w) || w <= 0) continue;
        double tp = row["timePosition"].asDouble().unwrapOr(std::nan(""));
        if (std::isnan(tp) && row["timePosition"].isString())
            tp = std::strtod(row["timePosition"].asString().unwrapOr("").c_str(), nullptr);
        if (std::isnan(tp)) continue;
        fw::Timing t;
        t.t = seconds ? tp * 240.0 : tp * 240.0 / fps;
        t.w = w;
        t.p2 = row.contains("isPlayer2") && row["isPlayer2"].asBool().unwrapOr(false);
        t.input = static_cast<int>(row["input"].asDouble().unwrapOr(static_cast<double>(n)));
        // our own extra fields (ignored by NaNDL): exact edges, press/release
        if (row.contains("lo") && row.contains("hi")) {
            t.lo = row["lo"].asDouble().unwrapOr(std::nan(""));
            t.hi = row["hi"].asDouble().unwrapOr(std::nan(""));
        }
        if (row.contains("press")) t.down = row["press"].asBool().unwrapOr(true) ? 1 : 0;
        out.push_back(t);
    }
    return Ok(out);
}

Result<std::vector<fw::Timing>> readFwc(std::filesystem::path const& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return Err("can't open the file");
    std::vector<char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t pos = 0;
    auto take = [&](void* dst, size_t n) {
        if (pos + n > buf.size()) return false;
        std::memcpy(dst, buf.data() + pos, n);
        pos += n;
        return true;
    };
    char magic[4];
    if (!take(magic, 4)) return Err("file too short");
    bool legacy = std::memcmp(magic, "FWCB", 4) == 0;
    bool v2 = std::memcmp(magic, "FWC2", 4) == 0;
    if (!legacy && !v2) return Err("not a frame-window-counter .fwc file");
    double fps = 240.0;
    uint32_t count = 0;
    if (!take(&fps, 8) || !take(&count, 4)) return Err("file too short");
    if (!(fps > 0)) fps = 240.0;
    std::vector<fw::Timing> out;
    for (uint32_t i = 0; i < count; i++) {
        int32_t frame = 0;
        double w = 1.0;
        uint8_t flags = 0;
        int32_t ifCount = 1;
        if (!take(&frame, 4)) return Err("file cut off at entry {}", i + 1);
        if (legacy) {
            int32_t lw = 1;
            if (!take(&lw, 4)) return Err("file cut off at entry {}", i + 1);
            w = lw;
        } else if (!take(&w, 8)) {
            return Err("file cut off at entry {}", i + 1);
        }
        if (!take(&flags, 1)) return Err("file cut off at entry {}", i + 1);
        if (v2 && !take(&ifCount, 4)) return Err("file cut off at entry {}", i + 1);
        if (!(flags & 1) || !(w > 0)) continue; // not drawn = not a timing there
        fw::Timing t;
        t.t = frame * 240.0 / fps;
        t.w = w;
        t.p2 = (flags & 2) != 0;
        t.input = static_cast<int>(i) + 1;
        out.push_back(t);
    }
    return Ok(out);
}

std::string num(double v) {
    auto s = fmt::format("{:.4f}", v);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s.empty() ? "0" : s;
}

} // namespace

namespace fw::timings {

Result<std::vector<Timing>> readFile(std::filesystem::path const& path) {
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    auto res = ext == ".fwc" ? readFwc(path) : readJson(path);
    if (!res) return res;
    auto list = res.unwrap();
    std::stable_sort(list.begin(), list.end(), [](Timing const& a, Timing const& b) { return a.t < b.t; });
    return Ok(list);
}

std::string toNandlJson(std::vector<Timing> const& list) {
    std::string rows;
    for (size_t k = 0; k < list.size(); k++) {
        auto const& t = list[k];
        if (k) rows += ",\n";
        std::string extra;
        if (!std::isnan(t.lo) && !std::isnan(t.hi)) extra += fmt::format(", \"lo\": {}, \"hi\": {}", num(t.lo), num(t.hi));
        if (t.down >= 0) extra += fmt::format(", \"press\": {}", t.down ? "true" : "false");
        rows += fmt::format("    {{ \"input\": {}, \"timePosition\": {}, \"frameWindow\": {}, \"isPlayer2\": {}{} }}",
            t.input > 0 ? t.input : static_cast<int>(k) + 1, num(t.t), num(t.w), t.p2 ? "true" : "false", extra);
    }
    return fmt::format(
        "{{\n  \"format\": \"nandl-calculator\",\n  \"version\": 1,\n"
        "  \"settings\": {{ \"gameFps\": 240, \"windowFps\": 240, \"respawnSeconds\": 0, \"timeUnit\": \"frames\" }},\n"
        "  \"frameWindows\": [\n{}\n  ]\n}}\n", rows);
}

std::vector<uint8_t> toFwc(std::vector<Timing> const& list) {
    std::vector<uint8_t> out;
    auto put = [&](void const* p, size_t n) {
        auto b = static_cast<uint8_t const*>(p);
        out.insert(out.end(), b, b + n);
    };
    put("FWC2", 4);
    double fps = 240.0;
    put(&fps, 8);
    uint32_t count = static_cast<uint32_t>(list.size());
    put(&count, 4);
    for (auto const& t : list) {
        int32_t frame = static_cast<int32_t>(std::llround(t.t));
        double w = t.w;
        uint8_t flags = 1 | (t.p2 ? 2 : 0);
        int32_t ifCount = 1;
        put(&frame, 4);
        put(&w, 8);
        put(&flags, 1);
        put(&ifCount, 4);
    }
    return out;
}

} // namespace fw::timings
