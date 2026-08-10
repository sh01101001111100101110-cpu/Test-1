#include <Geode/Geode.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/MDTextArea.hpp>
#include <Geode/utils/web.hpp>
#include <filesystem>

namespace fs = std::filesystem;
using namespace geode::prelude;

// Angle-bracket tags like <mod:hjfod.betteredit> get mangled by translation
// (e.g. "mod" gets translated into a real word, breaking the tag). We swap
// them out for plain placeholder tokens before translating, then restore
// the original tags afterward once the placeholders survive intact.
std::string protectTags(std::string const& text, std::vector<std::string>& tags) {
    std::string result;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '<') {
            size_t end = text.find('>', i);
            if (end != std::string::npos) {
                tags.push_back(text.substr(i, end - i + 1));
                result += "zzztag" + std::to_string(tags.size() - 1) + "zzz";
                i = end + 1;
                continue;
            }
        }
        result += text[i];
        i++;
    }
    return result;
}

std::string restoreTags(std::string const& text, std::vector<std::string> const& tags) {
    std::string result = text;
    for (size_t idx = 0; idx < tags.size(); idx++) {
        std::string placeholder = "zzztag" + std::to_string(idx) + "zzz";
        size_t pos = result.find(placeholder);
        if (pos != std::string::npos) {
            result.replace(pos, placeholder.size(), tags[idx]);
        }
    }
    return result;
}

// Cache of already-translated text per mod ID + translator choice, so we
// don't hit the translation API again every time the same popup reopens.
static std::unordered_map<std::string, std::string> g_translationCache;

// MyMemory's single-request limit is ~500 bytes; we keep a safety margin.
// Google's unofficial endpoint tolerates much bigger requests.
constexpr size_t MAX_CHUNK_BYTES_MYMEMORY = 450;
constexpr size_t MAX_CHUNK_BYTES_GOOGLE = 1800;

// Copies our bundled Cyrillic font pack into Texture Loader's packs folder
// on first run, so Geode's own MDTextArea can render Cyrillic without the
// user having to install the pack separately. Safe to call every launch -
// it checks for a marker file first and does nothing if already installed.
void installFontPackIfNeeded() {
    auto textureLoader = Loader::get()->getInstalledMod("geode.texture-loader");
    if (!textureLoader) {
        log::warn("RU Mod Descriptions: Texture Loader not installed, skipping font pack install");
        return;
    }

    auto packDir = textureLoader->getConfigDir() / "packs" / "CyrillicMarkdownFontPack";
    auto markerFile = packDir / "geode.loader" / "mdFont.fnt";

    if (fs::exists(markerFile)) {
        log::info("RU Mod Descriptions: Cyrillic font pack already installed");
        return;
    }

    auto sourceDir = Mod::get()->getResourcesDir() / "texturepack";
    if (!fs::exists(sourceDir)) {
        log::warn("RU Mod Descriptions: bundled font pack source missing at {}", sourceDir.string());
        return;
    }

    std::error_code ec;
    fs::create_directories(packDir, ec);
    fs::copy(
        sourceDir, packDir,
        fs::copy_options::recursive | fs::copy_options::overwrite_existing,
        ec
    );

    if (ec) {
        log::error("RU Mod Descriptions: failed to install font pack: {}", ec.message());
    } else {
        log::info("RU Mod Descriptions: Cyrillic font pack installed to {}", packDir.string());
    }
}

std::string translateViaGoogle(std::string const& chunk) {
    auto req = web::WebRequest();
    req.param("client", "gtx");
    req.param("sl", "en");
    req.param("tl", "ru");
    req.param("dt", "t");
    req.param("q", chunk);
    auto response = req.getSync("https://translate.googleapis.com/translate_a/single", Mod::get());

    if (!response.ok()) return "";

    auto json = response.json();
    if (!json) return "";

    auto root = json.unwrap();
    auto segmentsRes = root[0].asArray();
    if (!segmentsRes) return "";

    std::string result;
    for (auto const& segment : segmentsRes.unwrap()) {
        auto partRes = segment[0].asString();
        if (partRes) result += partRes.unwrap();
    }
    return result;
}

