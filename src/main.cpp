#include <Geode/Geode.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/utils/web.hpp>

using namespace geode::prelude;

// Which mods we've already processed, so re-opening the same popup
// doesn't re-trigger a translation request every time.
static std::unordered_set<std::string> g_processedModIDs;

// The scale we draw our custom-font label at. The font was baked at a large
// base size (80pt) for crisp text, so we shrink it down to fit the popup.
constexpr float LABEL_SCALE = 0.4f;

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
    ModPopupUIEvent().listen(
        [](FLAlertLayer* popup, std::string_view modIDView, std::optional<Mod*> modOpt) -> bool {
            if (!Mod::get()->getSettingValue<bool>("enabled")) {
                return false;
            }

            std::string modID(modIDView);
            if (g_processedModIDs.contains(modID)) {
                return false;
            }

            if (!popup || !modOpt.has_value()) {
                return false;
            }
            auto mod = modOpt.value();

            auto textarea = popup->querySelector("description-container > textarea");
            if (!textarea) return false;

            auto detailsRes = mod->getMetadata().getDetails();
            if (!detailsRes) return false;

            auto original = detailsRes.unwrap();
            if (original.empty()) return false;

            g_processedModIDs.insert(modID);

            // We can't fix MDTextArea's built-in font, so hide it and draw
            // our own text with our custom Cyrillic-capable font instead.
            auto pos = textarea->getPosition();
            auto size = textarea->getContentSize();
            auto parent = textarea->getParent();
            textarea->setVisible(false);

            std::string textToShow = original;

            // MyMemory's single-request limit is ~500 bytes. If the
            // description is longer than that, just show the original
            // English for now rather than nothing.
            if (original.size() <= 480) {
                auto req = web::WebRequest();
                req.param("q", original);
                req.param("langpair", "en|ru");
                auto response = req.getSync("https://api.mymemory.translated.net/get", Mod::get());

                if (response.ok()) {
                    auto json = response.json();
                    if (json) {
                        auto translated = json.unwrap()["responseData"]["translatedText"].asString();
                        if (translated) {
                            textToShow = translated.unwrap();
                        } else {
                            log::warn("RU Mod Descriptions: response JSON missing translatedText");
                        }
                    } else {
                        log::warn("RU Mod Descriptions: failed to parse response JSON");
                    }
                } else {
                    log::warn("RU Mod Descriptions: translation request failed");
                }
            } else {
                log::warn("RU Mod Descriptions: description too long to translate ({} bytes), showing original", original.size());
            }

            showLabel(parent, pos, size, textToShow);

            return false;
        }
    ).leak();
}
