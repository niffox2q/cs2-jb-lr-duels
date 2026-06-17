#include "jb_lr_duels.h"
#include <random>
#include <cstdio>
#include <algorithm>

jb_lr_duels g_jb_lr_duels;
PLUGIN_EXPOSE(jb_lr_duels, g_jb_lr_duels);

#define MAX_PLAYERS 64
#define CS_TEAM_CT 3
#define CS_TEAM_T  2


// SYSTEM API
IVEngineServer2* engine = nullptr;
CGlobalVars* gpGlobals = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem* g_pEntitySystem = nullptr;

// API
IUtilsApi* utils;
IPlayersApi* players_api;
IMenusApi* menus_api;
IJailbreakApi* jailbreak_api;
IAdminApi* admin_api;

SH_DECL_HOOK3_void(IServerGameDLL, GameFrame, SH_NOATTRIB, 0, bool, bool, bool);


// VARS

struct PositionInfo{
    std::string KeyName;

    Vector vPrisonerLocation;
    Vector vGuardLocation;

    QAngle qaPrisonerRotation;
    QAngle qaGuardRotation;
};

struct DuelPosition{
    std::string sMapName;

    std::vector<PositionInfo> vPositions;
};

std::vector<DuelPosition> g_DuelPositions;

bool b_debug = true;
std::string g_sAdminPermission;
float g_flDuelDuration;
bool g_bDuelStarted = false;

// One bullet vars
bool g_bOneBulletStarted = false;

// NoZoom vars
bool g_bNoScopeActive = false;

// OneHP vars
bool g_bOneHPStarted = true;

// Modificator vars
bool g_bEnableNoZoomModificator;
bool g_bEnableKnifeOnlyModificator;
bool g_bEnableOneBulletModificator;
bool g_bEnable1HPModificator;

std::map<std::string, std::string> phrases;

void LoadDuelPositions() {
    g_DuelPositions.clear();
    
    KeyValues* kv = new KeyValues("Maps");
    const char* path = "addons/configs/Jailbreak/lr_duels_maps.ini";

    if (!kv->LoadFromFile(g_pFullFileSystem, path)) {
        utils->ErrorLog("%s Failed to load positions: %s", g_PLAPI->GetLogTag(), path);
        delete kv;
        return;
    }

    for (KeyValues* pMapKey = kv->GetFirstTrueSubKey(); pMapKey; pMapKey = pMapKey->GetNextTrueSubKey()) {
        DuelPosition duelMap;
        duelMap.sMapName = pMapKey->GetName();

        for (KeyValues* pLocKey = pMapKey->GetFirstTrueSubKey(); pLocKey; pLocKey = pLocKey->GetNextTrueSubKey()) {
            PositionInfo posInfo;
            posInfo.KeyName = pLocKey->GetName();

            const char* pLoc = pLocKey->GetString("prisoner_location", "0 0 0");
            const char* pAng = pLocKey->GetString("prisoner_angle", "0 0 0");
            const char* gLoc = pLocKey->GetString("guard_location", "0 0 0");
            const char* gAng = pLocKey->GetString("guard_angle", "0 0 0");

            sscanf(pLoc, "%f %f %f", &posInfo.vPrisonerLocation.x, &posInfo.vPrisonerLocation.y, &posInfo.vPrisonerLocation.z);
            sscanf(pAng, "%f %f %f", &posInfo.qaPrisonerRotation.x, &posInfo.qaPrisonerRotation.y, &posInfo.qaPrisonerRotation.z);
            sscanf(gLoc, "%f %f %f", &posInfo.vGuardLocation.x, &posInfo.vGuardLocation.y, &posInfo.vGuardLocation.z);
            sscanf(gAng, "%f %f %f", &posInfo.qaGuardRotation.x, &posInfo.qaGuardRotation.y, &posInfo.qaGuardRotation.z);

            duelMap.vPositions.push_back(posInfo);
        }
        g_DuelPositions.push_back(duelMap);
    }
    
    delete kv;
}

void SaveNewDuelPosition(const char* szMapName, const char* szLocationName, Vector vPLoc, QAngle qaPAng, Vector vGLoc, QAngle qaGAng) {
    KeyValues* kv = new KeyValues("Maps");
    const char* path = "addons/configs/Jailbreak/lr_duels_maps.ini";

    kv->LoadFromFile(g_pFullFileSystem, path);

    KeyValues* pMapKey = kv->FindKey(szMapName, true);

    KeyValues* pLocKey = pMapKey->FindKey(szLocationName, true);

    char buf[128];

    snprintf(buf, sizeof(buf), "%f %f %f", vPLoc.x, vPLoc.y, vPLoc.z);
    pLocKey->SetString("prisoner_location", buf);

    snprintf(buf, sizeof(buf), "%f %f %f", qaPAng.x, qaPAng.y, qaPAng.z);
    pLocKey->SetString("prisoner_angle", buf);

    snprintf(buf, sizeof(buf), "%f %f %f", vGLoc.x, vGLoc.y, vGLoc.z);
    pLocKey->SetString("guard_location", buf);

    snprintf(buf, sizeof(buf), "%f %f %f", qaGAng.x, qaGAng.y, qaGAng.z);
    pLocKey->SetString("guard_angle", buf);

    kv->SaveToFile(g_pFullFileSystem, path);
    delete kv;

    LoadDuelPositions();
}

void RemoveDuelPosition(const char* szMapName, const char* szLocationName) {
    KeyValues* kv = new KeyValues("Maps");
    const char* path = "addons/configs/Jailbreak/lr_duels_maps.ini";
    
    if (kv->LoadFromFile(g_pFullFileSystem, path)) {
        KeyValues* pMapKey = kv->FindKey(szMapName);
        if (pMapKey) {
            KeyValues* pLocKey = pMapKey->FindKey(szLocationName);
            if (pLocKey) {
                pMapKey->RemoveSubKey(pLocKey);
                pLocKey->deleteThis();          
                kv->SaveToFile(g_pFullFileSystem, path); 
            }
        }
    }
    delete kv;

    for (auto& duelpos : g_DuelPositions) {
        if (duelpos.sMapName == szMapName) {
            auto& positions = duelpos.vPositions;
            positions.erase(std::remove_if(positions.begin(), positions.end(),
                [&szLocationName](const PositionInfo& pos) { 
                    return pos.KeyName == szLocationName; 
                }), 
                positions.end()
            );
            break; 
        }
    }
    LoadDuelPositions();
}

void LoadConfig() {
    KeyValues* config = new KeyValues("Config");
    const char* path = "addons/configs/Jailbreak/lr_duels.ini";
    if (!config->LoadFromFile(g_pFullFileSystem, path)) {
        utils->ErrorLog("%s Failed to load: %s", g_PLAPI->GetLogTag(), path);
        delete config;
        return;
    }

    g_sAdminPermission = config->GetString("admin_permission","@admin/root");

    g_bEnableNoZoomModificator      = config->GetBool("enable_nozoom",true);
    g_bEnableKnifeOnlyModificator   = config->GetBool("enable_knifeonly",true);
    g_bEnableOneBulletModificator   = config->GetBool("enable_onebullet",true);
    g_bEnable1HPModificator         = config->GetBool("enable_1hp",true);

    g_flDuelDuration                = config->GetFloat("duel_duration",30.0f);

    delete config;
}

