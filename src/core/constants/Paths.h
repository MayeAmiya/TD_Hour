#pragma once

#include "container/container_types.h"
// ── Configuration File Paths ────────────────────────────────────────────────

static constexpr container::StringView GAME_OPTIONS_INI        = "GameOptions.ini";
static constexpr container::StringView SKIRMISH_INI            = "Skirmish.ini";
static constexpr container::StringView OPTIONS_INI             = "Options.ini";
static constexpr container::StringView USER_SAVE_ROOT          = "save";
static constexpr container::StringView USER_REPLAY_ROOT        = "replays";
static constexpr container::StringView USER_MAP_ROOT           = "maps";
// User maps must not share the official `maps/...` VFS namespace.  ZH keeps
// installed Maps and UserData\Maps as distinct real/portable roots; preserving
// that distinction also makes a selected map source stable after VFS winner
// resolution.
static constexpr container::StringView USER_MAP_VFS_ROOT       = "user/maps";
// Optional loose-art fallbacks used only after installed locale/common Art
// candidates miss. Keeping them in isolated VFS namespaces prevents an
// arbitrary UserData tree from becoming a global content override.
static constexpr container::StringView USER_W3D_VFS_ROOT       = "user/w3d";
static constexpr container::StringView USER_TEXTURE_VFS_ROOT   = "user/textures";
static constexpr container::StringView USER_MAP_PREVIEW_VFS_ROOT = "user/mappreviews";
static constexpr container::StringView USER_SESSION_ROOT       = "sessions";

// ── Data File Paths (VFS) ───────────────────────────────────────────────────

static constexpr container::StringView CAMPAIGN_INI            = "data/ini/campaign.ini";
static constexpr container::StringView CHALLENGEMODE_INI       = "data/ini/challengemode.ini";
static constexpr container::StringView MULTIPLAYER_INI         = "data/ini/multiplayer.ini";
static constexpr container::StringView PLAYERTEMPLATE_INI      = "data/ini/playertemplate.ini";
static constexpr container::StringView MAPPED_IMAGES_DIR       = "data/ini/mappedimages";
static constexpr container::StringView MAP_CACHE_DIR           = "data/maps/skirmish";
static constexpr container::StringView MAP_NAME_TEMPLATE       = "data/maps/skirmish/MapName.map";

// ── Archive Paths ───────────────────────────────────────────────────────────

static constexpr container::StringView INIZH_BIG_WINDOWS       = "data\\ini\\inizh.big";
static constexpr container::StringView INIZH_BIG_UNIX          = "data/ini/inizh.big";

// ── WND File Paths ──────────────────────────────────────────────────────────

static constexpr container::StringView MAINMENU_WND            = "window/menus/mainmenu.wnd";
static constexpr container::StringView OPTIONSMENU_WND         = "window/menus/optionsmenu.wnd";
static constexpr container::StringView CREDITSMENU_WND         = "window/menus/creditsmenu.wnd";
static constexpr container::StringView SKIRMISHGAMEOPTIONS_WND = "window/menus/skirmishgameoptionsmenu.wnd";
static constexpr container::StringView SINGLEPLAYERMENU_WND    = "window/menus/singleplayermenu.wnd";
static constexpr container::StringView CHALLENGEMENU_WND       = "window/menus/challengemenu.wnd";
static constexpr container::StringView LANLOBBYMENU_WND        = "window/menus/lanlobbymenu.wnd";
static constexpr container::StringView SAVELOAD_WND            = "window/menus/saveload.wnd";
static constexpr container::StringView REPLAYMENU_WND          = "window/menus/replaymenu.wnd";
static constexpr container::StringView DIRECTCONNECT_WND       = "window/menus/networkdirectconnect.wnd";
static constexpr container::StringView SKIRMISHMAPSELECT_WND   = "window/menus/skirmishmapselectmenu.wnd";
static constexpr container::StringView LANMAPSELECT_WND        = "window/menus/lanmapselectmenu.wnd";
static constexpr container::StringView HOSTGAME_WND            = "window/menus/popuphostgame.wnd";
static constexpr container::StringView LANGAMEOPTIONS_WND      = "window/menus/langameoptionsmenu.wnd";

