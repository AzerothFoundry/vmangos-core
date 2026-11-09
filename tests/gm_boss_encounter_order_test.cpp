#include <cstdint>
using uint32=std::uint32_t;
#include "../src/game/Commands/GmInstanceBossCatalog.h"
#include "../src/game/Commands/GmBossEncounterOrder.h"
#include <cassert>
#include <set>
int main(){using namespace GmBossEncounterOrder;auto mc=Entries(409);unsigned expected[]={12118,11982,12259,12057,12056,12264,12098,11988,12018,11502};assert(mc.size()==10);for(unsigned i=0;i<10;++i)assert(mc[i].id==expected[i]&&mc[i].order==i+1);auto naxx=Entries(533);std::set<unsigned> orders;unsigned four=0;for(auto row:naxx){orders.insert(row.order);if(row.order==9)++four;}assert(orders.size()==15&&four==4);auto aq=Entries(531);unsigned trio=0,twins=0;for(auto row:aq){trio+=row.order==2;twins+=row.order==7;}assert(trio==3&&twins==2);auto sfk=Entries(33);assert(sfk.front().id==3914&&sfk.back().id==14682);auto dm=Entries(36);unsigned sneed=0;for(auto row:dm)if(row.id==642||row.id==643){if(sneed)assert(sneed==row.order);else sneed=row.order;}assert(SameContext(409,7,409,7));assert(!SameContext(409,8,409,7));assert(!SameContext(469,7,409,7));}