void LoadTranslations() {
    phrases.clear();
    KeyValues* g_kvPhrases = new KeyValues("Phrases");
    const char *pszPath = "addons/translations/jailbreak_duels.phrases.txt";

    if (!g_kvPhrases->LoadFromFile(g_pFullFileSystem, pszPath))
    {
        utils->ErrorLog("%s Failed to load %s", g_PLAPI->GetLogTag(), pszPath);
        delete g_kvPhrases;
        return;
    }

    const char* language = utils->GetLanguage();

    for (KeyValues *pKey = g_kvPhrases->GetFirstTrueSubKey(); pKey; pKey = pKey->GetNextTrueSubKey()) {
        phrases[std::string(pKey->GetName())] = std::string(pKey->GetString(language));
    }
    delete g_kvPhrases;
}

const char* GetTranslation(const char* key) {
    auto it = phrases.find(key);
    if (it == phrases.end()) return key;
    else return it->second.c_str();
}

void PrintSlotPrefixed(int iSlot, const char* content) {
    if (!content || content[0] == '\0') return;
    char buf[512];
    g_SMAPI->Format(buf, sizeof(buf), "%s %s", GetTranslation("Prefix"), content);
    utils->PrintToChat(iSlot, "%s",buf);
}

void PrintAllPrefixed(const char* content) {
    if (!content || content[0] == '\0') return;
    char buf[512];
    g_SMAPI->Format(buf, sizeof(buf), "%s %s", GetTranslation("Prefix"), content);
    utils->PrintToChatAll("%s",buf);
}

ConVarRefAbstract* FindConVar(const char* sCvarName)
{
    return new ConVarRefAbstract(g_pCVar->FindConVar(sCvarName));
}


struct DuelContext {
    int iDuelSessionId = 0;

    int iInitiatorSlot = -1; 
    int iTargetSlot = -1; 
    std::string sTargetName = "";
    
    int iHP = 100;
    int iArmor = 100;
    bool bHelmet = true;
    
    std::string sWeapon = "weapon_ak47";
    std::string sWeaponDisplay = "AK-47";

    std::string sModificator = "default";
    std::string sModificatorDisplay = GetTranslation("Duels_DefaultDiplayName");

    void Reset() {
        iInitiatorSlot = -1;

        iTargetSlot = -1;
        sTargetName = "";

        iHP = 100;

        iArmor = 100;
        bHelmet = true;

        sWeapon = "weapon_ak47";
        sWeaponDisplay = "AK-47";

        sModificator = "default";
        sModificatorDisplay = GetTranslation("Duels_DefaultDiplayName");
    }

    bool IsActive() const {
        return iInitiatorSlot != -1 && iTargetSlot != -1;
    }
};

DuelContext g_Duel;

void OnGameFrame(bool sim, bool first, bool last){
    if (!g_bDuelStarted && !g_bNoScopeActive) return;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i != g_Duel.iInitiatorSlot && i != g_Duel.iTargetSlot) continue;
        auto pPlayer = CCSPlayerController::FromSlot(i);
        if (!pPlayer || !pPlayer->IsConnected()) continue;

        auto pPawn = pPlayer->GetPlayerPawn();
        if (!pPawn || !pPawn->IsAlive()) continue;

        auto pWeaponServices = pPawn->m_pWeaponServices.Get();
        if (!pWeaponServices) continue;

        auto ActiveWeapon = pWeaponServices->m_hActiveWeapon.Get();
        if (!ActiveWeapon) continue;

        if (strcmp(ActiveWeapon->GetClassname(), "weapon_awp")    == 0 ||
            strcmp(ActiveWeapon->GetClassname(), "weapon_scar20") == 0 ||
            strcmp(ActiveWeapon->GetClassname(), "weapon_g3sg1")  == 0 ||
            strcmp(ActiveWeapon->GetClassname(), "weapon_ssg08")  == 0 ) 
            {
                ActiveWeapon->m_flNextSecondaryAttackTickRatio = gpGlobals->curtime + 9999.0f;
        }
    }
}

void EnableInfinityAmmo() {
    auto Cvar = FindConVar("sv_infinite_ammo");
    if (Cvar) {
        Cvar->SetInt(2);
        delete Cvar;
    }
}
void DisableInfinityAmmo() {
    auto Cvar = FindConVar("sv_infinite_ammo");
    if (Cvar) {
        Cvar->SetInt(0);
        delete Cvar;
    }
}

void EndDuel(int iWinner){
    g_Duel.Reset();
    players_api->RemoveWeapons(iWinner);
    g_bDuelStarted = false;
    g_bNoScopeActive = false;
    g_bOneBulletStarted = false;
    g_bOneHPStarted = false;
    DisableInfinityAmmo();
    auto pController = CCSPlayerController::FromSlot(iWinner);
    if (!pController) return;
    auto pPawn = pController->GetPlayerPawn();
    if (!pPawn) return;
    char msg[256];
    g_SMAPI->Format(msg,sizeof(msg),GetTranslation("Duels_GGWinner"),pController->GetPlayerName());
    PrintAllPrefixed(msg);
    if (pPawn->IsAlive()){
        auto itemService = pPawn->m_pItemServices.Get();
        if (itemService) {
            itemService->GiveNamedItem("weapon_knife");
        }
    }
}

void GiveOneBullet(int iSlot) {
    auto pController = CCSPlayerController::FromSlot(iSlot);
    if (!pController) return;
    auto pPawn = pController->GetPlayerPawn();
    if (!pPawn || !pPawn->IsAlive()) return;

    auto WeaponService = pPawn->m_pWeaponServices.Get();
    if (!WeaponService) return;
    
    auto WeaponHandle = WeaponService->m_hActiveWeapon.Get();
    if (!WeaponHandle.IsValid()) return;
    auto Weapon = WeaponHandle.Get();
    

    Weapon->m_iClip1 = 1;

    utils->SetStateChanged(Weapon, "CBasePlayerWeapon", "m_iClip1");
}

void GiveSlotWeapon(int iSlot, const char* szWeapon) {
    auto pController = CCSPlayerController::FromSlot(iSlot);
    if (!pController) return;
    auto pPawn = pController->GetPlayerPawn();
    if (!pPawn || !pPawn->IsAlive()) return;

    players_api->RemoveWeapons(iSlot);

    auto itemService = pPawn->m_pItemServices.Get();
    if (itemService) {
        itemService->GiveNamedItem(szWeapon);
    }
}

void RemoveAndGiveWeaponNoAmmo(int iSlot,const char* szWeapon) {
    auto pController = CCSPlayerController::FromSlot(iSlot);
    if (!pController) return;
    auto pPawn = pController->GetPlayerPawn();
    if (!pPawn || !pPawn->IsAlive()) return;

    players_api->RemoveWeapons(iSlot);

    auto itemService = pPawn->m_pItemServices.Get();
    if (itemService) {
        itemService->GiveNamedItem(szWeapon);

        auto WeaponService = pPawn->m_pWeaponServices.Get();
        if (!WeaponService) return;
        auto WeaponHandle = WeaponService->m_hActiveWeapon.Get();
        if (!WeaponHandle.IsValid()) return;
        auto Weapon = WeaponHandle.Get();
        Weapon->m_iClip1 = 0;
        Weapon->m_iClip2 = 0;
        Weapon->m_pReserveAmmo = 0;
        utils->SetStateChanged(Weapon,"CBasePlayerWeapon","m_iClip1");
        utils->SetStateChanged(Weapon,"CBasePlayerWeapon","m_iClip2");
    }
}
void DuelHubMenu(int iSlot);

