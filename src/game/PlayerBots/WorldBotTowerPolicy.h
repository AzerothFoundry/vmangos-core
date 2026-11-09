#pragma once
#include <cmath>
namespace WorldBotTowerPolicy {
enum class Action { KeepCapture,SelectCapture,KeepGuard,SelectGuardOrPatrol };
inline Action Decide(bool objective,bool objectiveFull,bool allFull,bool expired){
 if(!allFull)return !objective||objectiveFull?Action::SelectCapture:Action::KeepCapture;
 return !objective||expired?Action::SelectGuardOrPatrol:Action::KeepGuard;
}
inline bool UsefulStage(float progress,float before,float remaining){return std::isfinite(progress)&&std::isfinite(before)&&std::isfinite(remaining)&&progress>=50.f&&before-remaining>=50.f;}
inline bool WithinSphere(float dx,float dy,float dz,float radius){return radius>0&&std::isfinite(dx)&&std::isfinite(dy)&&std::isfinite(dz)&&std::sqrt(dx*dx+dy*dy+dz*dz)<=radius;}
inline bool Arrived(float distance3d){return distance3d<=1.f;}
inline bool GatherWait(bool ready,unsigned elapsed){return !ready&&elapsed<45000;}
inline bool BarrierReady(bool anyAlive,bool allReady,unsigned online){return anyAlive||(allReady&&online==5);}
inline bool Full(float value,float maximum,bool alliance){return maximum>0&&(alliance?value>=maximum-.01f:value<=-maximum+.01f);}
inline bool Reclaim(bool inside,bool delay,bool allReady,unsigned elapsed){return inside&&delay&&(allReady||elapsed>=90000);}
inline bool Engage(bool alive,bool legal,bool visible,bool los,float distance){return alive&&legal&&visible&&los&&distance<=45;}
inline float ObjectiveCost(float path,unsigned reservations,unsigned tie){return path+reservations*600.f+tie%17;}
}
