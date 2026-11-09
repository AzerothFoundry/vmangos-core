#pragma once
namespace WarriorTankPolicy {
struct Candidate {bool npc,alive,engaged,held,threatList,legal,controlled,reachable,assigned;};
inline bool Eligible(Candidate const& c){return c.npc&&c.alive&&c.engaged&&c.held&&c.threatList&&c.legal&&!c.controlled&&c.reachable&&c.assigned;}
inline bool MaintainSunder(unsigned stacks,int remaining){return stacks<5||remaining<5000;}

inline bool Rotate(float mine,float competitor) { return competitor>0 ? mine<competitor*1.3f : mine<100; }
inline float Margin(float mine,float competitor) { return competitor>0?mine/competitor:mine/100.0f; }
inline bool ShieldBlock(float health,unsigned attackers,unsigned rage) { return attackers && (health<50 || rage>=300); }
inline bool DumpRage(unsigned rage) { return rage>=500; }
inline bool MassTaunt(unsigned leaks,unsigned elapsed) { return leaks>=3 && elapsed>=8000; }
}