void StartDuel() {
    if (g_Duel.iInitiatorSlot == -1 || g_Duel.iTargetSlot == -1) return;

    int iSlot = g_Duel.iInitiatorSlot;
    auto tController = CCSPlayerController::FromSlot(iSlot);
    if (!tController || tController->GetTeam() != CS_TEAM_T) return;

    auto ctController = CCSPlayerController::FromSlot(g_Duel.iTargetSlot);
    if (!ctController || !ctController->IsConnected() || ctController->GetTeam() != CS_TEAM_CT) return;

    auto tPawn = tController->GetPlayerPawn();
    if (!tPawn || !tPawn->IsAlive()) return;

    auto ctPawn = ctController->GetPlayerPawn();
    if (!ctPawn || !ctPawn->IsAlive()) return;

    std::string currentMap = gpGlobals->mapname.ToCStr();
    PositionInfo selectedArena;
    bool bArenaFound = false;

    for (const auto& mapData : g_DuelPositions) {
        if (mapData.sMapName == currentMap) {
            if (!mapData.vPositions.empty()) {
                int iRandomIndex = rand() % mapData.vPositions.size();
                selectedArena = mapData.vPositions[iRandomIndex];
                bArenaFound = true;
            }
            break;
        }
    }
    
    if (bArenaFound) {
        players_api->Teleport(g_Duel.iInitiatorSlot, &selectedArena.vPrisonerLocation, &selectedArena.qaPrisonerRotation, nullptr);
        players_api->Teleport(g_Duel.iTargetSlot, &selectedArena.vGuardLocation, &selectedArena.qaGuardRotation, nullptr);
    }

    tPawn->m_iHealth     = g_Duel.iHP;
    tPawn->m_iMaxHealth  = g_Duel.iHP;
    tPawn->m_ArmorValue  = g_Duel.iArmor;

    ctPawn->m_iHealth    = g_Duel.iHP;
    ctPawn->m_iMaxHealth = g_Duel.iHP;
    ctPawn->m_ArmorValue = g_Duel.iArmor;

    players_api->RemoveWeapons(g_Duel.iInitiatorSlot);
    players_api->RemoveWeapons(g_Duel.iTargetSlot);
    
    PrintAllPrefixed(GetTranslation("Duels_PrisonerStartedDuel"));

    g_Duel.iDuelSessionId++;
    int iCapturedSession = g_Duel.iDuelSessionId;

    utils->CreateTimer(5.0f, [iCapturedSession]() {

        if (g_Duel.iDuelSessionId != iCapturedSession || !g_Duel.IsActive()) {
            return -1.0f; 
        }

        auto pT = CCSPlayerController::FromSlot(g_Duel.iInitiatorSlot);
        auto pCT = CCSPlayerController::FromSlot(g_Duel.iTargetSlot);
        if (!pT || !pCT || !pT->GetPlayerPawn() || !pCT->GetPlayerPawn() || 
            !pT->GetPlayerPawn()->IsAlive() || !pCT->GetPlayerPawn()->IsAlive()) {
            g_Duel.Reset();
            return -1.0f;
        }

        g_bDuelStarted = true;

        if (g_Duel.sModificator == "onebullet") {
            RemoveAndGiveWeaponNoAmmo(g_Duel.iInitiatorSlot, g_Duel.sWeapon.c_str());
            RemoveAndGiveWeaponNoAmmo(g_Duel.iTargetSlot, g_Duel.sWeapon.c_str());
            g_bOneBulletStarted = true;
            GiveOneBullet(g_Duel.iInitiatorSlot);
        }
        else {
            GiveSlotWeapon(g_Duel.iInitiatorSlot, g_Duel.sWeapon.c_str());
            GiveSlotWeapon(g_Duel.iTargetSlot, g_Duel.sWeapon.c_str());
        }

        if (g_Duel.sModificator == "onehp") {
            g_bOneHPStarted = true;
            EnableInfinityAmmo();
        }
        
        if (g_Duel.sModificator == "nozoom") {
            g_bNoScopeActive = true;
        }

        utils->CreateTimer(30.0f, [iCapturedSession]() {
            if (!g_bDuelStarted || g_Duel.iDuelSessionId != iCapturedSession) {
                return 1.0f; 
            }
            
            int iT = g_Duel.iInitiatorSlot;
            int iCT = g_Duel.iTargetSlot;

            g_bDuelStarted = false;

            players_api->CommitSuicide(iT, false, false);
            players_api->CommitSuicide(iCT, false, false);

            PrintAllPrefixed(GetTranslation("Duels_TimeEnd"));
            
            return -1.0f;
        });

        return -1.0f;
    });
}

void OpenArmorMenu(int iSlot){
    if (jailbreak_api->GetLRPrisoner() != iSlot) return;

    Menu hMenu;
    menus_api->SetTitleMenu(hMenu,GetTranslation("Duels_ArmorMenuTitle"));

    menus_api->AddItemMenu(hMenu,"100","100",ITEM_DEFAULT);
    menus_api->AddItemMenu(hMenu,"75","75",ITEM_DEFAULT);
    menus_api->AddItemMenu(hMenu,"50","50",ITEM_DEFAULT);
    menus_api->AddItemMenu(hMenu,"25","25",ITEM_DEFAULT);

    menus_api->SetBackMenu(hMenu,true);
    menus_api->SetExitMenu(hMenu,true);

    menus_api->SetCallback(hMenu,[](const char* szBack, const char* szFront, int iItem, int iSlot){
        if (!szBack || szBack[0] == '\0') return;
        if (strcmp(szBack,"back") == 0) {
            DuelHubMenu(iSlot);
            return;
        }
        if (strcmp(szBack,"exit") == 0) {
            menus_api->ClosePlayerMenu(iSlot);
            return;
        } 

        if (iItem < 7) {
            g_Duel.iArmor = atoi(szBack);
        }
    });

    menus_api->DisplayPlayerMenu(hMenu,iSlot,true,true);
}

void OpenHPMenu(int iSlot){
    if (jailbreak_api->GetLRPrisoner() != iSlot) return;

    Menu hMenu;
    menus_api->SetTitleMenu(hMenu,GetTranslation("Duels_HPMenuTitle"));

    menus_api->AddItemMenu(hMenu,"100","100",ITEM_DEFAULT);
    menus_api->AddItemMenu(hMenu,"75","75",ITEM_DEFAULT);
    menus_api->AddItemMenu(hMenu,"50","50",ITEM_DEFAULT);
    menus_api->AddItemMenu(hMenu,"25","25",ITEM_DEFAULT);

    menus_api->SetBackMenu(hMenu,true);
    menus_api->SetExitMenu(hMenu,true);

    menus_api->SetCallback(hMenu,[](const char* szBack, const char* szFront, int iItem, int iSlot){
        if (!szBack || szBack[0] == '\0') return;
        if (strcmp(szBack,"back") == 0) {
            DuelHubMenu(iSlot);
            return;
        }
        if (strcmp(szBack,"exit") == 0) {
            menus_api->ClosePlayerMenu(iSlot);
            return;
        } 

        if (iItem < 7) {
            g_Duel.iHP = atoi(szBack);
        }
    });

    menus_api->DisplayPlayerMenu(hMenu,iSlot,true,true);
}

