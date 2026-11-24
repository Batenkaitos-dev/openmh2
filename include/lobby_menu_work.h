#ifndef LOBBY_MENU_WORK_H
#define LOBBY_MENU_WORK_H

/* The town menus' shared work area gMenuWork as one type: the union MenuWork of the menus that live there.
 *
 * The menus are declared in several headers (lobby_menu.h, lobby_vobj.h, town_guide.h, lobby_search.h,
 * lobby_forge.h), most of which include lobby_menu.h, so the union is here, after them all. */

#include "common.h"
#include "lobby_menu.h"
#include "lobby_vobj.h"
#include "town_guide.h"
#include "lobby_search.h"
#include "lobby_forge.h"

/* The workshop's forge counters (WorkshopMenu's header, then the counter's own work at FORGE_WORK_OFFSET,
 * weapon_forge.c: the work of the weapon forge, which holds a pointer, WeaponForgeWork.upgradeEntry). The
 * armour forge and the decorations counter keep no pointer, and are not listed. */
typedef struct WeaponForgeMenu {
    /* 0x00 */ WorkshopMenu workshop;
    /* 0x08 */ u8 unk8[FORGE_WORK_OFFSET - sizeof(WorkshopMenu)];
    /* 0x0C */ WeaponForgeWork forge;
} WeaponForgeMenu;

/* The town menus' shared work area (3840 bytes at 0x8BDC20): a menu's Open clears it
 * (memset 3840) and starts the window slide that most menus keep first (WinSlide_Step).
 * Most units declare it as bytes; these are the menus that live there. The RAM image's types
 * (the port's RAM image) take a union word as a pointer when any member's is, so the members that hold
 * pointers are all here; the menus that hold none (ArenaMenu, BoardMenu, SongMenu, MailMenu, ...) need not be.
 * Not here: CombMenu (combine_shop.h), whose lines[].recipe pointers (+0xC0, 12 bytes apart) lie over
 * DrinkContest.counters (+0xC0, four u8: the drinking contest's counts, which reach 0x0A000000 and 0x09850000, the
 * addresses of the RAM block and of an object in a layout of the core); a union word is a pointer if any member's is, so
 * the counters would be translated (the route drink, VBlank 10020). Its words keep their lines in
 * the port's pointer list; those lines translate a value only when it is an address inside gRecipes or gRecipesEx. */
typedef union MenuWork {
    WinSlide win;
    PrizeMenu prize;
    ShopMenu shop;
    HairMenuWork hair;
    HouseAccessMenu houseAccess;
    AreaMenu area;
    ContestMenu contest;
    NameSearchMenu nameSearch;
    IdSearchMenu idSearch;
    AreaBoardMenu areaBoard;
    AreaList areaList;
    MoveMenu move;
    VillageBoardMenu villageBoard;
    DrinkContest drink;
    QuestCounterMenu questCounter;
    /* the menus that hold pointers (lobby_menu.h, then the other headers) */
    AreaSearchMenu areaSearch;
    BaitMenu bait;
    DojoMenu dojo;
    EquipShop equipShop;
    GuideMenu guide;
    KitchenMenu kitchen;
    MealScene meal;
    MineMenu mine;
    OfficialDeskMenu officialDesk;
    PoogieMenu poogie;
    PoogieNaming poogieNaming;
    QuestDesk questDesk;
    RecordMenu record;
    ShipMenu ship;
    WeaponForgeMenu weaponForge;
    u8 raw[0xF00];
} MenuWork;
typedef char MenuWork_size_check[(sizeof(MenuWork) == 0xF00) ? 1 : -1];

#endif /* LOBBY_MENU_WORK_H */
