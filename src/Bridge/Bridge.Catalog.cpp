/**
*  jax-gemma bridge for yrpp-spawner  (GPLv3)
*
*  One-shot type catalog dump. At the first in-match frame, write every buildable
*  type's (category, *Type-RTTI, ArrayIndex, internal ID, UI name label) to a CSV in
*  the game working directory. Python reads it to (a) GROUND the LLM commander with the
*  real unit/building roster (no hallucinated units) and (b) map a directive name ->
*  (rtti, type_id) for PRODUCE/PLACE actions.
*/
#include <cstdio>
#include <cstring>
#include <climits>
#include <windows.h>

#include "Bridge.h"

#include <UnitTypeClass.h>
#include <BuildingTypeClass.h>
#include <InfantryTypeClass.h>
#include <AircraftTypeClass.h>
#include <GeneralDefinitions.h>    // AbstractType
#include <Utilities/Debug.h>
#include <HouseClass.h>
#include <HouseTypeClass.h>
#include <RulesClass.h>
#include <ScenarioClass.h>
#include <AITriggerTypeClass.h>
#include <TeamTypeClass.h>
#include <TaskForceClass.h>
#include <ScriptTypeClass.h>
#include <FactoryClass.h>
#include <BuildingClass.h>
#include <MapClass.h>
#include <CellClass.h>
#include <TechnoTypeClass.h>
#include <WeaponTypeClass.h>
#include <WarheadTypeClass.h>
#include <BulletTypeClass.h>
#include <Fundamentals.h>
#include <GameModeOptionsClass.h>
#include <SessionClass.h>

namespace
{
    bool g_dumped = false;
}

void Bridge::DumpCatalogOnce()
{
    if (g_dumped)
        return;
    g_dumped = true;

    FILE* f = fopen("type_catalog.csv", "w");  // -> game working directory
    if (!f)
        return;
    fprintf(f, "category,type_rtti,index,id,ui_name\n");

    {
        DynamicVectorClass<BuildingTypeClass*>& a = BuildingTypeClass::Array;
        for (int i = 0; i < a.Count; ++i)
            if (a.Items[i])
                fprintf(f, "building,%d,%d,%s,%s\n", static_cast<int>(AbstractType::BuildingType),
                        a.Items[i]->ArrayIndex, a.Items[i]->ID, a.Items[i]->UINameLabel);
    }
    {
        DynamicVectorClass<UnitTypeClass*>& a = UnitTypeClass::Array;
        for (int i = 0; i < a.Count; ++i)
            if (a.Items[i])
                fprintf(f, "unit,%d,%d,%s,%s\n", static_cast<int>(AbstractType::UnitType),
                        a.Items[i]->ArrayIndex, a.Items[i]->ID, a.Items[i]->UINameLabel);
    }
    {
        DynamicVectorClass<InfantryTypeClass*>& a = InfantryTypeClass::Array;
        for (int i = 0; i < a.Count; ++i)
            if (a.Items[i])
                fprintf(f, "infantry,%d,%d,%s,%s\n", static_cast<int>(AbstractType::InfantryType),
                        a.Items[i]->ArrayIndex, a.Items[i]->ID, a.Items[i]->UINameLabel);
    }
    {
        DynamicVectorClass<AircraftTypeClass*>& a = AircraftTypeClass::Array;
        for (int i = 0; i < a.Count; ++i)
            if (a.Items[i])
                fprintf(f, "aircraft,%d,%d,%s,%s\n", static_cast<int>(AbstractType::AircraftType),
                        a.Items[i]->ArrayIndex, a.Items[i]->ID, a.Items[i]->UINameLabel);
    }

    fclose(f);
}

// ============================================================================================
//  M4 rule dump: rules_types.json / rules_ai.json / ai_scripts.json   (READ ONLY, once per match)
//  Source of truth = the engine's in-memory data after rules + AI overrides + MAP have loaded.
//  Field layouts: YRpp/TechnoTypeClass.h, BuildingTypeClass.h, RulesClass.h, AITriggerTypeClass.h,
//  TeamTypeClass.h, TaskForceClass.h, ScriptTypeClass.h.
// ============================================================================================
namespace
{
    bool g_rulesDumped = false;

    bool Readable(const void* p, size_t n)
    {
        return p && !IsBadReadPtr(p, n);
    }

    struct J
    {
        FILE* f;
        bool first[16];
        int d;
        explicit J(FILE* fp) : f(fp), d(0) { for (int i = 0; i < 16; ++i) first[i] = true; }

        static void EscStr(FILE* f, const char* s, size_t maxLen)
        {
            fputc('"', f);
            for (size_t i = 0; s && i < maxLen && s[i]; ++i)
            {
                unsigned char c = static_cast<unsigned char>(s[i]);
                if (c == '"') fputs("\\\"", f);
                else if (c == '\\') fputs("\\\\", f);
                else if (c < 0x20 || c >= 0x7F) fprintf(f, "\\u%04x", c);
                else fputc(c, f);
            }
            fputc('"', f);
        }
        static void EscWide(FILE* f, const wchar_t* s, size_t maxLen)
        {
            fputc('"', f);
            for (size_t i = 0; s && i < maxLen && s[i]; ++i)
            {
                unsigned c = static_cast<unsigned>(s[i]);
                if (c == '"') fputs("\\\"", f);
                else if (c == '\\') fputs("\\\\", f);
                else if (c < 0x20 || c >= 0x7F) fprintf(f, "\\u%04x", c & 0xFFFF);
                else fputc(static_cast<int>(c), f);
            }
            fputc('"', f);
        }
        void Sep(const char* key)
        {
            if (!first[d]) fputc(',', f);
            first[d] = false;
            fputc('\n', f);
            for (int i = 0; i < d; ++i) fputc(' ', f);
            if (key) { EscStr(f, key, 64); fputs(": ", f); }
        }
        void Open(const char* key, char c) { Sep(key); fputc(c, f); ++d; first[d] = true; }
        void Close(char c)
        {
            bool empty = first[d];
            --d;
            if (!empty) { fputc('\n', f); for (int i = 0; i < d; ++i) fputc(' ', f); }
            fputc(c, f);
        }
        void Int(const char* key, long long v) { Sep(key); fprintf(f, "%lld", v); }
        void Dbl(const char* key, double v)
        {
            Sep(key);
            if (v != v || v > 1e300 || v < -1e300) fputs("null", f);   // NaN / inf are not JSON
            else fprintf(f, "%.9g", v);
        }
        void Bool(const char* key, bool v) { Sep(key); fputs(v ? "true" : "false", f); }
        void Str(const char* key, const char* s, size_t maxLen = 64) { Sep(key); EscStr(f, s, maxLen); }
        void Wide(const char* key, const wchar_t* s) { Sep(key); if (Readable(s, 2)) EscWide(f, s, 96); else fputs("null", f); }
        void Null(const char* key) { Sep(key); fputs("null", f); }
        void IntList(const char* key, const DynamicVectorClass<int>& v)
        {
            Open(key, '[');
            if (v.Count > 0 && v.Count < 4096 && Readable(v.Items, sizeof(int) * v.Count))
                for (int i = 0; i < v.Count; ++i) Int(nullptr, v.Items[i]);
            Close(']');
        }
    };

    // {id,rtti,index} for a type pointer, or null.
    void TypeRef(J& j, const char* key, const AbstractTypeClass* t)
    {
        if (!Readable(t, sizeof(AbstractTypeClass)))
        {
            j.Null(key);
            return;
        }
        j.Open(key, '{');
        j.Str("id", t->ID, sizeof(t->ID));
        j.Int("rtti", static_cast<int>(t->WhatAmI()));
        j.Int("index", t->GetArrayIndex());
        j.Close('}');
    }

    void BldList(J& j, const char* key, const TypeList<BuildingTypeClass*>& v)
    {
        j.Open(key, '[');
        if (v.Count > 0 && v.Count < 4096 && Readable(v.Items, sizeof(void*) * v.Count))
            for (int i = 0; i < v.Count; ++i)
                TypeRef(j, nullptr, v.Items[i]);
        j.Close(']');
    }