void OpenTargetMenu(int iSlot) {
    if (jailbreak_api->GetLRPrisoner() != iSlot) return;

    Menu hMenu;
    menus_api->SetTitleMenu(hMenu,GetTranslation("Duels_TargetMenuTitle"));

    std::vector<int> vAliveCT;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        auto pController = CCSPlayerController::FromSlot(i);
        if (!pController || !pController->IsConnected() || pController->GetTeam() != CS_TEAM_CT) continue;
        auto pPawn = pController->GetPlayerPawn();
        if (!pPawn || !pPawn->IsAlive()) continue;
        vAliveCT.push_back(i);
    }

    if (vAliveCT.empty()) {
        menus_api->AddItemMenu(hMenu,"",GetTranslation("Duels_NoAvailableOpponents"),ITEM_DISABLED);
    } else {
        for (auto& iTarget : vAliveCT){
            auto pController = CCSPlayerController::FromSlot(iTarget);
            if (!pController || !pController->IsConnected() || pController->GetTeam() != CS_TEAM_CT) continue;
            auto pPawn = pController->GetPlayerPawn();
            if (!pPawn || !pPawn->IsAlive()) continue;
            menus_api->AddItemMenu(hMenu,std::to_string(iTarget).c_str(),pController->GetPlayerName(),ITEM_DEFAULT);
        }
    }

    menus_api->SetBackMenu(hMenu,true);
    menus_api->SetExitMenu(hMenu,true);

    menus_api->SetCallback(hMenu,[](const char* szBack, const char* szFront, int iItem, int iSlot){
        if (!szBack || szBack[0] == '\0') return;
        if (strcmp(szBack,"back") == 0) {
            DuelHubMenu(iSlot);
            return;
        }
        if (strcmp(szBack,"exit") == 0) {
            menus_api->ClosePlayerMenu(iSlot);
            return;
        } 

        if (iItem < 7) {
            g_Duel.iTargetSlot = atoi(szBack);
            auto pController = CCSPlayerController::FromSlot(g_Duel.iTargetSlot);
            if (pController) {
                g_Duel.sTargetName = pController->GetPlayerName();
            }
            DuelHubMenu(iSlot);
        }
    });

    menus_api->DisplayPlayerMenu(hMenu,iSlot,true,true);
}

void OpenModificatorMenu(int iSlot) {
    if (jailbreak_api->GetLRPrisoner() != iSlot) return;

    Menu hMenu;
    menus_api->SetTitleMenu(hMenu,GetTranslation("Duels_ModificatorMenuTitle"));
    menus_api->AddItemMenu(hMenu,"default",GetTranslation("Duels_ModificatorDefault"),ITEM_DEFAULT);
    if (g_bEnableNoZoomModificator) menus_api->AddItemMenu(hMenu,"nozoom",GetTranslation("Duels_ModificatorNoZoom"),ITEM_DEFAULT);
    if (g_bEnableOneBulletModificator) menus_api->AddItemMenu(hMenu,"onebullet",GetTranslation("Duels_ModificatorOneBullet"),ITEM_DEFAULT);
    if (g_bEnableKnifeOnlyModificator) menus_api->AddItemMenu(hMenu,"knifeonly",GetTranslation("Duels_ModificatorKnifeOnly"),ITEM_DEFAULT);
    if (g_bEnable1HPModificator) menus_api->AddItemMenu(hMenu,"onehp",GetTranslation("Duels_ModificatorOneHP"),ITEM_DEFAULT);

    menus_api->SetBackMenu(hMenu,true);
    menus_api->SetExitMenu(hMenu,true);

    menus_api->SetCallback(hMenu,[](const char* szBack, const char* szFront, int iItem, int iSlot){
        if (!szBack || szBack[0] == '\0') return;
        if (strcmp(szBack,"back") == 0) {
            DuelHubMenu(iSlot);
            return;
        }
        if (strcmp(szBack,"exit") == 0) {
            menus_api->ClosePlayerMenu(iSlot);
            return;
        } 

        if (strcmp(szBack,"default") == 0) {
            char msg[256];
            g_SMAPI->Format(msg,sizeof(msg),GetTranslation("Duels_DefaultModificationDescription"),szFront);
            PrintSlotPrefixed(iSlot,msg);
            g_Duel.sWeapon = "weapon_ak47";
            g_Duel.sWeaponDisplay = "AK-47";
            g_Duel.iHP = 100;   
            g_Duel.iArmor = 100; 
        }

        if (strcmp(szBack,"nozoom") == 0) {
            char msg[256];
            g_SMAPI->Format(msg,sizeof(msg),GetTranslation("Duels_NoZoomModificationDescription"),szFront);
            PrintSlotPrefixed(iSlot,msg);
            g_Duel.sWeapon = "weapon_awp";
            g_Duel.sWeaponDisplay = "AWP";
            g_Duel.iHP = 100;   
            g_Duel.iArmor = 100; 
        }

        if (strcmp(szBack,"onebullet") == 0) {
            char msg[256];
            g_SMAPI->Format(msg,sizeof(msg),GetTranslation("Duels_OneBulletModificationDescription"),szFront);
            PrintSlotPrefixed(iSlot,msg);
            g_Duel.sWeapon = "weapon_ak47";
            g_Duel.sWeaponDisplay = "AK-47";
            g_Duel.iHP = 100;   
            g_Duel.iArmor = 100; 
        }
        if (strcmp(szBack,"onehp") == 0) {
            char msg[256];
            g_SMAPI->Format(msg,sizeof(msg),GetTranslation("Duels_OneHPModificationDescription"),szFront);
            PrintSlotPrefixed(iSlot,msg);
            g_Duel.iHP = 1;
            g_Duel.iArmor = 0;
            g_Duel.sWeapon = "weapon_decoy";
            g_Duel.sWeaponDisplay = "DECOY";
        }
        if (strcmp(szBack,"knifeonly") == 0) {
            char msg[256];
            g_SMAPI->Format(msg,sizeof(msg),GetTranslation("Duels_KnifeOnlyModificationDescription"),szFront);
            PrintSlotPrefixed(iSlot,msg);
            g_Duel.sWeapon = "weapon_knife";
            g_Duel.sWeaponDisplay = "KNIFE";
            g_Duel.iHP = 100;   
            g_Duel.iArmor = 100; 
        }

        g_Duel.sModificator = szBack;
        g_Duel.sModificatorDisplay = szFront;
        DuelHubMenu(iSlot);
    });

    menus_api->DisplayPlayerMenu(hMenu,iSlot,true,true);
}

void OpenWeaponMenu(int iSlot) {
    if (jailbreak_api->GetLRPrisoner() != iSlot) return;

    std::string sModificator = g_Duel.sModificator;

    Menu hMenu;
    menus_api->SetTitleMenu(hMenu,GetTranslation("Duels_WeaponMenuTitle"));

    if (sModificator == "default" || sModificator == "onebullet"){
        menus_api->AddItemMenu(hMenu,"weapon_ak47","AK-47",ITEM_DEFAULT);
        menus_api->AddItemMenu(hMenu,"weapon_m4a1","M4A4",ITEM_DEFAULT);
        menus_api->AddItemMenu(hMenu,"weapon_m4a1","M4A1",ITEM_DEFAULT);
        menus_api->AddItemMenu(hMenu,"weapon_awp","AWP",ITEM_DEFAULT);
        menus_api->AddItemMenu(hMenu,"weapon_deagle","Deagle",ITEM_DEFAULT);
    }
    else if (sModificator == "nozoom") {
        menus_api->AddItemMenu(hMenu,"weapon_awp","AWP",ITEM_DEFAULT);
        menus_api->AddItemMenu(hMenu,"weapon_ssg08","Scout",ITEM_DEFAULT);
        menus_api->AddItemMenu(hMenu,"weapon_scar20","SCAR-20",ITEM_DEFAULT);
        menus_api->AddItemMenu(hMenu,"weapon_g3sg1","G3SG1",ITEM_DEFAULT);
    }

    menus_api->SetBackMenu(hMenu,true);
    menus_api->SetExitMenu(hMenu,true);

    menus_api->SetCallback(hMenu,[](const char* szBack, const char* szFront, int iItem, int iSlot){
        if (!szBack || szBack[0] == '\0') return;
        if (strcmp(szBack,"back") == 0) {
            DuelHubMenu(iSlot);
            return;
        }
        if (strcmp(szBack,"exit") == 0) {
            menus_api->ClosePlayerMenu(iSlot);
            return;
        } 

        g_Duel.sWeapon = szBack;
        g_Duel.sWeaponDisplay = szFront;
        DuelHubMenu(iSlot);
    });
    menus_api->DisplayPlayerMenu(hMenu,iSlot,true,true);
}

