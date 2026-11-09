#include "WorldBotAmbientPolicy.h"
#include <cassert>
#include <limits>
#include <iostream>
int main(){using namespace WorldBotAmbient;Patient p;p.health=30;assert(MayHelp(p));
 bool Patient::* gates[]={&Patient::combat,&Patient::duel,&Patient::ffa,&Patient::resting,&Patient::casting};
 for(auto field:gates){Patient a=p;a.*field=true;assert(!MayHelp(a));}
 for(auto field:{&Patient::alive,&Patient::inWorld,&Patient::sameMap,&Patient::friendly}){Patient a=p;a.*field=false;assert(!MayHelp(a));}
 p.health=70;assert(!MayHelp(p));p.health=std::numeric_limits<float>::quiet_NaN();assert(!MayHelp(p));
 assert(StageProgress(5,2));assert(!StageProgress(4.9f,20));assert(!StageProgress(20,0));assert(!StageProgress(NAN,20));
 assert(DuelBuffBudget(7999,0));assert(!DuelBuffBudget(8000,0));
 std::cout<<"PASS production ambient eligibility, no duel/FFA/combat/rest/cast interference, stage progress and preparation budget\n";
}
