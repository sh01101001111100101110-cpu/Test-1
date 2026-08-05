#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>

using namespace geode::prelude;

// This is the EXACT example from geode-sdk.org's homepage, unmodified,
// to rule out any signature mismatch on our end. If pressing "More Games"
// shows this popup, hooking works fine and the problem was specific to
// our MenuLayer::init() signature. If it doesn't, hooking itself is
// broken somehow for this GD version/setup.
class $modify(MoreGamesTest, MenuLayer) {
    void onMoreGames(CCObject*) {
        FLAlertLayer::create(
            "Geode",
            "Hello World from my Custom Mod!",
            "OK"
        )->show();
    }
};

class $modify(CyrillicTestMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        auto winSize = CCDirector::sharedDirector()->getWinSize();

        // STEP 1: prove the hook itself is firing at all, using GD's own
        // built-in font that we know 100% exists. If this doesn't show up,
        // the problem is with the hook/mod loading, not the custom font.
        auto proofLabel = CCLabelBMFont::create("MOD IS RUNNING", "bigFont.fnt");
        proofLabel->setScale(0.6f);
        proofLabel->setPosition(winSize.width / 2, winSize.height - 20);
        proofLabel->setColor({0, 255, 0});
        this->addChild(proofLabel, 100);

        // STEP 2: now test the custom Cyrillic font separately.
        // "PusiaCyrillic.fnt"_spr uses the font we defined in mod.json.
        auto label = CCLabelBMFont::create(
            "Привет мир! Проверка кириллицы (йцукен)",
            "PusiaCyrillic.fnt"_spr
        );

        if (label) {
            log::info("CyrillicTest: custom font label created successfully");
            label->setScale(0.5f);
            label->setPosition(winSize.width / 2, winSize.height - 40);
            label->setColor({255, 0, 0});
            this->addChild(label, 100);
        } else {
            // If our custom font failed to load, this uses GD's default
            // font instead, so we get SOME visible proof of what happened
            // rather than silent nothing.
            log::error("CyrillicTest: CCLabelBMFont::create returned nullptr - font not found");
            auto fallback = CCLabelBMFont::create("FONT FAILED", "bigFont.fnt");
            fallback->setScale(0.5f);
            fallback->setPosition(winSize.width / 2, winSize.height - 40);
            fallback->setColor({255, 0, 0});
            this->addChild(fallback, 100);
        }

        return true;
    }
};