void DuelHubMenu(int iSlot){
    if (jailbreak_api->GetLRPrisoner() != iSlot) return;
    Menu hMenu;
    menus_api->SetTitleMenu(hMenu,GetTranslation("Duels_HubMenuTitle"));
    char item[256];
    if (g_Duel.iTargetSlot == -1) {
        g_SMAPI->Format(item,sizeof(item),GetTranslation("Duels_TargetInfoButton"),GetTranslation("Duels_NoOpponent"));
    } else {
        g_SMAPI->Format(item,sizeof(item),GetTranslation("Duels_TargetInfoButton"),g_Duel.sTargetName.c_str());
    }
    menus_api->AddItemMenu(hMenu,"opponent",item,ITEM_DEFAULT);

    g_SMAPI->Format(item,sizeof(item),GetTranslation("Duels_ModificatorInfoButton"),g_Duel.sModificatorDisplay.c_str());
    menus_api->AddItemMenu(hMenu,"modificator",item,ITEM_DEFAULT);

    if (g_Duel.sModificator == "default") {

        // Weapon
        g_SMAPI->Format(item,sizeof(item),GetTranslation("Duels_WeaponInfoButton"),g_Duel.sWeaponDisplay.c_str());
        menus_api->AddItemMenu(hMenu,"weapon",item,ITEM_DEFAULT);

        // HP
        g_SMAPI->Format(item,sizeof(item),GetTranslation("Duels_HPInfoButton"),g_Duel.iHP);
        menus_api->AddItemMenu(hMenu,"hp",item,ITEM_DEFAULT);

        // Armor
        g_SMAPI->Format(item,sizeof(item),GetTranslation("Duels_ArmorInfoButton"),g_Duel.iArmor);
        menus_api->AddItemMenu(hMenu,"armor",item,ITEM_DEFAULT);
    }

    else if (g_Duel.sModificator == "nozoom"){

        // Weapon
        g_SMAPI->Format(item,sizeof(item),GetTranslation("Duels_WeaponInfoButton"),g_Duel.sWeaponDisplay.c_str());
        menus_api->AddItemMenu(hMenu,"weapon",item,ITEM_DEFAULT);

        // HP
        g_SMAPI->Format(item,sizeof(item),GetTranslation("Duels_HPInfoButton"),g_Duel.iHP);
        menus_api->AddItemMenu(hMenu,"hp",item,ITEM_DEFAULT);

        // Armor
        g_SMAPI->Format(item,sizeof(item),GetTranslation("Duels_ArmorInfoButton"),g_Duel.iArmor);
        menus_api->AddItemMenu(hMenu,"armor",item,ITEM_DEFAULT);
    }

    else if (g_Duel.sModificator == "onebullet") {

        // Weapon
        g_SMAPI->Format(item,sizeof(item),GetTranslation("Duels_WeaponInfoButton"),g_Duel.sWeaponDisplay.c_str());
        menus_api->AddItemMenu(hMenu,"weapon",item,ITEM_DEFAULT);

        // HP
        g_SMAPI->Format(item,sizeof(item),GetTranslation("Duels_HPInfoButton"),g_Duel.iHP);
        menus_api->AddItemMenu(hMenu,"hp",item,ITEM_DEFAULT);

        // Armor
        g_SMAPI->Format(item,sizeof(item),GetTranslation("Duels_ArmorInfoButton"),g_Duel.iArmor);
        menus_api->AddItemMenu(hMenu,"armor",item,ITEM_DEFAULT);
    }

    else if (g_Duel.sModificator == "knifeonly" ) {

        // HP
        g_SMAPI->Format(item,sizeof(item),GetTranslation("Duels_HPInfoButton"),g_Duel.iHP);
        menus_api->AddItemMenu(hMenu,"hp",item,ITEM_DEFAULT);

        // Armor
        g_SMAPI->Format(item,sizeof(item),GetTranslation("Duels_ArmorInfoButton"),g_Duel.iArmor);
        menus_api->AddItemMenu(hMenu,"armor",item,ITEM_DEFAULT);
    }

    else if (g_Duel.sModificator == "onehp") {

        // SHOW NOTHING JUST START
    }

    else {
        // ERROR BUTTON DISABLED
    }
    if (g_Duel.iTargetSlot == -1) {
        menus_api->AddItemMenu(hMenu,"start",GetTranslation("Duel_StartButton"),ITEM_DISABLED);
    }
    else menus_api->AddItemMenu(hMenu,"start",GetTranslation("Duel_StartButton"),ITEM_DEFAULT);

    menus_api->SetExitMenu(hMenu,true);

    menus_api->SetCallback(hMenu,[](const char* szBack, const char* szFront, int iItem, int iSlot){
        if (jailbreak_api->GetLRPrisoner() != iSlot) {
            menus_api->ClosePlayerMenu(iSlot);
            return;
        }
        if (!szBack || szBack[0] == '\0') {
            menus_api->ClosePlayerMenu(iSlot);
            return;
        }
        if (strcmp(szBack,"exit") == 0) {
            menus_api->ClosePlayerMenu(iSlot);
            return;
        }

        if (strcmp(szBack,"weapon") == 0) {
            OpenWeaponMenu(iSlot);
            return;
        }

        if (strcmp(szBack,"modificator") == 0) {
            OpenModificatorMenu(iSlot);
            return;
        }

        if (strcmp(szBack,"opponent") == 0) {
            OpenTargetMenu(iSlot);
            return;
        }

        if (strcmp(szBack,"hp") == 0) {
            OpenHPMenu(iSlot);
            return;
        }

        if (strcmp(szBack,"armor") == 0) {
            OpenArmorMenu(iSlot);
            return;
        }

        if (strcmp(szBack,"start") == 0) {
            auto targetController = CCSPlayerController::FromSlot(g_Duel.iTargetSlot);
            if (!targetController || !targetController->IsConnected()) {
                PrintSlotPrefixed(iSlot,GetTranslation("Duels_TargetNotFound"));
                g_Duel.iTargetSlot = -1;
                DuelHubMenu(iSlot);
                return;
            }
            auto targetPawn = targetController->GetPlayerPawn();
            if (!targetPawn || !targetPawn->IsAlive()) {
                PrintSlotPrefixed(iSlot,GetTranslation("Duels_TargetIsDead"));
                g_Duel.iTargetSlot = -1;
                DuelHubMenu(iSlot);
                return;
            }
            auto pController = CCSPlayerController::FromSlot(iSlot);
            if (!pController || !pController->IsConnected()) {
                return;
            }
            auto pPawn = pController->GetPlayerPawn();
            if (!pPawn || !pPawn->IsAlive()) {
                if (pPawn) {
                    PrintSlotPrefixed(iSlot,GetTranslation("Duels_YouCantStartCuzDead"));
                    menus_api->ClosePlayerMenu(iSlot);
                }
                return;
            }
            StartDuel();
            menus_api->ClosePlayerMenu(iSlot);
            return;
        }
    });

    menus_api->DisplayPlayerMenu(hMenu,iSlot,true,true);

}