static constexpr container::StringView CONTROLBAR_WND          = "ControlBar.wnd";
static constexpr container::StringView CONTROLBAR_WND_VFS      = "window/controlbar.wnd";
static constexpr container::StringView GENERALS_EXP_WND        = "GeneralsExpPoints.wnd";
static constexpr container::StringView GENERALS_EXP_WND_VFS    = "window/generalsexppoints.wnd";
static constexpr container::StringView CONTROLBAR_TOOLTIP_WND  = "ControlBarPopupDescription.wnd";
static constexpr container::StringView CONTROLBAR_TOOLTIP_WND_VFS = "window/controlbarpopupdescription.wnd";
static constexpr container::StringView GENPOWERS_US_WND        = "GenPowersShortcutBarUS.wnd";
static constexpr container::StringView GENPOWERS_US_WND_VFS    = "window/genpowersshortcutbarus.wnd";
static constexpr container::StringView GENPOWERS_CHINA_WND     = "GenPowersShortcutBarChina.wnd";
static constexpr container::StringView GENPOWERS_CHINA_WND_VFS = "window/genpowersshortcutbarchina.wnd";
static constexpr container::StringView GENPOWERS_GLA_WND       = "GenPowersShortcutBarGLA.wnd";
static constexpr container::StringView GENPOWERS_GLA_WND_VFS   = "window/genpowersshortcutbargla.wnd";
static constexpr container::StringView QUITMENU_WND            = "Menus/QuitMenu.wnd";
static constexpr container::StringView QUITMENU_WND_VFS        = "window/menus/quitmenu.wnd";
static constexpr container::StringView QUITNOSAVE_WND          = "Menus/QuitNoSave.wnd";
static constexpr container::StringView QUITNOSAVE_WND_VFS      = "window/menus/quitnosave.wnd";
static constexpr container::StringView POPUP_SAVELOAD_WND      = "Menus/PopupSaveLoad.wnd";
static constexpr container::StringView POPUP_SAVELOAD_WND_VFS  = "window/menus/popupsaveload.wnd";
static constexpr container::StringView INGAME_OPTIONS_WND      = "Menus/OptionsMenu.wnd";
static constexpr container::StringView INGAME_OPTIONS_WND_VFS  = "window/menus/optionsmenu.wnd";
static constexpr container::StringView INGAME_CHAT_WND         = "InGameChat.wnd";
static constexpr container::StringView INGAME_CHAT_WND_VFS     = "window/ingamechat.wnd";
static constexpr container::StringView SINGLEPLAYER_LOADSCREEN_WND = "SinglePlayerLoadScreen.wnd";
static constexpr container::StringView SINGLEPLAYER_LOADSCREEN_WND_VFS = "window/menus/singleplayerloadscreen.wnd";
static constexpr container::StringView MULTIPLAYER_LOADSCREEN_WND = "MultiplayerLoadScreen.wnd";
static constexpr container::StringView MULTIPLAYER_LOADSCREEN_WND_VFS = "window/menus/multiplayerloadscreen.wnd";
static constexpr container::StringView CHALLENGE_LOADSCREEN_WND = "ChallengeLoadScreen.wnd";
static constexpr container::StringView CHALLENGE_LOADSCREEN_WND_VFS = "window/menus/challengeloadscreen.wnd";
static constexpr container::StringView SHELLGAME_LOADSCREEN_WND = "ShellGameLoadScreen.wnd";
static constexpr container::StringView SHELLGAME_LOADSCREEN_WND_VFS = "window/menus/shellgameloadscreen.wnd";
static constexpr container::StringView GAMESPY_LOADSCREEN_WND = "GameSpyLoadScreen.wnd";
static constexpr container::StringView GAMESPY_LOADSCREEN_WND_VFS = "window/menus/gamespyloadscreen.wnd";
static constexpr container::StringView DISCONNECTSCREEN_WND    = "DisconnectScreen.wnd";
static constexpr container::StringView DISCONNECTSCREEN_WND_VFS = "window/menus/disconnectscreen.wnd";
static constexpr container::StringView REPLAY_CONTROL_WND      = "ReplayControl.wnd";
static constexpr container::StringView REPLAY_CONTROL_WND_VFS  = "window/replaycontrol.wnd";
static constexpr container::StringView POPUP_REPLAY_WND        = "PopupReplay.wnd";
static constexpr container::StringView POPUP_REPLAY_WND_VFS    = "window/menus/popupreplay.wnd";
static constexpr container::StringView OBSERVER_QUIT_WND       = "ObserverQuit.wnd";
static constexpr container::StringView OBSERVER_QUIT_WND_VFS   = "window/menus/observerquit.wnd";
static constexpr container::StringView SCORESCREEN_WND         = "Menus/ScoreScreen.wnd";
static constexpr container::StringView SCORESCREEN_WND_VFS     = "window/menus/scorescreen.wnd";
static constexpr container::StringView DEFEAT_WND              = "Menus/Defeat.wnd";
static constexpr container::StringView DEFEAT_WND_VFS          = "window/menus/defeat.wnd";
static constexpr container::StringView LOCAL_DEFEAT_WND        = "Menus/LocalDefeat.wnd";
static constexpr container::StringView LOCAL_DEFEAT_WND_VFS    = "window/menus/localdefeat.wnd";
static constexpr container::StringView VICTORIOUS_WND          = "Menus/Victorious.wnd";
static constexpr container::StringView VICTORIOUS_WND_VFS      = "window/menus/victorious.wnd";
static constexpr container::StringView INGAME_POPUP_MESSAGE_WND = "InGamePopupMessage.wnd";
static constexpr container::StringView INGAME_POPUP_MESSAGE_WND_VFS = "window/ingamepopupmessage.wnd";
static constexpr container::StringView DIPLOMACY_WND           = "Diplomacy.wnd";
static constexpr container::StringView DIPLOMACY_WND_VFS       = "window/diplomacy.wnd";
static constexpr container::StringView POPUP_COMMUNICATOR_WND  = "Menus/PopupCommunicator.wnd";
static constexpr container::StringView POPUP_COMMUNICATOR_WND_VFS = "window/menus/popupcommunicator.wnd";
static constexpr container::StringView GAMEINFO_WINDOW_WND     = "Menus/GameInfoWindow.wnd";
static constexpr container::StringView GAMEINFO_WINDOW_WND_VFS = "window/menus/gameinfowindow.wnd";
static constexpr container::StringView ESTABLISH_CONNECTIONS_WND = "Menus/EstablishConnectionsScreen.wnd";
static constexpr container::StringView ESTABLISH_CONNECTIONS_WND_VFS = "window/menus/establishconnectionsscreen.wnd";
static constexpr container::StringView MAP_TRANSFER_WND        = "Menus/MapTransferScreen.wnd";
static constexpr container::StringView MAP_TRANSFER_WND_VFS    = "window/menus/maptransferscreen.wnd";
static constexpr container::StringView CRC_MISMATCH_WND        = "Menus/CRCMismatch.wnd";
static constexpr container::StringView CRC_MISMATCH_WND_VFS    = "window/menus/crcmismatch.wnd";
static constexpr container::StringView MESSAGE_BOX_WND         = "Menus/MessageBox.wnd";
static constexpr container::StringView MESSAGE_BOX_WND_VFS     = "window/menus/messagebox.wnd";
static constexpr container::StringView QUIT_MESSAGE_BOX_WND    = "Menus/QuitMessageBox.wnd";
static constexpr container::StringView QUIT_MESSAGE_BOX_WND_VFS = "window/menus/quitmessagebox.wnd";
static constexpr container::StringView BLANK_WINDOW_WND        = "Menus/BlankWindow.wnd";
static constexpr container::StringView BLANK_WINDOW_WND_VFS    = "window/menus/blankwindow.wnd";

