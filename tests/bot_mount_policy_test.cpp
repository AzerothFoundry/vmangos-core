#include "BotMountPolicy.h"
#include "BattleBotTactics.h"
#include <cassert>
#include <set>
#include <iostream>
int main(){
 using namespace BotMountPolicy;
 for(unsigned race=1;race<=8;++race){
  assert(Select(42,race,1,39)==0);
  for(unsigned level:{40u,59u,60u,70u}){
   std::set<unsigned> colours;
   for(unsigned guid=1;guid<=512;++guid){
    auto s=Select(guid,race,1,level);assert(s&&Allows(s,race,1,level));
    assert(s!=5784&&s!=23161&&s!=13819&&s!=23214);
    assert(s==Select(guid,race,1,level));colours.insert(s);
    assert(Select(guid,race,9,level)==(level>=60?23161u:5784u));
    assert(Select(guid,race,2,level)==(level>=60?23214u:13819u));
   }
   assert(colours.size()==Options(race,1,level).size());
  }
 }
 assert(Select(1,0,1,60)==0&&Select(1,9,1,60)==0);
 // A follower gets its own mount, even when following a warlock on a dreadsteed.
 assert(!Allows(23161,2,1,60));assert(!Allows(23161,8,7,60));
 assert(Allows(23250,2,1,60));assert(Allows(23241,8,7,60));
 // Long vendor trips justify cast time; small standing/approach moves do not.
 assert(!BattleBotTactics::MountSavesTime(3,7,14,3));
 assert(!BattleBotTactics::MountSavesTime(20,7,14,3));
 assert(BattleBotTactics::MountSavesTime(300,7,14,3));
 assert(!BattleBotTactics::MountSavesTime(300,7,7,3));
 std::cout<<"PASS mount families, class exclusivity, stable colour variety, travel-time policy\n";
}