void OpenAdminMenu(int iSlot);
void DeletePosMenu(int iSlot);

void CheckingPosMenu(int iSlot, const char* szMapName, const char* szPosKey){
    if (iSlot < 0 || iSlot > MAX_PLAYERS) return;
    if (!admin_api->IsAdmin(iSlot) || !admin_api->HasPermission(iSlot, g_sAdminPermission.c_str())) {
        PrintSlotPrefixed(iSlot, GetTranslation("Duels_NoPermission"));
        return;
    }

    Menu hMenu;
    menus_api->SetTitleMenu(hMenu, szPosKey);
    menus_api->AddItemMenu(hMenu, "zek", GetTranslation("Duels_TPZekPos"), ITEM_DEFAULT);
    menus_api->AddItemMenu(hMenu, "guard", GetTranslation("Duels_TPGuardPos"), ITEM_DEFAULT);

    menus_api->SetBackMenu(hMenu, true);
    menus_api->SetExitMenu(hMenu, true);

    std::string sMap(szMapName);
    std::string sKey(szPosKey);

    menus_api->SetCallback(hMenu, [sMap, sKey](const char* szBack, const char* szFront, int iItem, int iSlot) {
        if (!szBack || szBack[0] == '\0') return;
        
        if (strcmp(szBack, "exit") == 0) {
            menus_api->ClosePlayerMenu(iSlot);
            return;
        }
        if (strcmp(szBack, "back") == 0) {
            OpenAdminMenu(iSlot); 
            return;
        }

        PositionInfo position;
        bool bFound = false;

        for (const auto& duelpos : g_DuelPositions) {
            if (duelpos.sMapName == sMap) {
                for (const auto& pos : duelpos.vPositions) {
                    if (pos.KeyName == sKey) {
                        position = pos;
                        bFound = true;
                        break;
                    }
                }
                break;
            }
        }

        if (!bFound) return;

        auto pController = CCSPlayerController::FromSlot(iSlot);
        if (!pController) return;
        auto pPawn = pController->GetPlayerPawn();
        if (!pPawn || !pPawn->IsAlive()) return;

        if (strcmp(szBack, "zek") == 0) {
            players_api->Teleport(iSlot, &position.vPrisonerLocation, &position.qaPrisonerRotation, nullptr);
        }
        else if (strcmp(szBack, "guard") == 0) {
            players_api->Teleport(iSlot, &position.vGuardLocation, &position.qaGuardRotation, nullptr);
        }

        CheckingPosMenu(iSlot, sMap.c_str(), sKey.c_str()); 
    });

    menus_api->DisplayPlayerMenu(hMenu, iSlot, true, true);
}

void CheckPosMenu(int iSlot){
    if (iSlot < 0 || iSlot > MAX_PLAYERS) return;
    if (!admin_api->IsAdmin(iSlot) || !admin_api->HasPermission(iSlot, g_sAdminPermission.c_str())) {
        PrintSlotPrefixed(iSlot, GetTranslation("Duels_NoPermission"));
        return;
    }

    Menu hMenu;
    menus_api->SetTitleMenu(hMenu, GetTranslation("Duels_CheckPosTitle"));
    
    std::string curMapName = gpGlobals->mapname.ToCStr();
    bool bHasPositions = false;

    for (const auto& duelpos : g_DuelPositions) {
        if (duelpos.sMapName == curMapName) {
            for (const auto& pos : duelpos.vPositions) {
                menus_api->AddItemMenu(hMenu, pos.KeyName.c_str(), pos.KeyName.c_str(), ITEM_DEFAULT);
                bHasPositions = true;
            }
            break;
        }
    }

    if (!bHasPositions) {
        menus_api->AddItemMenu(hMenu, "", GetTranslation("Duels_NoAvailableMapPositions"), ITEM_DISABLED);
    }

    menus_api->SetBackMenu(hMenu, true);
    menus_api->SetExitMenu(hMenu, true);

    menus_api->SetCallback(hMenu, [curMapName](const char* szBack, const char* szFront, int iItem, int iSlot) {
        if (!szBack || szBack[0] == '\0') return;
        
        if (strcmp(szBack, "exit") == 0) {
            menus_api->ClosePlayerMenu(iSlot);
            return;
        }
        if (strcmp(szBack, "back") == 0) {
            OpenAdminMenu(iSlot);
            return;
        }

        CheckingPosMenu(iSlot,curMapName.c_str(),szBack);
    });
    menus_api->DisplayPlayerMenu(hMenu,iSlot,true,true);
}

void DeletePosMenu(int iSlot) {
    if (iSlot < 0 || iSlot > MAX_PLAYERS) return;
    if (!admin_api->IsAdmin(iSlot) || !admin_api->HasPermission(iSlot, g_sAdminPermission.c_str())) {
        PrintSlotPrefixed(iSlot, GetTranslation("Duels_NoPermission"));
        return;
    }

    Menu hMenu;
    menus_api->SetTitleMenu(hMenu, GetTranslation("Duels_DeletePosTitle"));
    
    std::string curMapName = gpGlobals->mapname.ToCStr();
    bool bHasPositions = false;

    for (const auto& duelpos : g_DuelPositions) {
        if (duelpos.sMapName == curMapName) {
            for (const auto& pos : duelpos.vPositions) {
                menus_api->AddItemMenu(hMenu, pos.KeyName.c_str(), pos.KeyName.c_str(), ITEM_DEFAULT);
                bHasPositions = true;
            }
            break;
        }
    }

    if (!bHasPositions) {
        menus_api->AddItemMenu(hMenu, "", GetTranslation("Duels_NoAvailableMapPositions"), ITEM_DISABLED);
    }

    menus_api->SetBackMenu(hMenu, true);
    menus_api->SetExitMenu(hMenu, true);

    menus_api->SetCallback(hMenu, [](const char* szBack, const char* szFront, int iItem, int iSlot) {
        if (!szBack || szBack[0] == '\0') return;
        
        if (strcmp(szBack, "exit") == 0) {
            menus_api->ClosePlayerMenu(iSlot);
            return;
        }
        if (strcmp(szBack, "back") == 0) {
            OpenAdminMenu(iSlot);
            return;
        }

        RemoveDuelPosition(gpGlobals->mapname.ToCStr(), szBack);
        PrintSlotPrefixed(iSlot, GetTranslation("Duels_PositionDeleted")); 
        DeletePosMenu(iSlot); 
    });

    menus_api->DisplayPlayerMenu(hMenu, iSlot, true, true);
}

struct AdminSetupState {
    PositionInfo pos;
    bool bTPos = false;
    bool bCTPos = false;
    std::string textkeyname;
};
std::map<int, AdminSetupState> g_AdminSetup;
bool bWaitingCustomTextSlot[64] = {false};