// ── INI Section Names ───────────────────────────────────────────────────────

static constexpr container::StringView SECTION_GAME            = "game";
static constexpr container::StringView SECTION_SKIRMISH        = "skirmish";
static constexpr container::StringView SECTION_SLOT0           = "Slot0";
static constexpr container::StringView SECTION_SLOT            = "Slot";
static constexpr container::StringView SECTION_CAMPAIGN        = "campaign";
static constexpr container::StringView SECTION_FONTS           = "Fonts";
static constexpr container::StringView SECTION_PLAYERS         = "Players";
static constexpr container::StringView SECTION_COLORS          = "Colors";
static constexpr container::StringView SECTION_CASH            = "Cash";
static constexpr container::StringView SECTION_SETTINGS        = "Settings";
static constexpr container::StringView SECTION_PLAYER_TEMPLATE = "PlayerTemplate";
static constexpr container::StringView SECTION_MULTIPLAYER_COLOR = "MultiplayerColor";
static constexpr container::StringView SECTION_MULTIPLAYER_CASH = "MultiplayerStartingMoneyChoice";
static constexpr container::StringView SECTION_MULTIPLAYER_SETTINGS = "MultiplayerSettings";
static constexpr container::StringView SECTION_ONLINE_CHAT_COLORS = "OnlineChatColors";