    void HouseMask(J& j, const char* key, DWORD mask)
    {
        j.Open(key, '{');
        j.Int("raw", static_cast<long long>(static_cast<int>(mask)));   // -1 == sentinel, see TechnoTypeClass::InRequiredHouses
        j.Open("houses", '[');
        DynamicVectorClass<HouseTypeClass*>& ha = HouseTypeClass::Array;
        for (int i = 0; i < ha.Count; ++i)
        {
            HouseTypeClass* h = ha.Items[i];
            if (!Readable(h, sizeof(HouseTypeClass))) continue;
            // same convention as HouseClass::InForbiddenHouses: 1u << HouseType->ArrayIndex2
            if (mask & (1u << h->ArrayIndex2))
                j.Str(nullptr, h->ID, sizeof(h->ID));
        }
        j.Close(']');
        j.Close('}');
    }

    // ---- v2 combat dump (rules_types.json "dump_version" 2). PUBLIC rules data only (rulesmd + map
    // overrides, identical for every player): read from the type objects, never from live objects.
    const char* ArmorName(int a)
    {
        // YRpp/GeneralDefinitions.h:590-602 enum class Armor (also the index into WarheadTypeClass::Verses)
        static const char* const n[11] = { "none", "flak", "plate", "light", "medium", "heavy", "wood",
            "steel", "concrete", "special_1", "special_2" };
        return (a >= 0 && a < 11) ? n[a] : "unknown";
    }

    const char* MovementZoneName(int m)
    {
        // YRpp/GeneralDefinitions.h:1007-1023
        static const char* const n[14] = { "None", "Normal", "Crusher", "Destroyer", "AmphibiousDestroyer",
            "AmphibiousCrusher", "Amphibious", "Subterrannean", "Infantry", "InfantryDestroyer", "Fly",
            "Water", "WaterBeach", "CrusherAll" };
        return (m >= -1 && m < 13) ? n[m + 1] : "unknown";
    }

    const char* SpeedTypeName(int m)
    {
        // YRpp/GeneralDefinitions.h:1330-1341
        static const char* const n[9] = { "None", "Foot", "Track", "Wheel", "Hover", "Winged", "Float",
            "Amphibious", "FloatBeach" };
        return (m >= -1 && m < 8) ? n[m + 1] : "unknown";
    }

    // Well-known YR locomotor CLSIDs; unknown -> "" (Python then uses the raw GUID string).
    const char* LocomotorName(const _GUID& g)
    {
        struct E { unsigned long d1; const char* n; };
        static const E tab[] = {
            { 0x4A582741ul, "Drive" },  { 0x4A582742ul, "Hover" },  { 0x4A582743ul, "Tunnel" },
            { 0x4A582744ul, "Walk" },   { 0x4A582746ul, "Fly" },    { 0x4A582747ul, "Teleport" },
            { 0x55D141B8ul, "Mech" },   { 0x92612C46ul, "Jumpjet" }, { 0xB7B49766ul, "Rocket" },
            { 0x2BEA74E1ul, "Ship" },
        };
        for (const E& e : tab)
            if (g.Data1 == e.d1) return e.n;
        return "";
    }

    void WarheadObj(J& j, const char* key, const WarheadTypeClass* w)
    {
        if (!Readable(w, sizeof(WarheadTypeClass))) { j.Null(key); return; }
        j.Open(key, '{');
        j.Str("id", w->ID, sizeof(w->ID));
        j.Open("verses", '[');                                  // WarheadTypeClass.h:67 double Verses[0xB], index = Armor enum
        for (int i = 0; i < 11; ++i) j.Dbl(nullptr, w->Verses[i]);
        j.Close(']');
        j.Dbl("prone_damage", w->ProneDamage);                  // :69
        j.Dbl("cell_spread", w->CellSpread);                    // :75
        j.Bool("wall", w->Wall);                                // :83
        j.Bool("wood", w->Wood);                                // :86
        j.Bool("tiberium", w->Tiberium);                        // :87
        j.Bool("sonic", w->Sonic);                              // :90
        j.Bool("fire", w->Fire);                                // :91
        j.Bool("conventional", w->Conventional);                // :92
        j.Bool("em_effect", w->EMEffect);                       // :99
        j.Bool("mind_control", w->MindControl);                 // :100
        j.Bool("poison", w->Poison);                            // :101
        j.Bool("parasite", w->Parasite);                        // :104
        j.Bool("temporal", w->Temporal);                        // :105
        j.Bool("airstrike", w->Airstrike);                      // :108
        j.Int("paralyzes", w->Paralyzes);                       // :111
        j.Bool("culling", w->Culling);                          // :112
        j.Bool("radiation", w->Radiation);                      // :115
        j.Bool("psychic_damage", w->PsychicDamage);             // :116
        j.Bool("affects_allies", w->AffectsAllies);             // :117
        j.Close('}');
    }

    void WeaponObj(J& j, const char* key, const WeaponStruct& ws)
    {
        const WeaponTypeClass* w = ws.WeaponType;
        if (!Readable(w, sizeof(WeaponTypeClass))) { j.Null(key); return; }
        j.Open(key, '{');
        j.Str("id", w->ID, sizeof(w->ID));
        j.Int("range_leptons", w->Range);                       // WeaponTypeClass.h:71 (int(256*ini))
        j.Dbl("range_cells", w->Range / 256.0);
        j.Int("min_range_leptons", w->MinimumRange);            // :72
        j.Dbl("min_range_cells", w->MinimumRange / 256.0);
        j.Int("damage", w->Damage);                             // :67
        j.Int("rof", w->ROF);                                   // :70
        j.Int("burst", w->Burst);                               // :65
        j.Int("projectile_speed", w->Speed);                    // :68
        j.Bool("omni_fire", w->OmniFire);                       // :85
        j.Bool("lobber", w->Lobber);                            // :88
        j.Bool("is_sonic", w->IsSonic);                         // :90
        j.Bool("spawner", w->Spawner);                          // :91
        j.Bool("decloak_to_fire", w->DecloakToFire);            // :93
        j.Bool("never_use", w->NeverUse);                       // :96
        j.Bool("infinite_mind_control", w->InfiniteMindControl);// :103
        j.Bool("fire_while_moving", w->FireWhileMoving);        // :104
        j.Bool("suicide", w->Suicide);                          // :107
        j.Bool("is_laser", w->IsLaser);                         // :112
        j.Bool("area_fire", w->AreaFire);                       // :119
        j.Bool("is_railgun", w->IsRailgun);                     // :87
        j.Bool("turret_locked", ws.TurretLocked);               // TechnoTypeClass.h:61
        const BulletTypeClass* b = w->Projectile;               // :66
        if (Readable(b, sizeof(BulletTypeClass)))
        {
            j.Open("projectile", '{');
            j.Str("id", b->ID, sizeof(b->ID));
            j.Bool("aa", b->AA);                                // BulletTypeClass.h:92 can target air
            j.Bool("ag", b->AG);                                // :93 can target ground
            j.Bool("arcing", b->Arcing);                        // :83
            j.Bool("inviso", b->Inviso);                        // :86
            j.Bool("level", b->Level);                          // :85
            j.Bool("inaccurate", b->Inaccurate);                // :90
            j.Close('}');
        }
        else
            j.Null("projectile");
        WarheadObj(j, "warhead", w->Warhead);                   // :69
        j.Close('}');
    }

    void AbilitiesObj(J& j, const char* key, const AbilitiesStruct& a)
    {
        j.Open(key, '{');                                       // TechnoTypeClass.h:20-39
        j.Bool("faster", a.FASTER); j.Bool("stronger", a.STRONGER); j.Bool("firepower", a.FIREPOWER);
        j.Bool("scatter", a.SCATTER); j.Bool("rof", a.ROF); j.Bool("sight", a.SIGHT); j.Bool("cloak", a.CLOAK);
        j.Bool("tiberium_proof", a.TIBERIUM_PROOF); j.Bool("vein_proof", a.VEIN_PROOF);
        j.Bool("self_heal", a.SELF_HEAL); j.Bool("explodes", a.EXPLODES);
        j.Bool("radar_invisible", a.RADAR_INVISIBLE); j.Bool("sensors", a.SENSORS);
        j.Bool("fearless", a.FEARLESS); j.Bool("c4", a.C4); j.Bool("tiberium_heal", a.TIBERIUM_HEAL);
        j.Bool("guard_area", a.GUARD_AREA); j.Bool("crusher", a.CRUSHER);
        j.Close('}');
    }