void CreateNewPosMenu(int iSlot){
    if (iSlot < 0 || iSlot > MAX_PLAYERS) return;
    if (!admin_api->IsAdmin(iSlot) || !admin_api->HasPermission(iSlot, g_sAdminPermission.c_str())) {
        PrintSlotPrefixed(iSlot, GetTranslation("Duels_NoPermission"));
        return;
    }

    Menu hMenu;

    menus_api->SetTitleMenu(hMenu,GetTranslation("Duels_CreatePosTitle"));
    
    char t_item[128], ct_item[128];
    g_SMAPI->Format(t_item, sizeof(t_item), "%s [%s]", GetTranslation("Duels_SaveTPos"), g_AdminSetup[iSlot].bTPos ? "+" : "-");
    g_SMAPI->Format(ct_item, sizeof(ct_item), "%s [%s]", GetTranslation("Duels_SaveCTPos"), g_AdminSetup[iSlot].bCTPos ? "+" : "-");

    menus_api->AddItemMenu(hMenu, "zek", t_item, ITEM_DEFAULT);
    menus_api->AddItemMenu(hMenu, "guard", ct_item, ITEM_DEFAULT);
    menus_api->AddItemMenu(hMenu, "save", GetTranslation("Duels_SavePos"), ITEM_DEFAULT);
    menus_api->AddItemMenu(hMenu, "return", GetTranslation("Duels_ReturnButton"), ITEM_DEFAULT);

    menus_api->SetExitMenu(hMenu, true);

    menus_api->SetCallback(hMenu, [](const char* szBack, const char* szFront, int iItem, int iSlot) {
        if (!szBack || szBack[0] == '\0') return;
        
        if (strcmp(szBack, "exit") == 0) {
            g_AdminSetup.erase(iSlot); 
            menus_api->ClosePlayerMenu(iSlot);
            return;
        }
        if (strcmp(szBack, "return") == 0) {
            g_AdminSetup.erase(iSlot);
            OpenAdminMenu(iSlot);
            return;
        }
        if (strcmp(szBack, "zek") == 0) {
            auto pController = CCSPlayerController::FromSlot(iSlot);
            if (pController && pController->GetPlayerPawn()) {
                g_AdminSetup[iSlot].pos.vPrisonerLocation = pController->GetPlayerPawn()->GetAbsOrigin();
                g_AdminSetup[iSlot].pos.qaPrisonerRotation = pController->GetPlayerPawn()->GetAngRotation();
                g_AdminSetup[iSlot].bTPos = true;
                PrintSlotPrefixed(iSlot, GetTranslation("Duels_TPosSaved"));
            }
            CreateNewPosMenu(iSlot); 
            return;
        }
        if (strcmp(szBack, "guard") == 0) { 
            auto pController = CCSPlayerController::FromSlot(iSlot);
            if (pController && pController->GetPlayerPawn()) {
                g_AdminSetup[iSlot].pos.vGuardLocation = pController->GetPlayerPawn()->GetAbsOrigin();
                g_AdminSetup[iSlot].pos.qaGuardRotation = pController->GetPlayerPawn()->GetAngRotation();
                g_AdminSetup[iSlot].bCTPos = true;
                PrintSlotPrefixed(iSlot, GetTranslation("Duels_CTPosSaved"));
            }
            CreateNewPosMenu(iSlot); 
            return;
        }
        if (strcmp(szBack, "save") == 0) {
            if (!g_AdminSetup[iSlot].bTPos) {
                PrintSlotPrefixed(iSlot, GetTranslation("Duels_NoTPosInstalled"));
                CreateNewPosMenu(iSlot);
                return;
            }
            if (!g_AdminSetup[iSlot].bCTPos) { 
                PrintSlotPrefixed(iSlot, GetTranslation("Duels_NoCTPosInstalled"));
                CreateNewPosMenu(iSlot);
                return;
            }

            bWaitingCustomTextSlot[iSlot] = true;
            
            PrintSlotPrefixed(iSlot,GetTranslation("Duels_EnterKeyName"));
            menus_api->ClosePlayerMenu(iSlot);
        }
    });

    menus_api->DisplayPlayerMenu(hMenu, iSlot, true, true);
}

void OpenAdminMenu(int iSlot) {
    if  (iSlot < 0 || iSlot > MAX_PLAYERS) return;
    if (!admin_api->IsAdmin(iSlot) || !admin_api->HasPermission(iSlot,g_sAdminPermission.c_str())) {
        PrintSlotPrefixed(iSlot,GetTranslation("Duels_NoPermission"));
        return;
    }
    g_AdminSetup.erase(iSlot);
    bWaitingCustomTextSlot[iSlot] = false;
    Menu hMenu;
    menus_api->SetTitleMenu(hMenu,GetTranslation("Duels_AdminMenuTitle"));
    menus_api->AddItemMenu(hMenu,"new",GetTranslation("Duels_CreateNewPos"),ITEM_DEFAULT);
    menus_api->AddItemMenu(hMenu,"delete",GetTranslation("Duels_DeletePos"),ITEM_DEFAULT);
    menus_api->AddItemMenu(hMenu,"check",GetTranslation("Duels_CheckPos"),ITEM_DEFAULT);
    
    menus_api->SetExitMenu(hMenu,true);

    menus_api->SetCallback(hMenu,[](const char* szBack, const char* szFront, int iItem, int iSlot){
        if (!szBack || szBack[0] == '\0') return;
        if (strcmp(szBack,"exit") == 0) {
            menus_api->ClosePlayerMenu(iSlot);
            return;
        }
        if (strcmp(szBack,"new") == 0) {
            CreateNewPosMenu(iSlot);
            return;
        }
        if (strcmp(szBack,"delete") == 0) {
            DeletePosMenu(iSlot);
            return;
        }

        if (strcmp(szBack,"check") == 0) {
            CheckPosMenu(iSlot);
            return;
        }

    });
    menus_api->DisplayPlayerMenu(hMenu,iSlot,true,true);
}

bool OnAdminCommand(int iSlot, const char* content) {
    if  (iSlot < 0 || iSlot > MAX_PLAYERS) return true;
    if (!admin_api->IsAdmin(iSlot) || !admin_api->HasPermission(iSlot,g_sAdminPermission.c_str())) {
        PrintSlotPrefixed(iSlot,GetTranslation("Duels_NoPermission"));
        return true;
    }
    OpenAdminMenu(iSlot);
    return false;
}




CGameEntitySystem* GameEntitySystem() {
    return utils ? utils->GetCGameEntitySystem() : nullptr;
}

void StartupServer() {
    g_pGameEntitySystem = GameEntitySystem();
    g_pEntitySystem = utils->GetCEntitySystem();
    gpGlobals = utils->GetCGlobalVars();

}

bool jb_lr_duels::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late) {
    PLUGIN_SAVEVARS();

    GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetServerFactory, g_pSource2Server, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetServerFactory, g_pSource2GameClients, IServerGameClients, SOURCE2GAMECLIENTS_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetServerFactory, g_pSource2GameEntities, ISource2GameEntities, SOURCE2GAMEENTITIES_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetEngineFactory, g_pNetworkSystem, INetworkSystem, NETWORKSYSTEM_INTERFACE_VERSION);

    ConVar_Register(FCVAR_SERVER_CAN_EXECUTE | FCVAR_GAMEDLL);
    g_SMAPI->AddListener(this, this);

    SH_ADD_HOOK(IServerGameDLL, GameFrame, g_pSource2Server, OnGameFrame, true);

    return true;
}