// ── INI Key Names ───────────────────────────────────────────────────────────

static constexpr container::StringView KEY_GENERALS_DATAPATH   = "generalsdatapath";
static constexpr container::StringView KEY_ZEROHOUR_DATAPATH   = "zerohourdatapath";
static constexpr container::StringView KEY_MOD_DATAPATH        = "moddatapath";
static constexpr container::StringView KEY_LOCALE_DATAPATH     = "localedatapath";
static constexpr container::StringView KEY_MAP                 = "Map";
static constexpr container::StringView KEY_STARTING_CASH       = "StartingCash";
static constexpr container::StringView KEY_SUPERWEAPON_RESTRICTED = "SuperweaponRestricted";
static constexpr container::StringView KEY_OLD_FACTIONS_ONLY   = "OldFactionsOnly";
static constexpr container::StringView KEY_FPS                 = "FPS";
static constexpr container::StringView KEY_SEED                = "Seed";
static constexpr container::StringView KEY_STATE               = "State";
static constexpr container::StringView KEY_COLOR               = "Color";
static constexpr container::StringView KEY_START_POS           = "StartPos";
static constexpr container::StringView KEY_PLAYER_TEMPLATE     = "PlayerTemplate";
static constexpr container::StringView KEY_TEAM                = "Team";
static constexpr container::StringView KEY_NAME                = "Name";
static constexpr container::StringView KEY_CAMPAIGN_DIFFICULTY = "CampaignDifficulty";
static constexpr container::StringView KEY_VALUE               = "Value";
static constexpr container::StringView KEY_DEFAULT             = "Default";
static constexpr container::StringView KEY_SIDE                = "Side";
static constexpr container::StringView KEY_BASESIDE            = "BaseSide";
static constexpr container::StringView KEY_DISPLAY_NAME        = "DisplayName";
static constexpr container::StringView KEY_PLAYABLE_SIDE       = "PlayableSide";
static constexpr container::StringView KEY_OLD_FACTION         = "OldFaction";
static constexpr container::StringView KEY_RGB_F_COLOR         = "RGBFColor";
static constexpr container::StringView KEY_RGB_COLOR           = "RGBColor";
static constexpr container::StringView KEY_RGB_NIGHT_COLOR     = "RGBNightColor";
static constexpr container::StringView KEY_TOOLTIP_NAME        = "TooltipName";

// ── File Extensions ─────────────────────────────────────────────────────────

static constexpr container::StringView EXT_TGA  = ".tga";
static constexpr container::StringView EXT_DDS  = ".dds";
static constexpr container::StringView EXT_BIG  = ".big";
static constexpr container::StringView EXT_MAP  = ".map";
static constexpr container::StringView EXT_REPLAY = ".rep";
static constexpr container::StringView EXT_SAVE = ".sav";
static constexpr container::StringView EXT_INI  = ".ini";
static constexpr container::StringView EXT_WND  = ".wnd";