    void DumpCombat(J& j, TechnoTypeClass* t, AbstractType rtti)
    {
        j.Open("combat", '{');
        j.Int("strength", t->Strength);                              // ObjectTypeClass.h:77
        j.Int("armor", static_cast<int>(t->Armor));                  // ObjectTypeClass.h:76
        j.Str("armor_name", ArmorName(static_cast<int>(t->Armor)));
        j.Int("speed", t->Speed);                                    // TechnoTypeClass.h:319
        j.Int("speed_type", static_cast<int>(t->SpeedType));         // :320
        j.Str("speed_type_name", SpeedTypeName(static_cast<int>(t->SpeedType)));
        j.Int("movement_zone", static_cast<int>(t->MovementZone));   // :285
        j.Str("movement_zone_name", MovementZoneName(static_cast<int>(t->MovementZone)));
        {
            char g[40];
            const _GUID& L = t->Locomotor;                           // :210
            sprintf(g, "%08lX-%04X-%04X", L.Data1, static_cast<unsigned>(L.Data2), static_cast<unsigned>(L.Data3));
            j.Str("locomotor_guid", g);
            j.Str("locomotor", LocomotorName(L));
        }
        j.Bool("naval", t->Naval);                                   // :427
        j.Bool("considered_aircraft", t->ConsideredAircraft);        // :500
        j.Bool("is_air", rtti == AbstractType::AircraftType || t->ConsideredAircraft || t->JumpJet
            || t->MovementZone == MovementZone::Fly);                // derived (:498, :500, :285)
        j.Bool("jumpjet", t->JumpJet);                               // :498
        j.Bool("crusher", t->Crusher);                               // :452
        j.Bool("omni_crusher", t->OmniCrusher);                      // :453
        j.Bool("omni_crush_resistant", t->OmniCrushResistant);       // :454
        j.Bool("crushable", t->Crushable);                           // ObjectTypeClass.h:103
        j.Bool("selectable", t->Selectable);                         // ObjectTypeClass.h:106
        j.Int("sight", t->Sight);                                    // :292
        j.Int("guard_range", t->GuardRange);                         // :286
        j.Int("flight_level", t->FlightLevel);                       // :307
        j.Dbl("size", t->Size);                                      // :216
        j.Dbl("size_limit", t->SizeLimit);                           // :217
        j.Dbl("weight", t->Weight);                                  // :214
        j.Int("passengers", t->Passengers);                          // :290
        j.Bool("open_topped", t->OpenTopped);                        // :291
        j.Int("threat_posed", t->ThreatPosed);                       // :317
        j.Int("weapon_count", t->WeaponCount);                       // :386
        j.Int("turret_count", t->TurretCount);                       // :385
        j.Int("air_range_bonus", t->AirRangeBonus);                  // :324

        WeaponObj(j, "primary", t->Weapon[0]);                       // :390
        WeaponObj(j, "secondary", t->Weapon[1]);
        WeaponObj(j, "elite_primary", t->EliteWeapon[0]);            // :392
        WeaponObj(j, "elite_secondary", t->EliteWeapon[1]);

        j.Bool("cloakable", t->Cloakable);                           // :429
        j.Bool("can_disguise", t->CanDisguise);                      // :459
        j.Bool("perma_disguise", t->PermaDisguise);                  // :460
        j.Bool("detect_disguise", t->DetectDisguise);                // :461
        j.Bool("self_healing", t->SelfHealing);                      // :440
        j.Bool("organic", t->Organic);                               // :501
        j.Bool("teleporter", t->Teleporter);                         // :433
        j.Bool("gattling", t->IsGattling);                           // :434
        j.Bool("explodes", t->Explodes);                             // :441
        j.Bool("is_train", t->IsTrain);                              // :401
        j.Bool("resource_gatherer", t->ResourceGatherer);            // :293
        j.Bool("resource_destination", t->ResourceDestination);      // :294
        j.Bool("sensors", t->Sensors);                               // :410
        j.Int("sensors_sight", t->SensorsSight);                     // :297
        j.Bool("gap_generator", t->GapGenerator);                    // :430
        j.Bool("trainable", t->Trainable);                           // :395
        j.Bool("immune_to_psionics", t->ImmuneToPsionics);           // :465
        j.Bool("type_immune", t->TypeImmune);                        // :393
        j.Bool("opportunity_fire", t->OpportunityFire);              // :339
        j.Bool("mobile_fire", t->MobileFire);                        // :338
        j.Bool("deploy_fire", t->DeployFire);                        // :336
        j.Bool("distributed_fire", t->DistributedFire);              // :340
        j.Bool("attack_friendlies", t->AttackFriendlies);            // :345
        j.Bool("prevent_attack_move", t->PreventAttackMove);         // :348
        j.Bool("can_passive_aquire", t->CanPassiveAquire);           // :503
        j.Bool("can_retaliate", t->CanRetaliate);                    // :504
        j.Bool("default_to_guard_area", t->DefaultToGuardArea);      // :469
        j.Int("ammo", t->Ammo);                                      // :322
        j.Int("initial_ammo", t->InitialAmmo);                       // :321
        j.Int("reload", t->Reload);                                  // :331
        j.Bool("spawned", t->Spawned);                               // :480 (is a spawn, e.g. Kirov drone)
        j.Int("spawns_number", t->SpawnsNumber);                     // :482
        TypeRef(j, "spawns", t->Spawns);                             // :481
        j.Bool("slaved", t->Slaved);                                 // :474
        TypeRef(j, "enslaves", t->Enslaves);                         // :475
        j.Int("slaves_number", t->SlavesNumber);                     // :476
        TypeRef(j, "deploys_into", t->DeploysInto);                  // :237
        TypeRef(j, "undeploys_into", t->UndeploysInto);              // :238
        AbilitiesObj(j, "veteran_abilities", t->VeteranAbilities);   // :194
        AbilitiesObj(j, "elite_abilities", t->EliteAbilities);       // :195
        if (rtti == AbstractType::UnitType)
        {
            UnitTypeClass* u = static_cast<UnitTypeClass*>(t);
            j.Bool("harvester", u->Harvester);                       // UnitTypeClass.h:61
            j.Bool("weeder", u->Weeder);                             // :62
            j.Bool("deploy_to_fire", u->DeployToFire);               // :65
            j.Bool("simple_deployer", u->IsSimpleDeployer);          // :66
        }
        else if (rtti == AbstractType::InfantryType)
        {
            InfantryTypeClass* i = static_cast<InfantryTypeClass*>(t);
            j.Bool("engineer", i->Engineer);                         // InfantryTypeClass.h:109
            j.Bool("infiltrate", i->Infiltrate);                     // :104
            j.Bool("agent", i->Agent);                               // :110
            j.Bool("thief", i->Thief);                               // :111
            j.Bool("vehicle_thief", i->VehicleThief);                // :112
            j.Bool("deployer", i->Deployer);                         // :114
            j.Bool("c4", i->C4);                                     // :108
            j.Bool("cyborg", i->Cyborg);                             // :95
            j.Bool("fearless", i->Fearless);                         // :102
            j.Bool("crawls", i->Crawls);                             // :103
            j.Bool("occupier", i->Occupier);                         // :99
            j.Bool("civilian", i->Civilian);                         // :107
        }
        else if (rtti == AbstractType::AircraftType)
        {
            AircraftTypeClass* a = static_cast<AircraftTypeClass*>(t);
            j.Bool("carryall", a->Carryall);                         // AircraftTypeClass.h:49
            j.Bool("fighter", a->Fighter);                           // :58
            j.Bool("airport_bound", a->AirportBound);                // :57
            j.Bool("landable", a->Landable);                         // :54
        }
        else if (rtti == AbstractType::BuildingType)
        {
            BuildingTypeClass* b = static_cast<BuildingTypeClass*>(t);
            j.Bool("capturable", b->Capturable);                     // BuildingTypeClass.h:208
            j.Bool("can_c4", b->CanC4);                              // :213
            j.Bool("can_be_occupied", b->CanBeOccupied);             // :217
            j.Int("max_occupants", b->MaxNumberOccupants);           // :219
            j.Bool("bunker", b->Bunker);                             // :240
            j.Bool("hospital", b->Hospital);                         // :262
            j.Bool("armory", b->Armory);                             // :263
            j.Bool("radar", b->Radar);                               // :233
            j.Int("superweapon", b->SuperWeapon);                    // :285
            j.Int("superweapon2", b->SuperWeapon2);                  // :286
        }
        j.Close('}');
    }

