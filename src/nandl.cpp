#include "nandl.hpp"
#include "common.hpp"

#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/ui/MDPopup.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>

using namespace geode::prelude;

namespace {

constexpr char const* kApi = "https://nandl.pages.dev/api/rankings?inflated=0&list=rankings&v=6";

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// trims spaces so "VSC " in GD matches "VSC" on the list
std::string norm(std::string s) {
    s = lower(std::move(s));
    auto b = s.find_first_not_of(' ');
    auto e = s.find_last_not_of(' ');
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

double total(fw::nandl::Hist const& h) {
    double t = 0;
    for (double v : h) t += v;
    return t;
}

// average window over the inputs counted (a single "tighter/looser" number)
double mean(fw::nandl::Hist const& h) {
    double t = total(h), s = 0;
    for (size_t k = 0; k < h.size(); k++) s += k * h[k];
    return t > 0 ? s / t : 0;
}

void show(std::string const& name, fw::nandl::Hist const& nan, fw::nandl::Hist const& ours) {
    std::string md = fmt::format("# {} vs NaNDL\n\n", name);
    // plain lines: the markdown popup doesn't do tables
    for (size_t k = 0; k < nan.size(); k++)
        md += fmt::format("**{}f** - NaN {:.1f} - you {:.1f} ({:+.1f})\n\n", k, nan[k], ours[k], ours[k] - nan[k]);
    md += fmt::format("**total** - NaN {:.1f} - you {:.1f} ({:+.1f})\n\n", total(nan), total(ours), total(ours) - total(nan));
    double mn = mean(nan), mo = mean(ours);
    md += fmt::format("Average window: NaN **{:.2f}**, you **{:.2f}** frames. ", mn, mo);
    if (mo < mn - 0.15) md += "Your windows come out **tighter** than NaN's.";
    else if (mo > mn + 0.15) md += "Your windows come out **looser** than NaN's.";
    else md += "Close match on average.";
    md += "\n\nOnly inputs with a window of 10 or less count. NaN's numbers are averaged over click alignment, "
          "so they can be fractional; yours are counted the same way.";
    MDPopup::create(true, "NaNDL comparison", md, "OK")->show();
}

} // namespace

namespace fw::nandl {

Hist histogram(std::vector<double> const& windows) {
    Hist h{};
    for (double w : windows) {
        if (!(w >= 0.0)) continue;
        double fl = std::floor(w + 1e-9);
        double fr = std::clamp(w - fl, 0.0, 1.0);
        int k = static_cast<int>(fl);
        if (k >= 0 && k <= 10) h[k] += 1.0 - fr;
        if (k + 1 >= 0 && k + 1 <= 10) h[k + 1] += fr;
    }
    return h;
}

int nearestBin(double w) {
    int b = static_cast<int>(std::floor(w + 1e-9));
    if (w - b > 0.5 + 1e-9) b++;
    return std::max(0, b);
}

std::string fmtHist(Hist const& h) {
    std::string s;
    for (size_t k = 0; k < h.size(); k++) s += fmt::format("{}{}:{:.1f}", k ? " " : "", k, h[k]);
    return s;
}

void compare(std::string levelName, Hist ours, bool quiet) {
    auto override_ = Mod::get()->getSettingValue<std::string>("nandl-name");
    if (!norm(override_).empty()) levelName = override_;
    auto want = norm(levelName);
    if (want.empty()) return;

    async::spawn(web::WebRequest().get(kApi), [levelName, want, ours, quiet](web::WebResponse res) {
        if (!res.ok()) {
            FW_WARN("NANDL fetch failed: HTTP {}", res.code());
            if (!quiet) Notification::create("NaNDL: couldn't reach nandl.pages.dev", NotificationIcon::Error)->show();
            return;
        }
        auto json = res.json();
        if (!json) {
            FW_WARN("NANDL bad response: {}", json.unwrapErr());
            return;
        }
        auto root = json.unwrap();
        auto rows = root["frameWindows"].asArray();
        if (!rows) return;
        for (auto const& row : rows.unwrap()) {
            auto name = row["name"].asString();
            if (!name || norm(name.unwrap()) != want) continue;
            auto fws = row["frameWindows"].asArray();
            if (!fws) continue;
            Hist nan{};
            size_t k = 0;
            for (auto const& v : fws.unwrap()) {
                if (k >= nan.size()) break;
                double d = 0;
                if (v.isNumber()) d = v.asDouble().unwrapOr(0.0);
                else if (v.isString()) {
                    auto str = v.asString().unwrapOr("0");
                    char* end = nullptr;
                    d = std::strtod(str.c_str(), &end);
                }
                nan[k++] = d;
            }
            FW_LOG("NANDL level='{}' nan=[{}] ours=[{}] meanNan={:.2f} meanOurs={:.2f}",
                name.unwrap(), fmtHist(nan), fmtHist(ours), mean(nan), mean(ours));
            show(name.unwrap(), nan, ours);
            return;
        }
        FW_LOG("NANDL level='{}' not listed on NaNDL", levelName);
        if (!quiet)
            Notification::create(fmt::format("NaNDL has no level called '{}'.\nSet its name in the mod settings (NaNDL level name).",
                levelName), NotificationIcon::Info)->show();
    });
}

} // namespace fw::nandl
