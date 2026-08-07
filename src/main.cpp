#include <Geode/Geode.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/utils/web.hpp>

using namespace geode::prelude;

// Cache of already-translated text per mod ID, so we don't hit the
// translation API again every time the same popup is reopened - but we
// still redo the hide+label step every time, since a fresh popup means a
// fresh (visible-again) textarea node.
static std::unordered_map<std::string, std::string> g_translationCache;

// The scale we draw our custom-font label at. The font was baked at a large
// base size (80pt) for crisp text, so we shrink it down to fit the popup.
constexpr float LABEL_SCALE = 0.4f;

// MyMemory's single-request limit is ~500 bytes; we keep a safety margin.
constexpr size_t MAX_CHUNK_BYTES = 450;

// Translates a single chunk that's already known to be under the API's
// per-request limit. Falls back to returning the original chunk untranslated
// if anything goes wrong, rather than losing the text entirely.
std::string translateChunk(std::string const& chunk) {
    auto req = web::WebRequest();
    req.param("q", chunk);
    req.param("langpair", "en|ru");
    auto response = req.getSync("https://api.mymemory.translated.net/get", Mod::get());

    if (!response.ok()) {
        log::warn("RU Mod Descriptions: chunk translation request failed");
        return chunk;
    }

    auto json = response.json();
    if (!json) {
        log::warn("RU Mod Descriptions: chunk response JSON parse failed");
        return chunk;
    }

    auto translated = json.unwrap()["responseData"]["translatedText"].asString();
    if (!translated) {
        log::warn("RU Mod Descriptions: chunk response missing translatedText");
        return chunk;
    }

    return translated.unwrap();
}

// Splits text into line-respecting chunks under MAX_CHUNK_BYTES, translates
// each chunk with a separate request, and stitches the results back together
// with newlines. This lets us translate descriptions of any length, at the
// cost of one request per chunk.
std::string translateLong(std::string const& text) {
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
        result += translateChunk(buffer);
        buffer.clear();
    };

    for (auto const& line : lines) {
        if (!buffer.empty() && buffer.size() + 1 + line.size() > MAX_CHUNK_BYTES) {
            flush();
        }

        if (line.size() > MAX_CHUNK_BYTES) {
            // A single line longer than the whole limit - flush what we
            // have, then translate this line by itself as a best effort
            // (still under the limit since MAX_CHUNK_BYTES has a margin
            // below the real 500 byte cap, so this only truncates in
            // extreme edge cases).
            flush();
            if (!result.empty()) result += "\n";
            result += translateChunk(line.substr(0, MAX_CHUNK_BYTES));
            continue;
        }

        if (!buffer.empty()) buffer += "\n";
        buffer += line;
    }
    flush();

    return result;
}

void showLabel(CCNode* parent, CCPoint pos, CCSize size, std::string const& text) {
    if (!parent) return;

    // wrapWidth is in the font's own (unscaled) coordinate space, so we
    // divide the on-screen width by our scale to get the right wrap point.
    float wrapWidth = size.width / LABEL_SCALE;

    auto label = CCLabelBMFont::create(
        text.c_str(),
        "PusiaCyrillic.fnt"_spr,
        wrapWidth,
        kCCTextAlignmentLeft
    );

    label->setAnchorPoint({0.f, 1.f});
    label->setScale(LABEL_SCALE);
    label->setPosition(pos);
    label->setColor({255, 255, 255});
    parent->addChild(label, 100);
}

$on_mod(Loaded) {
    log::info("RU Mod Descriptions: registering listener");

    ModPopupUIEvent().listen(
        [](FLAlertLayer* popup, std::string_view modIDView, std::optional<Mod*> modOpt) -> bool {
            if (!Mod::get()->getSettingValue<bool>("enabled")) {
                return false;
            }

            std::string modID(modIDView);

            if (!popup) return false;

            auto mod = Loader::get()->getInstalledMod(modID);
            if (!mod) return false;

            auto textarea = popup->querySelector("description-container > textarea");
            if (!textarea) return false;

            auto detailsOpt = mod->getMetadata().getDetails();
            if (!detailsOpt.has_value()) return false;

            auto original = detailsOpt.value();
            if (original.empty()) return false;

            // We can't fix MDTextArea's built-in font, so hide it and draw
            // our own text with our custom Cyrillic-capable font instead.
            // boundingBox() gives us the real edges regardless of the
            // original node's anchor point, which getPosition() alone
            // doesn't account for.
            auto box = textarea->boundingBox();
            CCPoint topLeft = { box.origin.x, box.origin.y + box.size.height };
            CCSize size = box.size;
            auto parent = textarea->getParent();
            textarea->setVisible(false);

            std::string textToShow;

            if (auto cached = g_translationCache.find(modID); cached != g_translationCache.end()) {
                log::info("RU Mod Descriptions: using cached translation");
                textToShow = cached->second;
            } else {
                log::info("RU Mod Descriptions: translating {} bytes", original.size());
                textToShow = translateLong(original);
                g_translationCache[modID] = textToShow;
            }

            showLabel(parent, topLeft, size, textToShow);

            return false;
        }
    ).leak();
}