    void DumpTechno(J& j, TechnoTypeClass* t, int arrayIndex, AbstractType rtti, HouseClass* pAgent)
    {
        j.Open(nullptr, '{');
        j.Str("category", rtti == AbstractType::BuildingType ? "building" : rtti == AbstractType::UnitType ? "unit"
            : rtti == AbstractType::InfantryType ? "infantry" : "aircraft");
        j.Int("rtti", static_cast<int>(rtti));
        j.Int("index", arrayIndex);
        j.Str("id", t->ID, sizeof(t->ID));
        j.Str("ui_label", t->UINameLabel, sizeof(t->UINameLabel));
        j.Wide("ui_name", t->UIName);
        j.Int("cost", t->Cost);
        j.Int("tech_level", t->TechLevel);
        j.Int("build_limit", t->BuildLimit);
        j.Bool("naval", t->Naval);
        j.Bool("unbuildable", t->Unbuildable);
        j.IntList("prerequisite_raw", t->Prerequisite);                   // >=0: BuildingType index; <0: generic group
        j.IntList("prerequisite_override_raw", t->PrerequisiteOverride);
        HouseMask(j, "owners", t->GetOwners());
        HouseMask(j, "required_houses", t->RequiredHouses);
        HouseMask(j, "forbidden_houses", t->ForbiddenHouses);

        if (rtti == AbstractType::BuildingType)
        {
            BuildingTypeClass* b = static_cast<BuildingTypeClass*>(t);
            j.Int("power_bonus", b->PowerBonus);
            j.Int("power_drain", b->PowerDrain);
            j.Int("foundation_enum", static_cast<int>(b->Foundation));
            j.Int("foundation_w", b->GetFoundationWidth());
            j.Int("foundation_h", b->GetFoundationHeight(false));
            j.Int("adjacent", b->Adjacent);
            j.Int("build_cat", static_cast<int>(b->BuildCat));
            j.Str("powers_up_building", b->PowersUpBuilding, sizeof(b->PowersUpBuilding));
            j.Int("powers_up_to_level", b->PowersUpToLevel);
            j.Int("factory_type", static_cast<int>(b->Factory));
            j.Bool("weapons_factory", b->WeaponsFactory);
            j.Bool("construction_yard", b->ConstructionYard);
            j.Bool("refinery", b->Refinery);
            j.Bool("helipad", b->Helipad);
            j.Bool("is_base_defense", b->IsBaseDefense);
            j.Bool("ai_build_this", b->AIBuildThis);
            j.Bool("gdi_barracks", b->GDIBarracks);
            j.Bool("nod_barracks", b->NODBarracks);
            j.Bool("yuri_barracks", b->YuriBarracks);
            j.Bool("sensor_array", b->SensorArray);
            j.Bool("ore_purifier", b->OrePurifier);
            j.Bool("factory_plant", b->FactoryPlant);
        }

        DumpCombat(j, t, rtti);

        if (pAgent)
        {
            // Same call as the PRODUCE path (Bridge.Act.cpp DoProduce): HouseClass::CanBuild(type, false, true).
            // JMP_THIS(0x4F7870), declared const in YRpp/HouseClass.h:661; the sidebar calls it for every
            // type each refresh, so it is a pure query.
            CanBuildResult r = pAgent->CanBuild(t, false, true);
            j.Int("can_build_result", static_cast<int>(r));
            j.Bool("can_build_now", r == CanBuildResult::Buildable);
        }
        j.Close('}');
    }

    template <class T>
    int DumpTypeArray(J& j, DynamicVectorClass<T*>& a, AbstractType rtti, HouseClass* pAgent)
    {
        int n = 0;
        for (int i = 0; i < a.Count; ++i)
        {
            T* t = a.Items[i];
            if (!Readable(t, sizeof(TechnoTypeClass))) continue;
            DumpTechno(j, t, t->ArrayIndex, rtti, pAgent);
            ++n;
        }
        return n;
    }

    int DumpTypes(HouseClass* pAgent)
    {
        FILE* f = fopen("rules_types.json", "w");
        if (!f) return 0;
        J j(f);
        int n = 0;
        j.Open(nullptr, '{');
        j.Int("dump_version", 2);   // 1 = M4 (no "combat" objects); 2 = + per-type "combat" (M6)
        j.Int("agent_house_index", pAgent ? pAgent->ArrayIndex : -1);
        j.Open("types", '[');
        n += DumpTypeArray(j, BuildingTypeClass::Array, AbstractType::BuildingType, pAgent);
        n += DumpTypeArray(j, UnitTypeClass::Array, AbstractType::UnitType, pAgent);
        n += DumpTypeArray(j, InfantryTypeClass::Array, AbstractType::InfantryType, pAgent);
        n += DumpTypeArray(j, AircraftTypeClass::Array, AbstractType::AircraftType, pAgent);
        j.Close(']');
        j.Close('}');
        fputc('\n', f);
        fclose(f);
        return n;
    }

    int DumpRulesAI()
    {
        RulesClass* r = RulesClass::Instance;
        if (!Readable(r, sizeof(RulesClass)))
            return 0;
        FILE* f = fopen("rules_ai.json", "w");
        if (!f) return 0;
        J j(f);
        j.Open(nullptr, '{');

        j.Open("build_lists", '{');
        BldList(j, "Shipyard", r->Shipyard);
        BldList(j, "BuildConst", r->BuildConst);
        BldList(j, "BuildPower", r->BuildPower);
        BldList(j, "BuildRefinery", r->BuildRefinery);
        BldList(j, "BuildBarracks", r->BuildBarracks);
        BldList(j, "BuildTech", r->BuildTech);
        BldList(j, "BuildWeapons", r->BuildWeapons);
        BldList(j, "BuildDefense", r->BuildDefense);
        BldList(j, "BuildPDefense", r->BuildPDefense);
        BldList(j, "BuildAA", r->BuildAA);
        BldList(j, "BuildHelipad", r->BuildHelipad);
        BldList(j, "BuildRadar", r->BuildRadar);
        BldList(j, "BuildNavalYard", r->BuildNavalYard);
        BldList(j, "BuildDummy", r->BuildDummy);
        BldList(j, "AlliedBaseDefenses", r->AlliedBaseDefenses);
        BldList(j, "SovietBaseDefenses", r->SovietBaseDefenses);
        BldList(j, "ThirdBaseDefenses", r->ThirdBaseDefenses);
        BldList(j, "ConcreteWalls", r->ConcreteWalls);
        BldList(j, "NeutralTechBuildings", r->NeutralTechBuildings);
        j.Close('}');

        // Generic prerequisite groups: lists of BuildingType indices.
        j.Open("prerequisite_groups", '{');
        j.IntList("PrerequisitePower", r->PrerequisitePower);
        j.IntList("PrerequisiteFactory", r->PrerequisiteFactory);
        j.IntList("PrerequisiteBarracks", r->PrerequisiteBarracks);
        j.IntList("PrerequisiteRadar", r->PrerequisiteRadar);
        j.IntList("PrerequisiteTech", r->PrerequisiteTech);
        j.IntList("PrerequisiteProc", r->PrerequisiteProc);
        j.Close('}');

        j.Open("scalars", '{');
        j.Dbl("AirstripRatio", r->AirstripRatio);   j.Int("AirstripLimit", r->AirstripLimit);
        j.Dbl("HelipadRatio", r->HelipadRatio);     j.Int("HelipadLimit", r->HelipadLimit);
        j.Dbl("TeslaRatio", r->TeslaRatio);         j.Int("TeslaLimit", r->TeslaLimit);
        j.Dbl("AARatio", r->AARatio);               j.Int("AALimit", r->AALimit);
        j.Dbl("DefenseRatio", r->DefenseRatio);     j.Int("DefenseLimit", r->DefenseLimit);
        j.Dbl("WarRatio", r->WarRatio);             j.Int("WarLimit", r->WarLimit);
        j.Dbl("BarracksRatio", r->BarracksRatio);   j.Int("BarracksLimit", r->BarracksLimit);
        j.Dbl("RefineryRatio", r->RefineryRatio);   j.Int("RefineryLimit", r->RefineryLimit);
        j.Int("PowerSurplus", r->PowerSurplus);
        j.Dbl("PowerEmergency", r->PowerEmergency);
        j.Int("BaseSizeAdd", r->BaseSizeAdd);
        j.Int("InfantryReserve", r->InfantryReserve);
        j.Int("InfantryBaseMult", r->InfantryBaseMult);
        j.Int("AIBaseSpacing", r->AIBaseSpacing);
        j.Int("AISafeDistance", r->AISafeDistance);
        j.Int("AINavalYardAdjacency", r->AINavalYardAdjacency);
        j.Int("AIRestrictReplaceTime", r->AIRestrictReplaceTime);
        j.Int("AISuperDefenseFrames", r->AISuperDefenseFrames);
        j.Dbl("AISuperDefenseDistance", r->AISuperDefenseDistance);
        j.Dbl("AIMinorSuperReadyPercent", r->AIMinorSuperReadyPercent);
        j.Dbl("AIUseTurbineUpgradeProbability", r->AIUseTurbineUpgradeProbability);
        j.Dbl("AITriggerSuccessWeightDelta", r->AITriggerSuccessWeightDelta);
        j.Dbl("AITriggerFailureWeightDelta", r->AITriggerFailureWeightDelta);
        j.Dbl("AITriggerTrackRecordCoefficient", r->AITriggerTrackRecordCoefficient);
        j.Int("DissolveUnfilledTeamDelay", r->DissolveUnfilledTeamDelay);
        j.Int("AIAlternateProductionCreditCutoff", r->AIAlternateProductionCreditCutoff);
        j.Int("MaximumBuildingPlacementFailures", r->MaximumBuildingPlacementFailures);
        j.Bool("AIBuildsWalls", r->AIBuildsWalls);
        j.Bool("NodAIBuildsWalls", r->NodAIBuildsWalls);
        j.Int("AIDifficultyStruct_raw", r->AIDifficultyStruct);   // meaning uncertain
        j.Int("AIPlayers", r->AIPlayers);
        j.Close('}');

        // TypeList<int>: per-difficulty where the INI gives 3 values (index 0..2 = Easy/Normal/Hard, assumed)
        j.Open("int_lists", '{');
        j.IntList("TeamDelays", r->TeamDelays);
        j.IntList("AIHateDelays", r->AIHateDelays);
        j.IntList("MinimumAIDefensiveTeams", r->MinimumAIDefensiveTeams);
        j.IntList("MaximumAIDefensiveTeams", r->MaximumAIDefensiveTeams);
        j.IntList("TotalAITeamCap", r->TotalAITeamCap);
        j.IntList("AIForcePredictionFudge", r->AIForcePredictionFudge);
        j.IntList("AIPickWallDefensePercent", r->AIPickWallDefensePercent);
        j.IntList("AIAutoDeployFrameDelay", r->AIAutoDeployFrameDelay);
        j.IntList("AICaptureNormal", r->AICaptureNormal);
        j.IntList("AICaptureWounded", r->AICaptureWounded);
        j.IntList("AICaptureLowPower", r->AICaptureLowPower);
        j.IntList("AICaptureLowMoney", r->AICaptureLowMoney);
        j.IntList("AISuperDefenseProbability", r->AISuperDefenseProbability);
        j.IntList("AIVirtualPurifiers", r->AIVirtualPurifiers);
        j.IntList("AISlaveMinerNumber", r->AISlaveMinerNumber);
        j.IntList("AIExtraRefineries", r->AIExtraRefineries);
        j.IntList("MultiplayerAICM", r->MultiplayerAICM);
        j.IntList("AlliedBaseDefenseCounts", r->AlliedBaseDefenseCounts);
        j.IntList("SovietBaseDefenseCounts", r->SovietBaseDefenseCounts);
        j.IntList("ThirdBaseDefenseCounts", r->ThirdBaseDefenseCounts);
        j.Close('}');

        j.Close('}');
        fputc('\n', f);
        fclose(f);
        return 1;
    }

