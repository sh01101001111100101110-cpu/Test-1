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

std::string buildTranslateUrl(std::string const& text) {
    auto encoded = web::urlEncode(text);
    return fmt::format(
        "https://api.mymemory.translated.net/get?q={}&langpair=en|ru",
        encoded
    );
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
    new EventListener<EventFilter<ModPopupUIEvent>>(+[](ModPopupUIEvent* event) {
        if (!Mod::get()->getSettingValue<bool>("enabled")) {
            return ListenerResult::Propagate;
        }

        auto modID = event->getModID();
        if (g_processedModIDs.contains(modID)) {
            return ListenerResult::Propagate;
        }

        auto popup = event->getPopup();
        if (!popup) return ListenerResult::Propagate;

        auto textarea = popup->querySelector("description-container > textarea");
        if (!textarea) return ListenerResult::Propagate;

        auto mod = Loader::get()->getInstalledMod(modID);
        if (!mod) return ListenerResult::Propagate;

        auto detailsRes = mod->getMetadata().getDetails();
        if (!detailsRes) return ListenerResult::Propagate;

        auto original = detailsRes.unwrap();
        if (original.empty()) return ListenerResult::Propagate;

        g_processedModIDs.insert(modID);

        // We can't fix MDTextArea's built-in font, so hide it and draw our
        // own text with our custom Cyrillic-capable font instead.
        auto pos = textarea->getPosition();
        auto size = textarea->getContentSize();
        auto parent = textarea->getParent();
        textarea->setVisible(false);

        // MyMemory's single-request limit is ~500 bytes. If the description
        // is longer than that, just show the original English for now
        // rather than nothing - splitting into chunks is a later step.
        if (original.size() > 480) {
            log::warn("RU Mod Descriptions: description too long to translate ({} bytes), showing original", original.size());
            showLabel(parent, pos, size, original);
            return ListenerResult::Propagate;
        }

        auto url = buildTranslateUrl(original);
        web::WebRequest().get(url).listen(
            [parent, pos, size, original](web::WebResponse* response) {
                std::string textToShow = original;

                if (response->ok()) {
                    auto json = response->json();
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

                showLabel(parent, pos, size, textToShow);
            }
        );

        return ListenerResult::Propagate;
    });
}