// ── Screen Factory Callback Names ───────────────────────────────────────────

static constexpr container::StringView CB_OPTIONS_INIT     = "OptionsMenuInit";
static constexpr container::StringView CB_OPTIONS_UPDATE   = "OptionsMenuUpdate";
static constexpr container::StringView CB_OPTIONS_SHUTDOWN = "OptionsMenuShutdown";
static constexpr container::StringView CB_SP_INIT          = "SinglePlayerMenuInit";
static constexpr container::StringView CB_SP_UPDATE        = "SinglePlayerMenuUpdate";
static constexpr container::StringView CB_SP_SHUTDOWN      = "SinglePlayerMenuShutdown";
static constexpr container::StringView CB_SKIRMISH_INIT    = "SkirmishGameOptionsMenuInit";
static constexpr container::StringView CB_SKIRMISH_UPDATE  = "SkirmishGameOptionsMenuUpdate";
static constexpr container::StringView CB_SKIRMISH_SHUTDOWN= "SkirmishGameOptionsMenuShutdown";
static constexpr container::StringView CB_SAVELOAD_INIT    = "SaveLoadMenuInit";
static constexpr container::StringView CB_SAVELOAD_UPDATE  = "SaveLoadMenuUpdate";
static constexpr container::StringView CB_SAVELOAD_SHUTDOWN= "SaveLoadMenuShutdown";
static constexpr container::StringView CB_CREDITS_INIT     = "CreditsInit";
static constexpr container::StringView CB_CREDITS_UPDATE   = "CreditsUpdate";
static constexpr container::StringView CB_CREDITS_SHUTDOWN = "CreditsShutdown";
static constexpr container::StringView CB_REPLAY_INIT      = "ReplayMenuInit";
static constexpr container::StringView CB_REPLAY_UPDATE    = "ReplayMenuUpdate";
static constexpr container::StringView CB_REPLAY_SHUTDOWN  = "ReplayMenuShutdown";
static constexpr container::StringView CB_LAN_INIT         = "LANLobbyMenuInit";
static constexpr container::StringView CB_LAN_UPDATE       = "LANLobbyMenuUpdate";
static constexpr container::StringView CB_LAN_SHUTDOWN     = "LANLobbyMenuShutdown";
static constexpr container::StringView CB_CHALLENGE_INIT   = "ChallengeMenuInit";
static constexpr container::StringView CB_CHALLENGE_UPDATE = "ChallengeMenuUpdate";
static constexpr container::StringView CB_CHALLENGE_SHUTDOWN = "ChallengeMenuShutdown";
static constexpr container::StringView CB_DIRECTCONNECT_INIT   = "NetworkDirectConnectInit";
static constexpr container::StringView CB_DIRECTCONNECT_UPDATE = "NetworkDirectConnectUpdate";
static constexpr container::StringView CB_DIRECTCONNECT_SHUTDOWN = "NetworkDirectConnectShutdown";
static constexpr container::StringView CB_MAPSELECT_INIT   = "SkirmishMapSelectMenuInit";
static constexpr container::StringView CB_MAPSELECT_UPDATE = "SkirmishMapSelectMenuUpdate";
static constexpr container::StringView CB_MAPSELECT_SHUTDOWN = "SkirmishMapSelectMenuShutdown";
static constexpr container::StringView CB_SINGLEPLAYER_LOAD_SHUTDOWN = "SinglePlayerLoadScreenShutdown";
static constexpr container::StringView CB_CHALLENGE_LOAD_SHUTDOWN = "ChallengeLoadScreenShutdown";
static constexpr container::StringView CB_POPUP_REPLAY_INIT = "PopupReplayInit";
static constexpr container::StringView CB_POPUP_REPLAY_UPDATE = "PopupReplayUpdate";
static constexpr container::StringView CB_POPUP_REPLAY_SHUTDOWN = "PopupReplayShutdown";
static constexpr container::StringView CB_SCORE_SCREEN_INIT = "ScoreScreenInit";
static constexpr container::StringView CB_SCORE_SCREEN_UPDATE = "ScoreScreenUpdate";
static constexpr container::StringView CB_SCORE_SCREEN_SHUTDOWN = "ScoreScreenShutdown";
static constexpr container::StringView CB_INGAME_POPUP_MESSAGE_INIT = "InGamePopupMessageInit";
static constexpr container::StringView CB_WOL_BUDDY_OVERLAY_INIT = "WOLBuddyOverlayInit";
static constexpr container::StringView CB_WOL_BUDDY_OVERLAY_UPDATE = "WOLBuddyOverlayUpdate";
static constexpr container::StringView CB_WOL_BUDDY_OVERLAY_SHUTDOWN = "WOLBuddyOverlayShutdown";
static constexpr container::StringView CB_POPUP_COMMUNICATOR_INIT = "PopupCommunicatorInit";
static constexpr container::StringView CB_POPUP_COMMUNICATOR_SHUTDOWN = "PopupCommunicatorShutdown";
static constexpr container::StringView CB_GAMEINFO_WINDOW_INIT = "GameInfoWindowInit";

