#include "LayoutCallbackRegistry.h"
#include "LayoutCallbacks.h"
#include "../../../core/constants/Paths.h"
#include "debug/debug.h"

namespace gui {

// Register only callbacks used by the retained in-game WND overlays. The
// launcher owns game-outside menus; accepting their callbacks here would
// silently recreate a second, non-functional shell entry path.

static void stubInit(ScreenGroup*) {}
static void stubUpdate(ScreenGroup*) {}
static void stubShutdown(ScreenGroup*, bool) {}

void registerDefaultLayoutCallbacks() {
    static const bool registered = (
    // In-game loading, replay, and score overlays. These are game-domain WNDs;
    // real data population will replace the stubs as gameplay systems come online.
    LayoutCallbackRegistry::instance().registerShutdown(CB_SINGLEPLAYER_LOAD_SHUTDOWN.data(), stubShutdown),
    LayoutCallbackRegistry::instance().registerShutdown(CB_CHALLENGE_LOAD_SHUTDOWN.data(), stubShutdown),
    LayoutCallbackRegistry::instance().registerInit(CB_POPUP_REPLAY_INIT.data(), stubInit),
    LayoutCallbackRegistry::instance().registerUpdate(CB_POPUP_REPLAY_UPDATE.data(), stubUpdate),
    LayoutCallbackRegistry::instance().registerShutdown(CB_POPUP_REPLAY_SHUTDOWN.data(), stubShutdown),
    LayoutCallbackRegistry::instance().registerInit(CB_SCORE_SCREEN_INIT.data(), stubInit),
    LayoutCallbackRegistry::instance().registerUpdate(CB_SCORE_SCREEN_UPDATE.data(), stubUpdate),
    LayoutCallbackRegistry::instance().registerShutdown(CB_SCORE_SCREEN_SHUTDOWN.data(), stubShutdown),
    LayoutCallbackRegistry::instance().registerInit(CB_INGAME_POPUP_MESSAGE_INIT.data(), stubInit),
    LayoutCallbackRegistry::instance().registerInit(CB_WOL_BUDDY_OVERLAY_INIT.data(), stubInit),
    LayoutCallbackRegistry::instance().registerUpdate(CB_WOL_BUDDY_OVERLAY_UPDATE.data(), stubUpdate),
    LayoutCallbackRegistry::instance().registerShutdown(CB_WOL_BUDDY_OVERLAY_SHUTDOWN.data(), stubShutdown),
    LayoutCallbackRegistry::instance().registerInit(CB_POPUP_COMMUNICATOR_INIT.data(), stubInit),
    LayoutCallbackRegistry::instance().registerShutdown(CB_POPUP_COMMUNICATOR_SHUTDOWN.data(), stubShutdown),
    LayoutCallbackRegistry::instance().registerInit(CB_GAMEINFO_WINDOW_INIT.data(), stubInit),

        true);
    (void)registered;
}

} // namespace gui
