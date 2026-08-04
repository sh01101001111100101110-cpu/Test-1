#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>

using namespace geode::prelude;

class $modify(CyrillicTestMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        // "PusiaCyrillic.fnt"_spr uses the font we defined in mod.json.
        auto label = CCLabelBMFont::create(
            "Привет мир! Проверка кириллицы (йцукен)",
            "PusiaCyrillic.fnt"_spr
        );

        // Make it reasonably sized and place it near the top of the screen,
        // clear of the usual buttons, so it's easy to spot.
        label->setScale(0.5f);
        label->setPosition(
            CCDirector::sharedDirector()->getWinSize().width / 2,
            CCDirector::sharedDirector()->getWinSize().height - 40
        );
        this->addChild(label, 100);

        return true;
    }
};
