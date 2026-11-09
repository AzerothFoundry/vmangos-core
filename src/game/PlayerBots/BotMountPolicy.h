#ifndef BOT_MOUNT_POLICY_H
#define BOT_MOUNT_POLICY_H
#include <cstdint>
#include <vector>
namespace BotMountPolicy {
inline std::vector<std::uint32_t> Options(unsigned race,unsigned cls,unsigned level){
 if(level<40||race<1||race>8)return {};
 if(cls==9)return {level>=60?23161u:5784u};
 if(cls==2)return {level>=60?23214u:13819u};
 if(level>=60)switch(race){
  case 1:return {23227,23228,23229}; // Human horses.
  case 2:return {23250,23251,23252}; // Orc wolves.
  case 3:return {23238,23239,23240}; // Dwarf rams.
  case 4:return {23219,23221,23338}; // Night elf sabers.
  case 5:return {17465,23246};       // Undead skeletal warhorses.
  case 6:return {23247,23248,23249}; // Tauren kodos.
  case 7:return {23222,23223,23225}; // Gnome mechanostriders.
  case 8:return {23241,23242,23243}; // Troll raptors.
 }
 switch(race){
  case 1:return {458,470,472,6648};
  case 2:return {580,6653,6654};
  case 3:return {6777,6898,6899};
  case 4:return {8394,10789,10793};
  case 5:return {17462,17463,17464};
  case 6:return {18989,18990};
  case 7:return {10873,10969,17453,17454};
  case 8:return {8395,10796,10799};
 }
 return {};
}
inline std::uint32_t Select(std::uint64_t identity,unsigned race,unsigned cls,unsigned level){
 auto choices=Options(race,cls,level);if(choices.empty())return 0;
 // Stable per character; don't change colour every AI update or every trip.
 std::uint64_t v=identity^(std::uint64_t(race)<<32)^(std::uint64_t(cls)<<48);
 v+=0x9e3779b97f4a7c15ULL;v=(v^(v>>30))*0xbf58476d1ce4e5b9ULL;v=(v^(v>>27))*0x94d049bb133111ebULL;v^=v>>31;
 return choices[v%choices.size()];
}
inline bool Allows(std::uint32_t spell,unsigned race,unsigned cls,unsigned level){
 for(auto id:Options(race,cls,level))if(id==spell)return true;
 return false;
}
}
#endif