void jb_lr_duels::AllPluginsLoaded() {
    int ret;
    utils = (IUtilsApi*)g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED) {
        META_CONPRINTF("%s | Missing UTILS plugin.\n", g_PLAPI->GetLogTag());
        engine->ServerCommand(("meta unload " + std::to_string(g_PLID)).c_str());
        return;
    }

    menus_api = (IMenusApi*)g_SMAPI->MetaFactory(Menus_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED) {
        META_CONPRINTF("%s | Missing UTILS plugin.\n", g_PLAPI->GetLogTag());
        engine->ServerCommand(("meta unload " + std::to_string(g_PLID)).c_str());
        return;
    }

    players_api = (IPlayersApi*)g_SMAPI->MetaFactory(PLAYERS_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED) {
        META_CONPRINTF("%s | Missing UTILS plugin.\n", g_PLAPI->GetLogTag());
        engine->ServerCommand(("meta unload " + std::to_string(g_PLID)).c_str());
        return;
    }

    jailbreak_api = (IJailbreakApi*)g_SMAPI->MetaFactory(JAILBREAK_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED) {
        META_CONPRINTF("%s | Missing Jailbreak Core plugin.\n", g_PLAPI->GetLogTag());
        engine->ServerCommand(("meta unload " + std::to_string(g_PLID)).c_str());
        return;
    }
    admin_api = (IAdminApi*)g_SMAPI->MetaFactory(Admin_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED) {
        META_CONPRINTF("%s | Missing Admin System plugin.",g_PLAPI->GetLogTag());
        engine->ServerCommand(("meta unload " + std::to_string(g_PLID)).c_str());
        return;
    }

    LoadDuelPositions();
    LoadConfig();
    LoadTranslations();

    jailbreak_api->RegisterLRFeature(g_PLID,"duels",GetTranslation("Duels_DuelLRButton"),[](int iSlot){
        g_Duel.Reset();
        g_Duel.iInitiatorSlot = iSlot;
        DuelHubMenu(iSlot);
    });     

    utils->HookEvent(g_PLID,"weapon_fire",[](const char* szName, IGameEvent* pEvent, bool bDontBroadcast){
        if (g_bDuelStarted && g_bOneBulletStarted) {
            int iSlot = pEvent->GetInt("userid");
            if (iSlot == g_Duel.iTargetSlot) {
                GiveOneBullet(g_Duel.iInitiatorSlot);
            } 
            else if (iSlot == g_Duel.iInitiatorSlot) {
                GiveOneBullet(g_Duel.iTargetSlot);
            }
        }
    });

    utils->HookEvent(g_PLID,"player_death",[](const char* szName, IGameEvent* pEvent, bool bDontBroadcast){
        int iVictim = pEvent->GetInt("userid");
        int iAttacker = pEvent->GetInt("attacker");

        if (!g_bDuelStarted) {
            if (iVictim == g_Duel.iInitiatorSlot || iVictim == g_Duel.iTargetSlot) {
                menus_api->ClosePlayerMenu(g_Duel.iInitiatorSlot);
                g_Duel.Reset(); 
            }
            return;
        }

        if (g_Duel.iInitiatorSlot == iAttacker && g_Duel.iTargetSlot == iVictim) {
            EndDuel(g_Duel.iInitiatorSlot);
        } 
        else if (g_Duel.iInitiatorSlot == iVictim && g_Duel.iTargetSlot == iAttacker) {
            EndDuel(g_Duel.iTargetSlot);
        }
        else if (iVictim == g_Duel.iInitiatorSlot) {
            EndDuel(g_Duel.iTargetSlot);
        }
        else if (iVictim == g_Duel.iTargetSlot) {
            EndDuel(g_Duel.iInitiatorSlot);
        }
    });

    utils->HookEvent(g_PLID,"player_disconnect",[](const char* szName, IGameEvent* pEvent, bool bDontBroadcast){
        int iVictim = pEvent->GetInt("userid");

        if (!g_bDuelStarted) {
            if (iVictim == g_Duel.iInitiatorSlot || iVictim == g_Duel.iTargetSlot) {
                menus_api->ClosePlayerMenu(g_Duel.iInitiatorSlot);
                g_Duel.Reset(); 
            }
            return;
        }

        if (iVictim == g_Duel.iInitiatorSlot) {
            EndDuel(g_Duel.iTargetSlot);
            return;
        }
        else if (iVictim == g_Duel.iTargetSlot) {
            EndDuel(g_Duel.iInitiatorSlot);
            return;
        }
    });

    utils->HookEvent(g_PLID,"decoy_started",[](const char* szName, IGameEvent* pEvent, bool bDontBroadcast){
        if (g_bDuelStarted && g_bOneHPStarted) {
            CEntityIndex index = (CEntityIndex)pEvent->GetInt("entityid");
            auto GrenadeInstance = g_pEntitySystem->GetEntityInstance(index);
            if (!GrenadeInstance) return;
            utils->AcceptEntityInput(GrenadeInstance,"Kill");
        }
    });

    utils->AddChatListenerPre(g_PLID, [](int iSlot, const char* szContent, bool bTeam){
        if (!bWaitingCustomTextSlot[iSlot]) return true;
        std::string sInput = szContent;
        sInput.erase(std::remove(sInput.begin(), sInput.end(), '\"'), sInput.end());
        if (sInput.empty()) {
            PrintSlotPrefixed(iSlot, GetTranslation("Duels_EmptyInputRetry"));
            return false;
        }

        g_AdminSetup[iSlot].textkeyname = sInput;
        PrintSlotPrefixed(iSlot,GetTranslation("Duels_PositionSaved"));
        SaveNewDuelPosition(
            gpGlobals->mapname.ToCStr(),
            g_AdminSetup[iSlot].textkeyname.c_str(),
            g_AdminSetup[iSlot].pos.vPrisonerLocation,
            g_AdminSetup[iSlot].pos.qaPrisonerRotation,
            g_AdminSetup[iSlot].pos.vGuardLocation,
            g_AdminSetup[iSlot].pos.qaGuardRotation
        );
        g_AdminSetup.erase(iSlot);
        bWaitingCustomTextSlot[iSlot] = false;
        OpenAdminMenu(iSlot);
        
        return false;
    });

    utils->RegCommand(g_PLID,{"mm_jb_duels"},{"!jb_duels"},OnAdminCommand);

    utils->StartupServer(g_PLID, StartupServer);
}

bool jb_lr_duels::Unload(char* error, size_t maxlen) {
    jailbreak_api->ClearAllPluginHooks(g_PLID);
    utils->ClearAllHooks(g_PLID);
    ConVar_Unregister();

    SH_REMOVE_HOOK(IServerGameDLL, GameFrame, g_pSource2Server, OnGameFrame, true);

    return true;
}

const char* jb_lr_duels::GetAuthor() { return "niffox"; }
const char* jb_lr_duels::GetDate() { return __DATE__; }
const char* jb_lr_duels::GetDescription() { return "[JB] LR Duels"; }
const char* jb_lr_duels::GetLicense() { return "GPL"; }
const char* jb_lr_duels::GetLogTag() { return "[JB] LR Duels"; }
const char* jb_lr_duels::GetName() { return "[JB] LR Duels"; }
const char* jb_lr_duels::GetURL() { return "https://t.me/niffox_2q"; }
const char* jb_lr_duels::GetVersion() { return "1.1.0"; }