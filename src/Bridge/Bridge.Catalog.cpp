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

#include "Bridge.h"

#include <UnitTypeClass.h>
#include <BuildingTypeClass.h>
#include <InfantryTypeClass.h>
#include <AircraftTypeClass.h>
#include <GeneralDefinitions.h>    // AbstractType

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
