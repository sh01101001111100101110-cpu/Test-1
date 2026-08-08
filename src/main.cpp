#include <Geode/Geode.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/utils/web.hpp>
#include <algorithm>
#include <sstream>

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
// Google's unofficial endpoint tolerates much bigger requests, so fewer,
// larger requests are used when it's the chosen translator - arguably also
// less conspicuous than many small rapid-fire requests.
constexpr size_t MAX_CHUNK_BYTES_MYMEMORY = 450;
constexpr size_t MAX_CHUNK_BYTES_GOOGLE = 1800;

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

    // Google's response shape: [[["translated part","source part",...], ...], null, "en"]
    // The text can be split across multiple segments that need concatenating.
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

// Translates a single chunk using whichever translator the user picked in
// settings. Falls back to returning the chunk untranslated if the request
// fails, rather than losing the text entirely.
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
// the results back together with newlines. This lets us translate
// descriptions of any length, at the cost of one request per chunk.
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
            // A single line longer than the whole limit - flush what we
            // have, then translate this line by itself as a best effort.
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

// std::string::size() counts bytes, not characters - and Cyrillic letters
// are 2 bytes each in UTF-8. This counts actual codepoints instead, by only
// counting bytes that aren't UTF-8 continuation bytes (10xxxxxx).
size_t utf8Length(std::string const& s) {
    size_t count = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) count++;
    }
    return count;
}

// Manually wraps text to fit within maxWidthUnscaled (in the font's own
// unscaled coordinate space), by measuring an unwrapped test label to
// estimate average character width, then greedily wrapping word by word.
// We do this ourselves rather than relying on CCLabelBMFont's built-in
// width/alignment overload, since that didn't actually wrap correctly here.
std::string wrapText(std::string const& text, float maxWidthUnscaled) {
    if (text.empty()) return text;

    std::string result;
    std::istringstream sourceStream(text);
    std::string sourceLine;
    bool firstLine = true;

    while (std::getline(sourceStream, sourceLine)) {
        if (!firstLine) result += "\n";
        firstLine = false;

        if (sourceLine.empty()) continue;

        // Measure THIS line's own natural width. Since we already split on
        // '\n' above, this line is guaranteed to render as a single line,
        // giving an accurate average character width - unlike measuring
        // the whole multi-paragraph text at once, which was the bug.
        auto testLabel = CCLabelBMFont::create(sourceLine.c_str(), "PusiaCyrillic.fnt"_spr);
        float lineWidth = testLabel->getContentSize().width;
        size_t lineChars = utf8Length(sourceLine);
        float avgCharWidth = (lineChars > 0 && lineWidth > 0.f)
            ? (lineWidth / static_cast<float>(lineChars))
            : 10.f;
        int maxCharsPerLine = std::max(1, static_cast<int>(maxWidthUnscaled / avgCharWidth));

        std::istringstream wordStream(sourceLine);
        std::string word;
        std::string currentLine;
        bool firstWord = true;

        while (wordStream >> word) {
            std::string candidate = firstWord ? word : (currentLine + " " + word);
            if (!firstWord && static_cast<int>(utf8Length(candidate)) > maxCharsPerLine && !currentLine.empty()) {
                result += currentLine + "\n";
                currentLine = word;
            } else {
                currentLine = candidate;
            }
            firstWord = false;
        }
        result += currentLine;
    }

    return result;
}

void showLabel(CCNode* parent, CCPoint pos, CCSize size, std::string const& text) {
    if (!parent) return;

    // maxWidthUnscaled is in the font's own (unscaled) coordinate space, so
    // we divide the on-screen width by our scale to get the right value.
    float maxWidthUnscaled = size.width / LABEL_SCALE;
    auto wrapped = wrapText(text, maxWidthUnscaled);
    log::info("RU Mod Descriptions: wrapped result: [{}]", wrapped);

    auto label = CCLabelBMFont::create(wrapped.c_str(), "PusiaCyrillic.fnt"_spr);
    label->setAnchorPoint({0.f, 1.f});
    label->setScale(LABEL_SCALE);
    label->setColor({255, 255, 255});

    // How tall the translated text actually is once scaled down, so the
    // scroll area knows how far it should be able to scroll.
    float textHeight = label->getContentSize().height * LABEL_SCALE;
    float contentHeight = std::max(textHeight, size.height);

    auto scrollLayer = ScrollLayer::create(size);
    // pos is the top-left corner of the box; ScrollLayer positions itself
    // from its bottom-left corner, so we shift down by the box height.
    scrollLayer->setPosition({ pos.x, pos.y - size.height });
    scrollLayer->m_contentLayer->setContentSize({ size.width, contentHeight });
    label->setPosition({ 0.f, contentHeight });
    scrollLayer->m_contentLayer->addChild(label);
    scrollLayer->scrollToTop();

    parent->addChild(scrollLayer, 100);
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
                textToShow = translateLong(original, translator);
                g_translationCache[cacheKey] = textToShow;
            }

            showLabel(parent, topLeft, size, textToShow);

            return false;
        }
    ).leak();
}