    void IdOrNull(J& j, const char* key, const AbstractTypeClass* t)
    {
        if (Readable(t, sizeof(AbstractTypeClass))) j.Str(key, t->ID, sizeof(t->ID));
        else j.Null(key);
    }

    // Waypoint lookup through the engine/Phobos accessors (Phobos hooks 0x68BDC0/0x68BD80/0x68BCC0 and keeps the data in
    // its scenario extension, so ScenarioClass::Waypoints is all zeros). IsDefinedWaypoint MUST be checked first: the
    // Phobos getter can insert an undefined entry. Returns false for undefined slots.
    bool WaypointCell(int i, short& x, short& y)
    {
        ScenarioClass* sc = ScenarioClass::Instance;
        if (i < 0 || i >= 702 || !Readable(sc, sizeof(ScenarioClass))) return false;
        if (!sc->IsDefinedWaypoint(i)) return false;
        CellStruct cs = sc->GetWaypointCoords(i);
        if (cs.X < 0 || cs.Y < 0) return false;
        x = cs.X; y = cs.Y;
        return true;
    }

    // Coordinate-bearing script arguments are redacted (-1) when the destination is shrouded, so private
    // scenario scripts cannot disclose unexplored locations. Covered actions: 4 (packed cell: x=arg%128,
    // y=arg/128) and the waypoint-index actions 1 (attack waypoint), 3 (move to waypoint), 15 (spy on
    // building at waypoint), 16 (patrol to waypoint), 59 (select building at waypoint): the complete coordinate-bearing set per dispatch audit. Waypoints 0..7 (player starts) are public.
    // Not covered: any other action whose argument is a waypoint or cell (none identified).
    int RedactScriptArg(int action, int arg)
    {
        short x, y;
        if (action == 4)
        {
            x = static_cast<short>(arg % 128); y = static_cast<short>(arg / 128);   // engine decode: signed div, short truncation
        }
        else if (action == 1 || action == 3 || action == 15 || action == 16 || action == 59)
        {
            if (arg < 0 || arg >= 702) return -1;
            if (arg < 8 && !SessionClass::IsCampaign()) return arg;   // start waypoints public only in MP/skirmish
            if (!WaypointCell(arg, x, y)) return arg;      // undefined waypoint: nothing to hide
        }
        else return arg;
        CellStruct cs; cs.X = x; cs.Y = y;
        CellClass* c = MapClass::Instance.TryGetCellAt(cs);
        if (!c || c->IsShrouded()) return -1;
        return arg;
    }

