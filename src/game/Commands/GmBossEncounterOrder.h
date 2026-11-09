#pragma once
#include <vector>
#include <algorithm>
#include <cstdint>
namespace GmBossEncounterOrder {
struct Row{std::uint32_t id,order;};
inline std::vector<Row> Entries(std::uint32_t map){
 std::vector<Row> rows;
 auto add=[&](unsigned order,std::initializer_list<std::uint32_t> ids){for(auto id:ids)rows.push_back({id,order});};
 if(map==409){unsigned i=0;for(auto id:{12118u,11982u,12259u,12057u,12056u,12264u,12098u,11988u,12018u,11502u})add(++i,{id});}
 else if(map==469){unsigned i=0;for(auto id:{12435u,13020u,12017u,11983u,14601u,11981u,14020u,11583u})add(++i,{id});}
 else if(map==533){add(1,{15956});add(2,{15953});add(3,{15952});add(4,{15954});add(5,{15936});add(6,{16011});add(7,{16061});add(8,{16060});add(9,{16064,16065,16062,16063});add(10,{16028});add(11,{15931});add(12,{15932});add(13,{15928});add(14,{15989});add(15,{15990});}
 else if(map==531){add(1,{15263});add(2,{15543,15544,15511});add(3,{15516});add(4,{15510});add(5,{15299});add(6,{15509});add(7,{15275,15276});add(8,{15517});add(9,{15727});}
 else {unsigned order=0;for(auto const& row:GmInstanceBossCatalog)if(row.map==map){if(std::find_if(rows.begin(),rows.end(),[&](Row const& r){return r.id==row.entry;})!=rows.end())continue;
  if(map==36&&row.entry==642){auto existing=std::find_if(rows.begin(),rows.end(),[](Row const& r){return r.id==643;});if(existing!=rows.end()){rows.push_back({row.entry,existing->order});continue;}}
  rows.push_back({row.entry,++order});}}
 return rows;
}
inline bool SameContext(std::uint32_t currentMap,std::uint32_t currentInstance,std::uint32_t expectedMap,std::uint32_t expectedInstance){return currentMap==expectedMap&&currentInstance==expectedInstance;}
}
