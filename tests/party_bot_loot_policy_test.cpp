#include "PartyBotLootPolicy.h"
#include <cassert>
#include <limits>
#include <iostream>
int main(){using namespace PartyBotLootPolicy;
 assert(Candidate(true,true,true,true,true,10,20));
 assert(!Candidate(false,true,true,true,true,10,20));assert(!Candidate(true,false,true,true,true,10,20));
 assert(!Candidate(true,true,false,true,true,10,20));assert(!Candidate(true,true,true,true,false,10,20));
 assert(!Candidate(true,true,true,true,true,26,20));assert(!Candidate(true,true,true,true,true,10,41));
 assert(!Candidate(true,true,true,true,true,std::numeric_limits<float>::quiet_NaN(),20));
 assert(TakeMoney(true,100));assert(!TakeMoney(false,100));assert(!TakeMoney(true,0));
 // No capacity input on money: completely full bags still collect native coins.
 assert(!TakeItem(false,true,false,true));assert(TakeItem(true,true,false,true));
 assert(!TakeItem(true,false,false,true));assert(!TakeItem(true,true,true,true));assert(!TakeItem(true,true,false,false));
 std::cout<<"PASS safe corpse eligibility, full-bag money independence, persistent item capacity and loot rights\n";
}
