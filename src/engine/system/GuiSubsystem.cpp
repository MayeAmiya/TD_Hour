#include "GuiSubsystem.h"
#include "debug/debug.h"
#include "DrawFunc.h"
#include "MappedImageCollection.h"

GuiSubsystem::GuiSubsystem() {
    setName("Gui");
}

GuiSubsystem::~GuiSubsystem() {
    shutdown();
}

void GuiSubsystem::init() {
    TD_LOG_INFO("[Gui] Initializing...");

    gui::draw::initDrawFuncs();
    TD_LOG_INFO("[Gui] Draw functions registered");

    engine::MappedImageCollection::instance().load();
    TD_LOG_INFO("[Gui] Loaded {} mapped images", engine::MappedImageCollection::instance().getImageCount());
}

void GuiSubsystem::reset() {
    // MappedImageCollection persists across games
}

void GuiSubsystem::shutdown() {
    TD_LOG_INFO("[Gui] Shutdown");
}
