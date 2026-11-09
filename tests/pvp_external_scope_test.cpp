#include "BattleBotPolicyRuntime.h"
#include <cassert>
#include <iostream>
int main(){using namespace BattleBotPolicyRuntime;
 // World scope accepts an already chosen legally attackable player-controlled
 // entity, independent of human/bot identity; ordinary PvE creatures never qualify.
 assert(ExternalEnemyAllowed(false,false,false,true,true));
 assert(!ExternalEnemyAllowed(false,false,false,false,true));
 assert(!ExternalEnemyAllowed(false,false,false,true,false));
 // Duel countdown/bounds and third-party targets cannot acquire a policy cast.
 assert(!ExternalEnemyAllowed(true,false,true,true,true));
 assert(!ExternalEnemyAllowed(true,true,false,true,true));
 assert(ExternalEnemyAllowed(true,true,true,true,true));
 // A friendly same-group duel opponent must never become a healing recipient.
 assert(!ExternalAllyAllowed(true,false,true,false));
 assert(ExternalAllyAllowed(true,true,true,false));
 assert(ExternalAllyAllowed(false,false,true,false));
 assert(!ExternalAllyAllowed(false,false,true,true));
 assert(!ExternalAllyAllowed(false,false,false,false));
 std::cout<<"PASS production external PvP/duel scope, PvE exclusion and opponent-heal safety gates\n";
}
