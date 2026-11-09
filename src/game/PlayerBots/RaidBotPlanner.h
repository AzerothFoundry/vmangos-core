#ifndef RAID_BOT_PLANNER_H
#define RAID_BOT_PLANNER_H
#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace RaidBotPlanner {
struct Spec { uint32_t id; unsigned cls, role; };
struct Profile { uint32_t map; unsigned size,tanks,heals; char const* name; };
inline Profile const* Find(uint32_t map) {
    static Profile const rows[]={{409,40,5,12,"MC"},{469,40,5,12,"BWL"},{533,40,8,14,"Naxx"},{531,40,5,12,"AQ40"},{249,40,2,10,"Onyxia"},{309,20,2,5,"ZG"},{509,20,2,5,"AQ20"},{229,10,2,2,"UBRS"},{33,5,1,1,"SFK"},{34,5,1,1,"Stockade"},{36,5,1,1,"Deadmines"},{43,5,1,1,"WC"},{47,5,1,1,"RFK"},{48,5,1,1,"BFD"},{70,5,1,1,"Uldaman"},{90,5,1,1,"Gnomeregan"},{109,5,1,1,"Sunken Temple"},{129,5,1,1,"RFD"},{189,5,1,1,"SM"},{209,5,1,1,"ZF"},{230,5,1,1,"BRD"},{289,5,1,1,"Scholomance"},{329,5,1,1,"Stratholme"},{349,5,1,1,"Maraudon"},{389,5,1,1,"RFC"},{429,5,1,1,"Dire Maul"}};
    for(auto const& p:rows){if(p.map==map)return &p;}
    return nullptr;
}
struct Plan { std::vector<Spec> add; std::string error; };
inline unsigned Bucket(unsigned role){return role==3?0:role==4?1:2;}
inline Plan Build(Profile const& p,bool horde,std::vector<Spec> const& pool,std::vector<Spec> const& existing,uint32_t seed) {
    Plan out;std::array<unsigned,3> count{};std::array<unsigned,12> classes{};std::array<unsigned,12> healers{};
    std::array<unsigned,3> target{{p.tanks,p.heals,p.size-p.tanks-p.heals}};
    auto record=[&](Spec s){++count[Bucket(s.role)];++classes[s.cls];if(s.role==4)++healers[s.cls];};
    for(auto s:existing){if(s.cls>=12||s.role<1||s.role>4||(!horde&&s.cls==7)||(horde&&s.cls==2)){out.error="Unknown role or unavailable faction class in existing roster.";return out;}record(s);}
    for(unsigned i=0;i<3;++i)if(count[i]>target[i]){out.error="Existing roles exceed this raid profile; no members were removed.";return out;}
    std::mt19937 rng(seed);
    auto pick=[&](unsigned role,unsigned cls)->bool {
        std::vector<Spec> candidates;
        for(auto s:pool)if(s.cls<12&&s.role>=1&&s.role<=4&&count[Bucket(s.role)]<target[Bucket(s.role)]&&(!role||s.role==role)&&(!cls||s.cls==cls)&&!(horde?s.cls==2:s.cls==7))candidates.push_back(s);
        if(candidates.empty())return false;
        std::shuffle(candidates.begin(),candidates.end(),rng);
        // Prefer a warrior for an otherwise unconstrained tank slot; role counts stay exact.
        if(role==3&&!cls)std::stable_sort(candidates.begin(),candidates.end(),[](Spec a,Spec b){return (a.cls==1)>(b.cls==1);});
        else std::stable_sort(candidates.begin(),candidates.end(),[&](Spec a,Spec b){return classes[a.cls]<classes[b.cls];});
        Spec chosen=candidates.front();out.add.push_back(chosen);record(chosen);return true;
    };
    auto needHeals=[&](unsigned cls,unsigned n){while(healers[cls]<n)if(!pick(4,cls))return false;return true;};
    if(p.size==40){
        if(!needHeals(horde?7:2,horde?8:4)||!needHeals(5,2)||!needHeals(11,horde&&p.heals<12?0:2)){out.error="Cannot satisfy healer/buff class minimums with the existing roster.";return out;}
    }else if(p.size==20){
        if(!needHeals(horde?7:2,horde?4:2)||!needHeals(5,1)){out.error="Cannot satisfy 20-player healer minimums.";return out;}
    }
    while(count[0]<target[0])if(!pick(3,0)){out.error="No supported tank preset.";return out;}
    while(count[1]<target[1])if(!pick(4,0)){out.error="No supported healer preset.";return out;}
    if(p.size>=20){
        for(unsigned cls:{3u,4u,5u,8u,9u,11u}){
            unsigned minimum=cls==9&&p.map==409?4:cls==8&&p.size==40?3:(cls==3||cls==11)&&p.size==40?2:1;
            while(classes[cls]<minimum){if(!pick(2,cls)&&!pick(1,cls)){out.error="Cannot satisfy utility class minimums within available DPS slots.";return out;}}
        }
    }
    // A five-player druid healer still needs a priest/paladin/shaman able to cast normal resurrection.
    if(!classes[2]&&!classes[5]&&!classes[7])if(!pick(2,horde?7:5)&&!pick(1,2)){out.error="No normal resurrection provider.";return out;}
    while(count[2]<target[2])if(!pick(1+rng()%2,0)&&!pick(1,0)&&!pick(2,0)){out.error="No supported DPS preset.";return out;}
    return out;
}
// Real player first, then tanks, healers, normal resurrection-capable DPS, other DPS.
inline unsigned ResurrectionPriority(bool bot,unsigned role,unsigned cls){return !bot?0:role==3?1:role==4?2:(cls==2||cls==5||cls==7)?3:4;}
}
#endif
