#include <Geode/Geode.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/utils/web.hpp>
#include <algorithm>
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

using namespace geode::prelude;

// Copies our bundled Cyrillic font pack into Texture Loader's packs folder
// on first run, so descriptions actually render in Cyrillic without the
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

// Cache of already-translated text per mod ID, so we don't hit the
// translation API again every time the same popup is reopened - but we
// still redo the hide+label step every time, since a fresh popup means a
// fresh (visible-again) textarea node.
static std::unordered_map<std::string, std::string> g_translationCache;

// The scale we draw body text at. The font was baked at a large base size
// (80pt) for crisp text, so we shrink it down to fit the popup. Headings
// are drawn bigger, scaled relative to this.
constexpr float LABEL_SCALE = 0.4f;

// MyMemory's single-request limit is ~500 bytes; we keep a safety margin.
// Google's unofficial endpoint tolerates much bigger requests, so fewer,
// larger requests are used when it's the chosen translator.
constexpr size_t MAX_CHUNK_BYTES_MYMEMORY = 450;
constexpr size_t MAX_CHUNK_BYTES_GOOGLE = 1800;

// Geode's about.md supports extra tags Discord/GitHub markdown doesn't,
// like <c-RRGGBB>colored text</c> and <mod:some.mod.id> links. We don't
// render colors or mod cards (yet), but we strip the tag syntax itself so
// it doesn't show up as literal "<c-dddddd>" text in the output.
std::string stripAngleTags(std::string const& text) {
    std::string result;
    bool inTag = false;
    for (char c : text) {
        if (c == '<') { inTag = true; continue; }
        if (c == '>') { inTag = false; continue; }
        if (!inTag) result += c;
    }
    return result;
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

std::string trim(std::string const& s) {
    size_t start = s.find_first_not_of(" \t\r");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r");
    return s.substr(start, end - start + 1);
}

// Manually wraps text (a single paragraph, no embedded newlines) to fit
// within maxWidthUnscaled (in the font's own unscaled coordinate space), by
// measuring an unwrapped test label to estimate average character width,
// then greedily wrapping word by word. We do this ourselves rather than
// relying on CCLabelBMFont's built-in width/alignment overload, since that
// didn't actually wrap correctly here.
std::string wrapText(std::string const& text, std::string const& font, float maxWidthUnscaled) {
    if (text.empty()) return text;

    auto testLabel = CCLabelBMFont::create(text.c_str(), font.c_str());
    float lineWidth = testLabel->getContentSize().width;
    size_t lineChars = utf8Length(text);
    float avgCharWidth = (lineChars > 0 && lineWidth > 0.f)
        ? (lineWidth / static_cast<float>(lineChars))
        : 10.f;
    int maxCharsPerLine = std::max(1, static_cast<int>(maxWidthUnscaled / avgCharWidth));

    std::istringstream wordStream(text);
    std::string word;
    std::string currentLine;
    std::string result;
    bool firstWord = true;

    while (wordStream >> word) {
        std::string candidate = firstWord ? word : (currentLine + " " + word);
        if (!firstWord && static_cast<int>(utf8Length(candidate)) > maxCharsPerLine && !currentLine.empty()) {
            if (!result.empty()) result += "\n";
            result += currentLine;
            currentLine = word;
        } else {
            currentLine = candidate;
        }
        firstWord = false;
    }
    if (!result.empty()) result += "\n";
    result += currentLine;

    return result;
}

// A minimal markdown block parser covering just what Geode about.md files
// commonly use: #, ##, ### headings and --- / ___ horizontal rules. Colored
// text and mod-link tags are stripped elsewhere rather than rendered.
enum class BlockType { Heading1, Heading2, Heading3, Paragraph, Rule };

struct Block {
    BlockType type;
    std::string text;
};

std::vector<Block> parseBlocks(std::string const& text) {
    std::vector<Block> blocks;
    std::istringstream stream(text);
    std::string rawLine;

    while (std::getline(stream, rawLine)) {
        std::string line = trim(rawLine);

        bool isRule = line.size() >= 3 && (
            line.find_first_not_of('-') == std::string::npos ||
            line.find_first_not_of('_') == std::string::npos
        );
        if (isRule) {
            blocks.push_back({ BlockType::Rule, "" });
            continue;
        }

        if (line.empty()) {
            blocks.push_back({ BlockType::Paragraph, "" });
            continue;
        }

        int level = 0;
        size_t i = 0;
        while (i < line.size() && line[i] == '#' && level < 3) { level++; i++; }

        if (level > 0 && i < line.size() && line[i] == ' ') {
            std::string headingText = trim(line.substr(i + 1));
            BlockType t = level == 1 ? BlockType::Heading1
                        : level == 2 ? BlockType::Heading2
                        : BlockType::Heading3;
            blocks.push_back({ t, headingText });
            continue;
        }

        blocks.push_back({ BlockType::Paragraph, line });
    }

    return blocks;
}

void showLabel(CCNode* parent, CCPoint pos, CCSize size, std::string const& text) {
    if (!parent) return;

    auto blocks = parseBlocks(text);

    auto scrollLayer = ScrollLayer::create(size);
    // pos is the top-left corner of the box; ScrollLayer positions itself
    // from its bottom-left corner, so we shift down by the box height.
    scrollLayer->setPosition({ pos.x, pos.y - size.height });

    struct RenderedBlock {
        CCNode* node = nullptr;
        float height = 0.f;
    };
    std::vector<RenderedBlock> rendered;
    constexpr float blockSpacing = 6.f;
    float totalHeight = 0.f;

    for (auto const& block : blocks) {
        if (block.type == BlockType::Rule) {
            auto rule = CCLayerColor::create({255, 255, 255, 120}, size.width, 2.f);
            rendered.push_back({ rule, 2.f });
            totalHeight += 2.f + blockSpacing;
            continue;
        }

        if (block.text.empty()) {
            // Blank line - just vertical spacing between paragraphs.
            rendered.push_back({ nullptr, 10.f });
            totalHeight += 10.f + blockSpacing;
            continue;
        }

        float scale = LABEL_SCALE;
        if (block.type == BlockType::Heading1) scale = LABEL_SCALE * 1.7f;
        else if (block.type == BlockType::Heading2) scale = LABEL_SCALE * 1.4f;
        else if (block.type == BlockType::Heading3) scale = LABEL_SCALE * 1.15f;

        float maxWidthUnscaled = size.width / scale;
        auto wrapped = wrapText(block.text, "PusiaCyrillic.fnt"_spr, maxWidthUnscaled);

        auto label = CCLabelBMFont::create(wrapped.c_str(), "PusiaCyrillic.fnt"_spr);
        label->setAnchorPoint({0.f, 1.f});
        label->setScale(scale);
        label->setColor({255, 255, 255});

        float h = label->getContentSize().height * scale;
        rendered.push_back({ label, h });
        totalHeight += h + blockSpacing;
    }

    float contentHeight = std::max(totalHeight, size.height);
    scrollLayer->m_contentLayer->setContentSize({ size.width, contentHeight });

    float cursorY = contentHeight;
    for (auto const& rb : rendered) {
        if (rb.node) {
            if (auto rule = typeinfo_cast<CCLayerColor*>(rb.node)) {
                rule->setPosition({ 0.f, cursorY - rb.height });
            } else {
                rb.node->setPosition({ 0.f, cursorY });
            }
            scrollLayer->m_contentLayer->addChild(rb.node);
        }
        cursorY -= rb.height + blockSpacing;
    }

    scrollLayer->scrollToTop();
    parent->addChild(scrollLayer, 100);
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

            auto textarea = popup->querySelector("description-container > textarea");
            if (!textarea) return false;

            auto detailsOpt = mod->getMetadata().getDetails();
            if (!detailsOpt.has_value()) return false;

            auto original = stripAngleTags(detailsOpt.value());
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
