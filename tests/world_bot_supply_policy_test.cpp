#include "WorldBotSupplyPolicy.h"
#include <cassert>
#include <iostream>
int main(){using namespace WorldBotSupplyPolicy;
 // Trigger only when required stock is empty, independent of current HP/MP.
 assert(Empty(false,0,20));assert(!Empty(false,1,0));assert(!Empty(false,20,0));
 assert(Empty(true,0,20));assert(Empty(true,20,0));assert(!Empty(true,1,1));
 // Once started, partial purchases cannot clear the errand.
 assert(Active(true,true,20,0,20,20));assert(Active(true,true,20,19,20,20));
 assert(Active(true,true,19,20,20,20));assert(!Active(true,true,20,20,20,20));
 assert(Active(true,false,19,0,20,20));assert(!Active(true,false,20,0,20,20));
 assert(Active(true,true,20,5,20,10));assert(!Active(true,true,20,10,20,10));
 assert(Active(false,true,0,20,20,20));assert(!Active(false,true,19,19,20,20));
 assert(Active(true,true,20,20,0,20));
 std::cout<<"PASS depletion trigger, supply commitment, partial refill and per-item stack sizes\n";
}
