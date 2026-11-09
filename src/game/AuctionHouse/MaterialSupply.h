#ifndef MANGOS_MATERIAL_SUPPLY_H
#define MANGOS_MATERIAL_SUPPLY_H
namespace MaterialSupply
{
// World-thread only, called by the existing auction maintenance timer.
void Update();
}
#endif
