#ifndef UNIT_AURA_VISIBILITY_POLICY_H
#define UNIT_AURA_VISIBILITY_POLICY_H
#include <cstdint>
namespace UnitAuraVisibilityPolicy {
constexpr std::uint32_t AurasVisible=0x08000000u;
constexpr std::uint32_t NotSelectable=0x02000000u;
inline std::uint32_t ForObserver(std::uint32_t actual,bool gm,bool showEnemyBuffs){
 if(gm||showEnemyBuffs)actual|=AurasVisible;
 if(gm)actual&=~NotSelectable;
 return actual;
}
}
#endif
