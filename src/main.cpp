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
            log::info("RU Mod Descriptions: event fired for modID={}", std::string(modIDView));

            if (!Mod::get()->getSettingValue<bool>("enabled")) {
                log::info("RU Mod Descriptions: disabled via setting, bailing");
                return false;
            }

            std::string modID(modIDView);

            if (!popup) {
                log::warn("RU Mod Descriptions: popup is null, bailing");
                return false;
            }

            auto mod = Loader::get()->getInstalledMod(modID);
            if (!mod) {
                log::warn("RU Mod Descriptions: no installed mod found for {}, bailing", modID);
                return false;
            }

            auto textarea = popup->querySelector("description-container > textarea");
            if (!textarea) {
                log::warn("RU Mod Descriptions: querySelector found no textarea, bailing");
                return false;
            }
            log::info("RU Mod Descriptions: found textarea node");

            auto detailsOpt = mod->getMetadata().getDetails();
            if (!detailsOpt.has_value()) {
                log::warn("RU Mod Descriptions: getDetails() has no value, bailing");
                return false;
            }

            auto original = detailsOpt.value();
            if (original.empty()) {
                log::warn("RU Mod Descriptions: details string is empty, bailing");
                return false;
            }
            log::info("RU Mod Descriptions: got details, {} bytes", original.size());

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
            log::info("RU Mod Descriptions: hid original textarea, topLeft=({},{}) size=({},{})", topLeft.x, topLeft.y, size.width, size.height);

            std::string textToShow = original;

            if (auto cached = g_translationCache.find(modID); cached != g_translationCache.end()) {
                log::info("RU Mod Descriptions: using cached translation");
                textToShow = cached->second;
            }
            // MyMemory's single-request limit is ~500 bytes. If the
            // description is longer than that, just show the original
            // English for now rather than nothing.
            else if (original.size() <= 480) {
                auto req = web::WebRequest();
                req.param("q", original);
                req.param("langpair", "en|ru");
                log::info("RU Mod Descriptions: sending translation request");
                auto response = req.getSync("https://api.mymemory.translated.net/get", Mod::get());

                if (response.ok()) {
                    log::info("RU Mod Descriptions: response ok");
                    auto json = response.json();
                    if (json) {
                        auto translated = json.unwrap()["responseData"]["translatedText"].asString();
                        if (translated) {
                            textToShow = translated.unwrap();
                            g_translationCache[modID] = textToShow;
                            log::info("RU Mod Descriptions: translated text = {}", textToShow);
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

            showLabel(parent, topLeft, size, textToShow);
            log::info("RU Mod Descriptions: showLabel called");

            return false;
        }
    ).leak();
}