// ── Draw Callback Names ─────────────────────────────────────────────────────

static constexpr container::StringView DRAW_CB_W3D_NO_DRAW       = "W3DNoDraw";
static constexpr container::StringView DRAW_CB_GAME_WIN_DEFAULT  = "GameWinDefaultDraw";
static constexpr container::StringView DRAW_CB_W3D_GAME_WIN_DEFAULT = "W3DGameWinDefaultDraw";
static constexpr container::StringView DRAW_CB_W3D_COMMAND_BAR_BACKGROUND = "W3DCommandBarBackgroundDraw";
static constexpr container::StringView DRAW_CB_W3D_COMMAND_BAR_FOREGROUND = "W3DCommandBarForegroundDraw";
static constexpr container::StringView DRAW_CB_W3D_COMMAND_BAR_GRID = "W3DCommandBarGridDraw";
static constexpr container::StringView DRAW_CB_W3D_COMMAND_BAR_TOP = "W3DCommandBarTopDraw";
static constexpr container::StringView DRAW_CB_W3D_LEFT_HUD = "W3DLeftHUDDraw";
static constexpr container::StringView DRAW_CB_W3D_RIGHT_HUD = "W3DRightHUDDraw";
static constexpr container::StringView DRAW_CB_W3D_GADGET_PUSH_BUTTON_IMAGE = "W3DGadgetPushButtonImageDraw";
static constexpr container::StringView DRAW_CB_W3D_POWER = "W3DPowerDraw";
static constexpr container::StringView DRAW_CB_W3D_COMMAND_BAR_GEN_EXP = "W3DCommandBarGenExpDraw";

// ── Window Type Names ───────────────────────────────────────────────────────

static constexpr container::StringView TYPE_ROOT     = "ROOT";
static constexpr container::StringView TYPE_PUSHBUTTON = "PUSHBUTTON";
static constexpr container::StringView TYPE_CHECKBOX = "CHECKBOX";
static constexpr container::StringView TYPE_RADIOBUTTON = "RADIOBUTTON";
static constexpr container::StringView TYPE_STATICTEXT = "STATICTEXT";
static constexpr container::StringView TYPE_USER     = "USER";
static constexpr container::StringView TYPE_TABCONTROL = "TABCONTROL";

// ── Back Button Names ───────────────────────────────────────────────────────

static constexpr container::StringView BTN_BACK        = "Back";
static constexpr container::StringView BTN_BUTTON_BACK = "ButtonBack";
static constexpr container::StringView BTN_CANCEL      = "ButtonCancel";
static constexpr container::StringView BTN_CLOSE       = "ButtonClose";

// ── Map Border Names ────────────────────────────────────────────────────────