std::string translateViaMyMemory(std::string const& chunk) {
    auto req = web::WebRequest();
    req.param("q", chunk);
    req.param("langpair", "en|ru");
    auto response = req.getSync("https://api.mymemory.translated.net/get", Mod::get());

    if (!response.ok()) return "";

    auto json = response.json();
    if (!json) return "";

    auto translated = json.unwrap()["responseData"]["translatedText"].asString();
    if (!translated) return "";

    return translated.unwrap();
}

std::string translateChunk(std::string const& chunk, std::string const& translator) {
    std::string result;
    if (translator == "google") {
        result = translateViaGoogle(chunk);
    } else if (translator == "mymemory") {
        result = translateViaMyMemory(chunk);
    }

    if (result.empty()) {
        log::warn("RU Mod Descriptions: translation failed for a chunk via {}", translator);
        return chunk;
    }
    return result;
}

// Splits text into line-respecting chunks under the chosen translator's
// byte limit, translates each chunk with a separate request, and stitches
// the results back together with newlines.
std::string translateLong(std::string const& text, std::string const& translator) {
    size_t maxChunkBytes = (translator == "google") ? MAX_CHUNK_BYTES_GOOGLE : MAX_CHUNK_BYTES_MYMEMORY;

    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            lines.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }

    std::string result;
    std::string buffer;

    auto flush = [&]() {
        if (buffer.empty()) return;
        if (!result.empty()) result += "\n";
        result += translateChunk(buffer, translator);
        buffer.clear();
    };

    for (auto const& line : lines) {
        if (!buffer.empty() && buffer.size() + 1 + line.size() > maxChunkBytes) {
            flush();
        }

        if (line.size() > maxChunkBytes) {
            flush();
            if (!result.empty()) result += "\n";
            result += translateChunk(line.substr(0, maxChunkBytes), translator);
            continue;
        }

        if (!buffer.empty()) buffer += "\n";
        buffer += line;
    }
    flush();

    return result;
}

$on_mod(Loaded) {
    log::info("RU Mod Descriptions: registering listener");
    installFontPackIfNeeded();

    ModPopupUIEvent().listen(
        [](FLAlertLayer* popup, std::string_view modIDView, std::optional<Mod*> modOpt) -> bool {
            if (!Mod::get()->getSettingValue<bool>("enabled")) {
                return false;
            }

            std::string modID(modIDView);

            if (!popup) return false;

            auto mod = Loader::get()->getInstalledMod(modID);
            if (!mod) return false;

            auto textareaNode = popup->querySelector("description-container > textarea");
            auto textarea = typeinfo_cast<MDTextArea*>(textareaNode);
            if (!textarea) return false;

            auto detailsOpt = mod->getMetadata().getDetails();
            if (!detailsOpt.has_value()) return false;

            auto original = detailsOpt.value();
            if (original.empty()) return false;

            std::string translator = Mod::get()->getSettingValue<std::string>("translator");
            std::string cacheKey = translator + "|" + modID;

            std::string textToShow;

            if (translator == "none") {
                textToShow = original;
            } else if (auto cached = g_translationCache.find(cacheKey); cached != g_translationCache.end()) {
                log::info("RU Mod Descriptions: using cached translation");
                textToShow = cached->second;
            } else {
                log::info("RU Mod Descriptions: translating {} bytes via {}", original.size(), translator);
                std::vector<std::string> tags;
                auto protectedText = protectTags(original, tags);
                auto translatedProtected = translateLong(protectedText, translator);
                textToShow = restoreTags(translatedProtected, tags);
                g_translationCache[cacheKey] = textToShow;
            }

            // Feed the translated markdown straight back into the original
            // MDTextArea - now that the Cyrillic font pack is installed, it
            // renders headings, links, colors etc. natively, no need for
            // us to hide it and draw our own text anymore.
            textarea->setString(textToShow.c_str());

            return false;
        }
    ).leak();
}