    int DumpScripts(int counts[5])
    {
        FILE* f = fopen("ai_scripts.json", "w");
        if (!f) return 0;
        J j(f);
        j.Open(nullptr, '{');

        // ---- AITriggerTypes
        j.Open("ai_triggers", '[');
        {
            DynamicVectorClass<AITriggerTypeClass*>& a = AITriggerTypeClass::Array;
            for (int i = 0; i < a.Count; ++i)
            {
                AITriggerTypeClass* t = a.Items[i];
                if (!Readable(t, sizeof(AITriggerTypeClass))) continue;
                ++counts[0];
                j.Open(nullptr, '{');
                j.Int("index", i);
                j.Str("id", t->ID, sizeof(t->ID));
                j.Str("name", t->Name, sizeof(t->Name));
                IdOrNull(j, "team1", t->Team1);
                IdOrNull(j, "team2", t->Team2);
                j.Int("owner_house_type", static_cast<int>(t->OwnerHouseType));   // 0 None / 1 Single / 2 Any
                j.Int("house_index", t->HouseIndex);
                if (t->OwnerHouseType == AITriggerHouseType::Single && t->HouseIndex >= 0
                    && t->HouseIndex < HouseTypeClass::Array.Count)
                    IdOrNull(j, "house_id", HouseTypeClass::Array.Items[t->HouseIndex]);
                else
                    j.Null("house_id");
                j.Int("side_index", t->SideIndex);
                j.Int("tech_level", t->TechLevel);
                j.Int("condition_type", static_cast<int>(t->ConditionType));
                {
                    static const char* const cn[] = { "AIOwns", "EnemyOwns", "EnemyYellowPower", "EnemyRedPower",
                                                      "EnemyCashExceeds", "IronCharged", "ChronoCharged", "NeutralOwns" };
                    int c = static_cast<int>(t->ConditionType);
                    j.Str("condition_name", c >= 0 && c < 8 ? cn[c] : (c == -1 ? "Pool" : "?"));
                }
                IdOrNull(j, "condition_object", t->ConditionObject);
                j.Open("conditions", '[');   // 4 raw comparator slots ("don't ask" in YRpp)
                {
                    static const char* const on[] = { "<", "<=", "==", ">=", ">", "!=" };
                    for (int k = 0; k < 4; ++k)
                    {
                        unsigned ty = static_cast<unsigned>(t->Conditions[k].ComparatorType);
                        j.Open(nullptr, '{');
                        j.Int("operand", t->Conditions[k].ComparatorOperand);
                        j.Int("operator", ty);
                        j.Str("operator_str", ty < 6 ? on[ty] : "?");
                        j.Close('}');
                    }
                }
                j.Close(']');
                // weights (Weight_Current/min/max) deliberately NOT dumped: Weight_Current is runtime history and the
                // teacher takes weights from the stock INIs.

                j.Bool("is_for_skirmish", t->IsForSkirmish);
                j.Bool("is_base_defense", t->IsForBaseDefense);
                j.Bool("enabled_easy", t->Enabled_Easy);
                j.Bool("enabled_normal", t->Enabled_Normal);
                j.Bool("enabled_hard", t->Enabled_Hard);
                j.Bool("is_enabled", t->IsEnabled);
                j.Int("is_global", t->IsGlobal);
                j.Int("unknown_B4", t->unknown_B4);
                j.Close('}');
            }
        }
        j.Close(']');

        // ---- TeamTypes
        j.Open("team_types", '[');
        {
            DynamicVectorClass<TeamTypeClass*>& a = TeamTypeClass::Array;
            for (int i = 0; i < a.Count; ++i)
            {
                TeamTypeClass* t = a.Items[i];
                if (!Readable(t, sizeof(TeamTypeClass))) continue;
                ++counts[1];
                j.Open(nullptr, '{');
                j.Int("index", t->ArrayIndex);
                j.Str("id", t->ID, sizeof(t->ID));
                j.Str("name", t->Name, sizeof(t->Name));
                IdOrNull(j, "task_force", t->TaskForce);
                IdOrNull(j, "script", t->ScriptType);
                j.Int("max", t->Max);
                j.Int("priority", t->Priority);
                j.Int("group", t->Group);
                j.Int("veteran_level", t->VeteranLevel);
                j.Int("house_idx", t->idxHouse);
                j.Int("tech_level", t->TechLevel);
                j.Int("waypoint", t->Waypoint);
                j.Int("transport_waypoint", t->TransportWaypoint);
                j.Int("is_global", t->IsGlobal);
                j.Int("mind_control_decision", t->MindControlDecision);
                j.Bool("has_tag", t->Tag != nullptr);
                j.Bool("loadable", t->Loadable);
                j.Bool("full", t->Full);
                j.Bool("annoyance", t->Annoyance);
                j.Bool("guard_slower", t->GuardSlower);
                j.Bool("recruiter", t->Recruiter);
                j.Bool("autocreate", t->Autocreate);
                j.Bool("prebuild", t->Prebuild);
                j.Bool("reinforce", t->Reinforce);
                j.Bool("whiner", t->Whiner);
                j.Bool("aggressive", t->Aggressive);
                j.Bool("loose_recruit", t->LooseRecruit);
                j.Bool("suicide", t->Suicide);
                j.Bool("droppod", t->Droppod);
                j.Bool("use_transport_origin", t->UseTransportOrigin);
                j.Bool("dropship_loadout", t->DropshipLoadout);
                j.Bool("on_trans_only", t->OnTransOnly);
                j.Bool("avoid_threats", t->AvoidThreats);
                j.Bool("ion_immune", t->IonImmune);
                j.Bool("transports_return_on_unload", t->TransportsReturnOnUnload);
                j.Bool("are_team_members_recruitable", t->AreTeamMembersRecruitable);
                j.Bool("is_base_defense", t->IsBaseDefense);
                j.Bool("only_target_house_enemy", t->OnlyTargetHouseEnemy);
                j.Close('}');
            }
        }
        j.Close(']');

        // ---- TaskForces
        j.Open("task_forces", '[');
        {
            DynamicVectorClass<TaskForceClass*>& a = TaskForceClass::Array;
            for (int i = 0; i < a.Count; ++i)
            {
                TaskForceClass* t = a.Items[i];
                if (!Readable(t, sizeof(TaskForceClass))) continue;
                ++counts[2];
                j.Open(nullptr, '{');
                j.Int("index", i);
                j.Str("id", t->ID, sizeof(t->ID));
                j.Str("name", t->Name, sizeof(t->Name));
                j.Int("group", t->Group);
                j.Int("count_entries", t->CountEntries);
                j.Open("entries", '[');
                int ne = t->CountEntries;
                if (ne > 6) ne = 6;
                for (int k = 0; k < ne; ++k)
                {
                    j.Open(nullptr, '{');
                    j.Int("count", t->Entries[k].Amount);
                    TypeRef(j, "type", t->Entries[k].Type);
                    j.Close('}');
                }
                j.Close(']');
                j.Close('}');
            }
        }
        j.Close(']');

        // ---- ScriptTypes. YRpp has NO action enum -> raw numbers only.
        j.Open("script_types", '[');
        {
            DynamicVectorClass<ScriptTypeClass*>& a = ScriptTypeClass::Array;
            for (int i = 0; i < a.Count; ++i)
            {
                ScriptTypeClass* t = a.Items[i];
                if (!Readable(t, sizeof(ScriptTypeClass))) continue;
                ++counts[3];
                j.Open(nullptr, '{');
                j.Int("index", t->ArrayIndex);
                j.Str("id", t->ID, sizeof(t->ID));
                j.Str("name", t->Name, sizeof(t->Name));
                j.Open("actions", '[');
                int na = t->ActionsCount;
                if (na > 50) na = 50;
                for (int k = 0; k < na; ++k)
                {
                    j.Open(nullptr, '[');
                    j.Int(nullptr, t->ScriptActions[k].Action);
                    j.Int(nullptr, RedactScriptArg(t->ScriptActions[k].Action, t->ScriptActions[k].Argument));
                    j.Close(']');
                }
                j.Close(']');
                j.Close('}');
            }
        }
        j.Close(']');

        // ---- Map waypoints (702 slots; via IsDefinedWaypoint/GetWaypointCoords, undefined skipped)
        j.Open("waypoints", '[');
        {
            for (int i = 0; i < 702; ++i)
                {
                    short x, y;
                    if (!WaypointCell(i, x, y)) continue;   // undefined slots are skipped, never emitted as zeros
                    if (i >= 8 || SessionClass::IsCampaign())   // 0..7 = player starts: public via the lobby preview in MP/skirmish only
                    {
                        CellStruct wc; wc.X = x; wc.Y = y;
                        CellClass* wcell = MapClass::Instance.TryGetCellAt(wc);
                        if (!wcell || wcell->IsShrouded()) continue;
                    }
                    ++counts[4];
                    j.Open(nullptr, '[');
                    j.Int(nullptr, i);
                    j.Int(nullptr, x);
                    j.Int(nullptr, y);
                    j.Close(']');
                }
        }
        j.Close(']');

        j.Close('}');
        fputc('\n', f);
        fclose(f);
        return 1;
    }
}

void Bridge::ResetRuleDump()
{
    g_rulesDumped = false;
}

void Bridge::DumpRulesOnce(HouseClass* pAgent)
{
    if (g_rulesDumped)
        return;
    g_rulesDumped = true;     // set first: never retry (and never re-enter) within a match

    int nTypes = DumpTypes(pAgent);
    int okAI = DumpRulesAI();
    int c[5] = { 0, 0, 0, 0, 0 };
    int okScr = DumpScripts(c);
    Debug::Log("[BRIDGE] rule dump: types=%d rules_ai=%d scripts=%d aitriggers=%d teamtypes=%d taskforces=%d scripttypes=%d waypoints=%d\n",
        nTypes, okAI, okScr, c[0], c[1], c[2], c[3], c[4]);
}

// ============================================================================================
//  v6 placement hints. Legality = BuildingTypeClass::CanPlaceHere(CellStruct*, HouseClass*)
//  (YRpp/BuildingTypeClass.h:71, JMP_THIS 0x464AC0) -- the SAME check DoPlace uses to reject
//  PLACE_INVALID_CELL, so a cell listed here is exactly a cell PLACE will accept. Pure query.
// ============================================================================================
namespace
{
    struct Cand { short x, y; unsigned char dist, ore; };

    Cand g_cands[6000];
    unsigned char g_visited[512 * 512];
    BridgePlaceCell g_cache[BridgeContract::N_PLACE];
    int g_cacheN = 0;
    int g_cacheType = -1;
    int g_cacheBldCount = -1;
    int g_lastComputeFrame = -1000000;
    int g_scoreStartBal = INT_MIN;

    int __cdecl CmpCand(const void* a, const void* b)
    {
        const Cand* x = static_cast<const Cand*>(a);
        const Cand* y = static_cast<const Cand*>(b);
        return static_cast<int>(x->dist) - static_cast<int>(y->dist);
    }