static constexpr container::StringView MAP_BORDER       = "MapBorder";
static constexpr container::StringView MAP_BORDER1      = "MapBorder1";
static constexpr container::StringView MAP_BORDER2      = "MapBorder2";
static constexpr container::StringView MAP_BORDER3      = "MapBorder3";
static constexpr container::StringView MAP_BORDER4      = "MapBorder4";
static constexpr container::StringView MAIN_MENU_RULER  = "MainMenuRuler";

// ── MainMenu Button Names ───────────────────────────────────────────────────

static constexpr container::StringView BTN_SINGLEPLAYER   = "ButtonSinglePlayer";
static constexpr container::StringView BTN_MULTIPLAYER    = "ButtonMultiplayer";
static constexpr container::StringView BTN_LOAD_REPLAY    = "ButtonLoadReplay";
static constexpr container::StringView BTN_OPTIONS        = "ButtonOptions";
static constexpr container::StringView BTN_CREDITS        = "ButtonCredits";
static constexpr container::StringView BTN_EXIT           = "ButtonExit";
static constexpr container::StringView BTN_NETWORK        = "ButtonNetwork";
static constexpr container::StringView BTN_ONLINE         = "ButtonOnline";
static constexpr container::StringView BTN_LOAD_GAME      = "ButtonLoadGame";
static constexpr container::StringView BTN_REPLAY         = "ButtonReplay";
static constexpr container::StringView BTN_DIFF_BACK      = "ButtonDiffBack";
static constexpr container::StringView BTN_SINGLE_BACK    = "ButtonSingleBack";
static constexpr container::StringView BTN_MULTI_BACK     = "ButtonMultiBack";
static constexpr container::StringView BTN_LOAD_REPLAY_BACK = "ButtonLoadReplayBack";
static constexpr container::StringView BTN_SKIRMISH       = "ButtonSkirmish";
static constexpr container::StringView BTN_CHALLENGE      = "ButtonChallenge";
static constexpr container::StringView BTN_EASY           = "ButtonEasy";
static constexpr container::StringView BTN_MEDIUM         = "ButtonMedium";
static constexpr container::StringView BTN_HARD           = "ButtonHard";

// ── Faction Button Names ────────────────────────────────────────────────────

static constexpr container::StringView BTN_USA_RECENT_SAVE  = "ButtonUSARecentSave";
static constexpr container::StringView BTN_USA_LOAD_GAME    = "ButtonUSALoadGame";
static constexpr container::StringView BTN_GLA_RECENT_SAVE  = "ButtonGLARecentSave";
static constexpr container::StringView BTN_GLA_LOAD_GAME    = "ButtonGLALoadGame";
static constexpr container::StringView BTN_CHINA_RECENT_SAVE= "ButtonChinaRecentSave";
static constexpr container::StringView BTN_CHINA_LOAD_GAME  = "ButtonChinaLoadGame";

// ── Render Driver Names ─────────────────────────────────────────────────────

static constexpr container::StringView DRIVER_DIRECT3D12 = "direct3d12";
static constexpr container::StringView DRIVER_DIRECT3D11 = "direct3d11";
static constexpr container::StringView DRIVER_DIRECT3D   = "direct3d";
static constexpr container::StringView DRIVER_SOFTWARE   = "software";

// ── Window Title ────────────────────────────────────────────────────────────

static constexpr container::StringView WINDOW_TITLE = "GeneralsTD";

// ── Log Banner ──────────────────────────────────────────────────────────────

static constexpr container::StringView LOG_BANNER = "=== GeneralsTD: SDL3 + DX12 ===";
static constexpr container::StringView LOG_DEBUG_HINT = "Debug: F1 = skeleton/textured, F2 = world pass, F3 = texture-only material diagnostic";
static constexpr container::StringView LOG_FIRST_FRAME = "[Main] First frame rendered";
static constexpr container::StringView LOG_DONE = "=== Done ===";

// ── Skeleton Mode Labels ────────────────────────────────────────────────────

static constexpr container::StringView SKELETON_LABEL_PREFIX = "[Skeleton] ";

// ── Quit Message ────────────────────────────────────────────────────────────

static constexpr container::StringView MSG_QUIT_REQUESTED = "[Main] Quit requested, exiting loop";
