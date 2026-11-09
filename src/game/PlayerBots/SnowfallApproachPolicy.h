#pragma once
namespace SnowfallApproachPolicy {
// side1 south path runs up forward; side2 north path runs up in reverse.
// Descents remain legal for either team, including leaving a captured graveyard.
inline bool Allowed(unsigned team,unsigned side,bool reverse){
 if(side==1&&!reverse)return team==67;
 if(side==2&&reverse)return team==469;
 return true;
}
}