    int OreAround(int cx, int cy)
    {
        int sum = 0;
        for (int dy = -6; dy <= 6; ++dy)
            for (int dx = -6; dx <= 6; ++dx)
            {
                CellStruct cs;
                cs.X = static_cast<short>(cx + dx);
                cs.Y = static_cast<short>(cy + dy);
                CellClass* c = MapClass::Instance.TryGetCellAt(cs);
                if (c)
                {
                    int v = Bridge::SeenOre(c);
                    if (v > 0) sum += v;
                    if (sum >= 255) return 255;
                }
            }
        return sum;
    }

    // The agent's first factory holding a completed BUILDING (same condition as read_factories).
    BuildingTypeClass* ReadyBuildingType(HouseClass* pAgent)
    {
        DynamicVectorClass<FactoryClass*>& facs = FactoryClass::Array;
        for (int i = 0; i < facs.Count; ++i)
        {
            FactoryClass* f = facs.Items[i];
            if (!f || f->Owner != pAgent) continue;
            TechnoClass* obj = f->Object;
            if (!obj || obj->WhatAmI() != AbstractType::Building) continue;
            if (!f->IsDone()) continue;
            TechnoTypeClass* tt = obj->GetTechnoType();
            if (tt) return static_cast<BuildingTypeClass*>(tt);
        }
        return nullptr;
    }

    int ComputeCells(HouseClass* pAgent, BuildingTypeClass* bt, BridgePlaceCell* out)
    {
        const LTRBStruct& mb = MapClass::Instance.MapCoordBounds;
        DynamicVectorClass<BuildingClass*>& bl = pAgent->Buildings;
        const int nb = bl.Count;
        if (nb <= 0 || nb > 4096) return 0;

        static short cyx[64], cyy[64];
        int ncy = 0;
        for (int i = 0; i < nb; ++i)
        {
            BuildingClass* b = bl.Items[i];
            if (!b || b->InLimbo || !b->Type) continue;
            if (b->Type->ConstructionYard && ncy < 64)
            {
                CellStruct m = b->GetMapCoords();
                cyx[ncy] = m.X; cyy[ncy] = m.Y; ++ncy;
            }
        }
        memset(g_visited, 0, sizeof(g_visited));
        int nc = 0, tests = 0;
        const int maxCand = static_cast<int>(sizeof(g_cands) / sizeof(g_cands[0]));
        const int maxTests = 12000;

        for (int pass = 0; pass < 2 && tests < maxTests; ++pass)
            for (int i = 0; i < nb && tests < maxTests; ++i)
            {
                BuildingClass* b = bl.Items[i];
                if (!b || b->InLimbo || !b->Type) continue;
                if ((pass == 0) != (b->Type->ConstructionYard != 0)) continue;
                CellStruct m = b->GetMapCoords();
                for (int y = m.Y - 20; y <= m.Y + 20 && tests < maxTests; ++y)
                    for (int x = m.X - 20; x <= m.X + 20; ++x)
                    {
                        if (x < 0 || y < 0 || x >= 512 || y >= 512) continue;
                        if (x < mb.Left || x > mb.Right || y < mb.Top || y > mb.Bottom) continue;
                        unsigned char& v = g_visited[y * 512 + x];
                        if (v) continue;
                        v = 1;
                        CellStruct cs;
                        cs.X = static_cast<short>(x);
                        cs.Y = static_cast<short>(y);
                        if (!MapClass::Instance.TryGetCellAt(cs)) continue;
                        if (++tests > maxTests) break;
                        if (!Bridge::PlaceAllowed(bt, x, y, pAgent)) continue;   // shroud + human proximity rule, before the live query
                        if (bt->PowersUpBuilding[0] == '\0' && !bt->CanPlaceHere(&cs, pAgent)) continue;   // upgrades: host check in PlaceAllowed
                        if (nc >= maxCand) continue;
                        int best = 255;
                        for (int k = 0; k < ncy; ++k)
                        {
                            int dx = x - cyx[k], dy = y - cyy[k];
                            if (dx < 0) dx = -dx;
                            if (dy < 0) dy = -dy;
                            int d = dx > dy ? dx : dy;
                            if (d < best) best = d;
                        }
                        if (ncy == 0)   // no conyard: distance to the anchor building
                        {
                            int dx = x - m.X, dy = y - m.Y;
                            if (dx < 0) dx = -dx;
                            if (dy < 0) dy = -dy;
                            best = dx > dy ? dx : dy;
                        }
                        g_cands[nc].x = cs.X; g_cands[nc].y = cs.Y;
                        g_cands[nc].dist = static_cast<unsigned char>(best > 255 ? 255 : best);
                        g_cands[nc].ore = 0;
                        ++nc;
                    }
            }

        qsort(g_cands, nc, sizeof(Cand), CmpCand);
        int n = nc < BridgeContract::N_PLACE ? nc : BridgeContract::N_PLACE;
        for (int i = 0; i < n; ++i)
        {
            out[i].x = g_cands[i].x;
            out[i].y = g_cands[i].y;
            out[i].dist = g_cands[i].dist;
            out[i].ore = static_cast<uint8_t>(OreAround(g_cands[i].x, g_cands[i].y));
        }
        return n;
    }
}

void Bridge::ResetPlacement()
{
    g_cacheN = 0;
    g_cacheType = -1;
    g_cacheBldCount = -1;
    g_lastComputeFrame = -1000000;
    g_scoreStartBal = INT_MIN;
}

void Bridge::UpdatePlacement(HouseClass* pAgent, int frame, bool decision, bool lockstep,
                             int32_t& outType, uint8_t& outRtti, uint16_t& outN, BridgePlaceCell* outCells)
{
    outType = -1; outRtti = 0; outN = 0;
    if (!pAgent) return;
    BuildingTypeClass* bt = ReadyBuildingType(pAgent);
    if (!bt)
    {
        g_cacheType = -1;
        g_cacheN = 0;
        return;
    }
    const int typeIdx = bt->ArrayIndex;
    const int bc = pAgent->Buildings.Count;
    const bool changed = (typeIdx != g_cacheType) || (bc != g_cacheBldCount);
    const bool due = lockstep ? decision : (frame - g_lastComputeFrame >= 15);
    if (changed || due)
    {
        g_cacheN = ComputeCells(pAgent, bt, g_cache);
        g_cacheType = typeIdx;
        g_cacheBldCount = bc;
        g_lastComputeFrame = frame;
    }
    outType = typeIdx;
    outRtti = static_cast<uint8_t>(AbstractType::BuildingType);
    outN = static_cast<uint16_t>(g_cacheN);
    for (int i = 0; i < g_cacheN; ++i) outCells[i] = g_cache[i];
}

// ---- v7 score counters (read-only). Fields: YRpp/HouseClass.h:918-919 (CreditsSpent/HarvestedCredits),
// :921 (StolenBuildingsCredits), :935-938 (Killed*Types UnitTrackerClass, UnitTotals[0x200] at :42),
// :976-981 ("for endgame score screen": Total/KilledUnitsOfHouses/BuildingsOfHouses[20]).
namespace
{
    uint32_t SumTracker(UnitTrackerClass& t)
    {
        long long s = 0;
        for (int i = 0; i < 0x200; ++i)
            if (t.UnitTotals[i] > 0) s += t.UnitTotals[i];
        return s > 0x7FFFFFFF ? 0x7FFFFFFFu : static_cast<uint32_t>(s);
    }
    uint32_t Pos(int v) { return v > 0 ? static_cast<uint32_t>(v) : 0u; }
}

// Fairness-gated ore read (shared by the vision grid and the placement hints).
//  shrouded            -> 0 (never explored)
//  visible (unfogged)  -> live value, also stored in the per-cell cache
// FOG NOTE: CellClass::IsFogged() is a stub on this engine (0x5865E0 = `xor al,al; ret 4`), so the
// fogged branch below is DORMANT: fog-of-war is NOT supported/validated and no fog-on claim is made.
// The branch is kept only so a future verified IsFogged() yields last-seen ore instead of live ore.
namespace
{
    unsigned char g_oreCache[512 * 512];
    int g_oreLastFrame = 0;
}

void Bridge::ResetSeenOre()
{
    memset(g_oreCache, 0, sizeof(g_oreCache));
    g_oreLastFrame = 0;
}

