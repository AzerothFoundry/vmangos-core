#include "UnitAuraVisibilityPolicy.h"
#include <cassert>
#include <iostream>
int main(){using namespace UnitAuraVisibilityPolicy;
 constexpr unsigned PvP=0x1000,InvisibleToSelection=NotSelectable;
 const unsigned original=PvP|InvisibleToSelection;
 assert(ForObserver(original,false,false)==original);
 const auto enabled=ForObserver(original,false,true);
 assert(enabled&AurasVisible);assert(enabled&NotSelectable);assert(enabled&PvP);
 assert(original==(PvP|NotSelectable)); // Observer output never changes source flags.
 assert(ForObserver(original,false,false)==original); // OFF serializes removal again.
 assert(ForObserver(original|AurasVisible,false,false)&AurasVisible); // Detect Magic preserved.
 const auto gm=ForObserver(original,true,false);assert(gm&AurasVisible);assert(!(gm&NotSelectable));assert(gm&PvP);
 assert(ForObserver(original,true,true)==gm);
 std::cout<<"PASS on/off, detect magic, GM selection, unrelated flags and source isolation\n";
}
