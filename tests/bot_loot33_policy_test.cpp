#include "BotLoot33Policy.h"
#include <cassert>
#include <iostream>
int main(){using namespace BotLoot33Policy;
 Input i;i.cls=2;i.itemClass=4;i.subclass=1;i.inventory=5;i.itemLevel=71;i.dominantArmor=4;i.usable=i.capacity=i.retained=true;
 assert(Decide(i).vote==Vote::Greed); // Plate paladin cannot Need high-level cloth, regardless of role.
 i.itemLevel=70;assert(Decide(i).vote==Vote::Need); // Historical item-level exception.
 i.itemLevel=88;i.subclass=4;i.intellect=i.spellPower=true;assert(Decide(i).vote==Vote::Need); // Hybrid can Need caster plate.
 i.cls=1;assert(Decide(i).vote==Vote::Greed);i.intellect=i.spellPower=false;assert(Decide(i).vote==Vote::Need);
 i.inventory=16;i.subclass=1;assert(Decide(i).vote==Vote::Need); // Cloaks are not cloth armor restrictions.
 i.inventory=14;i.subclass=6;assert(Decide(i).vote==Vote::Need); // Shield not plate subclass.
 i.cls=3;i.intellect=true;assert(Decide(i).vote==Vote::Need); // Hunters use mana in 1.12.
 i.spellPower=true;assert(Decide(i).vote==Vote::Greed);
 i.cls=8;i.itemClass=2;i.inventory=13;i.physicalStats=true;i.intellect=i.spellPower=false;assert(Decide(i).vote==Vote::Greed);
 i.spellPower=true;assert(Decide(i).vote==Vote::Need); // Mixed caster weapon stays eligible.
 for(unsigned cls:{1u,2u,3u,4u,5u,7u,8u,9u,11u}){
  i={};i.cls=cls;i.itemClass=4;i.subclass=i.dominantArmor=DominantArmor(cls);i.inventory=1;i.itemLevel=80;i.usable=i.retained=i.capacity=true;assert(Decide(i).vote==Vote::Need);
  i.usable=false;assert(Decide(i).vote==Vote::Greed);
  i.usable=true;i.capacity=false;assert(Decide(i).vote==Vote::Pass);
  i.capacity=true;i.retained=false;assert(Decide(i).vote==Vote::Pass);
 }
 i={};i.cls=4;i.itemClass=2;i.inventory=13;i.usable=i.retained=i.capacity=true;i.spellPower=true;assert(Decide(i).vote==Vote::Greed);
 std::cout<<"PASS bot33 nine classes, ilvl70/71 armor boundary, cloak/shield, hybrid/caster stats, hunter intellect and capacity/retention\n";
}
