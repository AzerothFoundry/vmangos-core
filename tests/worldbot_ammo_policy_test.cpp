#include "WorldBotAmmoPolicy.h"
#include <cassert>
#include <iostream>
using namespace WorldBotAmmoPolicy;
int main(){
 std::vector<Candidate> arrows{{19316,2,51,55,200,true},{12654,2,54,60,200,true},{2512,2,1,1,200,true},{2516,3,1,1,200,true}};
 assert(Select(true,0,2,60,arrows)==19316); // Fixed kit must not silently upgrade to Doomshot.
 assert(Select(true,2512,18,60,arrows)==2512);
 assert(Select(true,2516,2,60,arrows)==0); // Bullet on bow is rejected.
 assert(Select(false,0,2,60,arrows)==12654);
 assert(Select(false,0,2,1,arrows)==2512);
 assert(Select(false,0,3,60,arrows)==2516);
 assert(Select(false,0,0,60,arrows)==0);
 auto forbidden=arrows;for(auto& c:forbidden)if(c.id==12654)c.usable=false;
 assert(Select(false,0,2,60,forbidden)==19316);
 std::vector<Candidate> tie{{101,2,1,5,200,true},{100,2,1,5,200,true}};
 assert(Select(false,0,2,1,tie)==100);
 assert(Deficit(0,200)==200);assert(Deficit(66,200)==134);assert(Deficit(200,200)==0);assert(Deficit(300,200)==0);
 assert(Compatible(2,2)&&Compatible(18,2)&&Compatible(3,3));assert(!Compatible(3,2)&&!Compatible(2,3));
 std::cout<<"PASS production World ammo planning: fixed/default IDs, native rejection, subtype/level, highest legal ammo, stable tie and refill deficit\n";
}