int Bridge::SeenOre(CellClass* c)
{
    if (!c) return 0;
    const int f = Unsorted::CurrentFrame;
    if (f < g_oreLastFrame) Bridge::ResetSeenOre();   // backup: frame regression = new match / load
    g_oreLastFrame = f;
    if (c->IsShrouded()) return 0;
    const bool fogged = c->IsFogged();                // always false on this engine (stub)
    const int x = c->MapCoords.X, y = c->MapCoords.Y;
    const bool inCache = (x >= 0 && x < 512 && y >= 0 && y < 512);
    if (fogged)
        return inCache ? g_oreCache[y * 512 + x] : 0;
    int v = c->GetContainedTiberiumValue();
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    if (inCache) g_oreCache[y * 512 + x] = static_cast<unsigned char>(v);
    return v;
}

bool Bridge::FoundationUnshrouded(BuildingTypeClass* bt, int x, int y)
{
    // Native UI shroud rule (0x4A9070): every cell of the GetFoundationWidth() x GetFoundationHeight(false)
    // RECTANGLE (not just FoundationData: 3x3Refinery omits offset (2,1)) must be on the map and unshrouded.
    if (!bt) return false;
    const int w = bt->GetFoundationWidth(), h = bt->GetFoundationHeight(false);
    if (w <= 0 || h <= 0 || w > 16 || h > 16) return false;
    for (int dy = 0; dy < h; ++dy)
        for (int dx = 0; dx < w; ++dx)
        {
            CellStruct cs;
            cs.X = static_cast<short>(x + dx);
            cs.Y = static_cast<short>(y + dy);
            CellClass* c = MapClass::Instance.TryGetCellAt(cs);
            if (!c || c->IsShrouded()) return false;
        }
    return true;
}

// Native anchor predicate (0x4A8EB0): an own BaseNormal building, or -- with GameModeOptions.BuildOffAlly -- a
// building whose owner IsAlliedWith(us) (true for self too) and whose type has EligibileForAllyBuilding.
static bool IsPlaceAnchor(BuildingClass* b, HouseClass* pAgent)
{
    if (!b || b->InLimbo || !b->Type || !b->Owner) return false;
    if (b->Owner == pAgent && b->Type->BaseNormal) return true;
    return GameModeOptionsClass::Instance.BuildOffAlly && b->Type->EligibileForAllyBuilding && b->Owner->IsAlliedWith(pAgent);
}

// Upgrade host eligibility (native Unlimbo 0x440871-0x440951; the UI then overrides proximity, 0x4ABA94): the cell's
// building is an own building of type PowersUpBuilding, anchored exactly at (x,y), with upgrade capacity left.
static bool UpgradeHostOK(BuildingTypeClass* bt, int x, int y, HouseClass* pAgent)
{
    // Mirrors native Unlimbo 0x4408C8-0x44094F / UI helper 0x452670 (disassembled): target = Cell::GetSomeObject
    // (0x4AA290), must be a Building; same owner; stricmp(PowersUpBuilding, host ID) == 0; then by PowersUpToLevel p:
    //   p == -1 : host.UpgradeLevel < host.Type.Upgrades (capacity), else the zero-level fallback UpgradeLevel == 0
    //   1..3    : host.UpgradeLevel == 0 (unupgraded);   anything else: fail.
    // No host-anchor-equals-cell requirement (native has none).
    CellStruct cs; cs.X = static_cast<short>(x); cs.Y = static_cast<short>(y);
    CellClass* c = MapClass::Instance.TryGetCellAt(cs);
    if (!c) return false;
    // Native passes a ZERO offset (two zeroed ints, address passed; callee reads it as a Point2D): offset (0,0), alt=false.
    CoordStruct co; co.X = 0; co.Y = 0; co.Z = 0;
    ObjectClass* o = c->GetSomeObject(co, false);
    if (!o || o->WhatAmI() != AbstractType::Building) return false;
    BuildingClass* host = static_cast<BuildingClass*>(o);
    if (host->InLimbo || !host->Type || host->Owner != pAgent) return false;
    if (lstrcmpiA(host->Type->ID, bt->PowersUpBuilding) != 0) return false;
    const int lvl = static_cast<signed char>(host->UpgradeLevel);
    const int p = bt->PowersUpToLevel;
    if (p == -1)
        return lvl < host->Type->Upgrades || lvl == 0;
    if (p >= 1 && p <= 3)
        return lvl == 0;
    return false;
}

// Human-UI placement preconditions that Building::Unlimbo skips (read-only reimplementation):
//  * shroud over the whole width x height rectangle, unless the type has ToTile (native 0x4A9186 tests ToTile);
//  * PowersUpBuilding types: upgrade-host validation REPLACES proximity (and the caller skips CanPlaceHere);
//  * otherwise proximity (0x4A8EB0), walls included: scan the footprint grown by Adjacent+1 EXCLUDING the proposed
//    footprint cells, and test the actual cell occupants with the anchor predicate.
bool Bridge::PlaceAllowed(BuildingTypeClass* bt, int x, int y, HouseClass* pAgent)
{
    if (!bt || !pAgent) return false;
    if (!bt->ToTile && !Bridge::FoundationUnshrouded(bt, x, y)) return false;
    if (bt->PowersUpBuilding[0] != '\0') return UpgradeHostOK(bt, x, y, pAgent);
    const int reach = bt->Adjacent + 1;
    const int w = bt->GetFoundationWidth(), h = bt->GetFoundationHeight(false);
    if (w <= 0 || h <= 0 || w > 16 || h > 16) return false;
    for (int cy = y - reach; cy < y + h + reach; ++cy)
        for (int cx = x - reach; cx < x + w + reach; ++cx)
        {
            if (cx >= x && cx < x + w && cy >= y && cy < y + h) continue;   // native excludes the whole w x h rectangle
            CellStruct cs; cs.X = static_cast<short>(cx); cs.Y = static_cast<short>(cy);
            CellClass* c = MapClass::Instance.TryGetCellAt(cs);
            if (c && IsPlaceAnchor(c->GetBuilding(), pAgent)) return true;
        }
    return false;
}

void Bridge::ReadScore(HouseClass* pAgent, BridgeScore& s)
{
    memset(&s, 0, sizeof(s));
    if (!pAgent) return;
    // Live evidence (3 games): TotalKilledUnits == sum_h h.KilledUnitsOfHouses[agent] exactly, so
    // TotalKilledUnits is NOT "kills by us"; it equals the column sum. Direction of the 20-slot arrays
    // (HouseClass.h:978-981, "for endgame score screen") is therefore taken as: X.Killed*OfHouses[v]
    // = things of house v that X destroyed. Then row(agent, enemies) = kills by us, column = our losses.
    // TotalKilled* is dropped (redundant). If live data disagrees the row/column slots just swap meaning.
    s.raw_trk_infantry  = SumTracker(pAgent->KilledInfantryTypes);
    s.raw_trk_vehicles  = SumTracker(pAgent->KilledUnitTypes);
    s.raw_trk_aircraft  = SumTracker(pAgent->KilledAircraftTypes);
    s.raw_trk_buildings = SumTracker(pAgent->KilledBuildingTypes);
    s.credits_harvested_raw = Pos(pAgent->HarvestedCredits);
    s.credits_spent         = Pos(pAgent->CreditsSpent);
    s.stolen_credits        = Pos(pAgent->StolenBuildingsCredits);
    if (g_scoreStartBal == INT_MIN) g_scoreStartBal = pAgent->Balance;
    long long inc = static_cast<long long>(pAgent->Balance) - g_scoreStartBal + pAgent->CreditsSpent;
    s.income_derived = inc > 0 ? static_cast<uint32_t>(inc > 0x7FFFFFFF ? 0x7FFFFFFF : inc) : 0u;

    const int me = pAgent->ArrayIndex;
    DynamicVectorClass<HouseClass*>& ha = HouseClass::Array;
    for (int i = 0; i < ha.Count && i < 20; ++i)
    {
        HouseClass* h = ha.Items[i];
        if (!h || h == pAgent) continue;
        if (me >= 0 && me < 20)
        {
            s.lost_units     += Pos(h->KilledUnitsOfHouses[me]);
            s.lost_buildings += Pos(h->KilledBuildingsOfHouses[me]);
        }
        if (h->ArrayIndex >= 0 && h->ArrayIndex < 20 && !pAgent->IsAlliedWith(h))
        {
            s.kills_units_by_us     += Pos(pAgent->KilledUnitsOfHouses[h->ArrayIndex]);
            s.kills_buildings_by_us += Pos(pAgent->KilledBuildingsOfHouses[h->ArrayIndex]);
        }
    }
}
