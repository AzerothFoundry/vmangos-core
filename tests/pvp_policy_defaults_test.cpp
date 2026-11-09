// Compile against the production PvPCombatPolicy.cpp; this tests decisions, not native casts.
#include "PvPCombatPolicy.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>
using namespace PvPCombatPolicy;
static long score(Snapshot const& p, Context const& c, std::string const& id) {
    for (auto const& x : Rank(p,c)) if(x.rule->id==id)return x.score;
    return -100000;
}
int main(int argc,char** argv) {
    assert(argc==2); std::ifstream f(argv[1]);std::stringstream in;in<<f.rdbuf();
    Snapshot p; auto parsed=Parse(in.str(),p);assert(parsed.ok);assert(p.rules.size()==88);
    Context c;
    c.numbers={{"self.class",9},{"self.role",3},{"self.health_pct",100},
      {"self.mana_pct",100},{"target.health_pct",100},{"target.distance",25},{"target.class",8}};
    c.flags={{"target.player",true},{"target.casting",false},{"target.controlled",false},{"self.moving",false}};
    c.sets["target.own_aura"]={}; c.sets["target.aura"]={};
    assert(score(p,c,"warlock-corruption")>score(p,c,"warlock-agony"));
    c.numbers["target.class"]=5;
    assert(score(p,c,"warlock-agony")>score(p,c,"warlock-corruption"));
    c.sets["target.own_aura"]={1714};
    assert(score(p,c,"warlock-agony")==-100000); // Do not overwrite own functional curse.
    c.sets["target.own_aura"].clear();
    c.flags["target.casting"]=true;c.flags["target.healing"]=true;
    assert(score(p,c,"warlock-tongues")>=700);
    c.numbers["self.class"]=1;c.numbers["target.distance"]=15;
    auto before=score(p,c,"warrior-intercept");c.recentTargetSpellsMs[1953]=500;
    assert(score(p,c,"warrior-intercept")==before+250);
    c.recentTargetSpellsMs[1953]=6001;assert(score(p,c,"warrior-intercept")==before);
    c.numbers["target.distance"]=5;assert(score(p,c,"warrior-intercept")==-100000);
    c.numbers["self.class"]=4;c.numbers["self.combo_points"]=0;
    assert(score(p,c,"rogue-kidney-shot")==-100000);
    c.numbers["self.combo_points"]=5;assert(score(p,c,"rogue-kidney-shot")>-100000);
    c.numbers["self.class"]=8;c.numbers["self.health_pct"]=10;c.flags["self.combat"]=true;
    c.flags["self.carrying_flag"]=true;assert(score(p,c,"mage-ice-block")==-100000);
    c.flags["self.carrying_flag"]=false;assert(score(p,c,"mage-ice-block")>-100000);
    c.numbers["self.class"]=5;c.numbers["self.role"]=4;c.numbers["ally.health_pct"]=60;
    c.sets["ally.own_aura"].clear();
    assert(score(p,c,"priest-renew")>-100000);
    c.sets["ally.own_aura"]={139};assert(score(p,c,"priest-renew")==-100000);
    c.numbers["self.class"]=11;c.sets["ally.own_aura"]={774};
    assert(score(p,c,"druid-rejuvenation")==-100000);
    std::cout<<"PASS defaults: 88 rules, matchup, aura ownership, observed blink expiry, range, combo, flag safety, own HoT preservation\n";
}
