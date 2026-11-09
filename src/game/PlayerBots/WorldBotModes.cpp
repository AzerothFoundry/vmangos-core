#include "WorldBotModes.h"
#include "WorldBotDuelPolicy.h"
#include "WorldBotAI.h"
#include "Player.h"
#include "Group.h"
#include "Map.h"
#include "Pet.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Opcodes.h"
#include "Packets/Duel.h"
#include "Timer.h"
#include "GameObject.h"
#include "PathFinder.h"
#include "MotionMaster.h"
#include "PointMovementGenerator.h"
#include "MoveSpline.h"
#include "Utilities/Random.h"
#include <vector>
#include <set>
#include <map>
#include <cmath>
namespace WorldBotModes {
namespace {constexpr uint32 DUEL_RETURN_POINT=0x57424452;
struct Arena {uint32 map;float x,y,z,radius;uint32 next=0,playerReservedUntil=0;};std::map<uint32,Arena> arenas;
struct Pending {uint32 created=0;bool answered=false;};std::map<uint64,Pending> pending;std::set<uint32> returning;
bool Recovered(Player* p){return p && p->IsAlive() && !p->IsInCombat() && !p->IsBeingTeleported() && !p->IsTaxiFlying() && !p->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL) && !p->HasAura(15007) && (p->GetPowerType()!=POWER_ENERGY||p->GetPowerPercent(POWER_ENERGY)>=95) && p->GetHealthPercent()>=99 && (p->GetMaxPower(POWER_MANA)==0 || p->GetPowerPercent(POWER_MANA)>=95) && (!p->GetPet() || (p->GetPet()->IsAlive() && p->GetPet()->GetHealthPercent()>=95));}
}
// Grinding uses native MovePoint without waypoint callbacks; BG adapter will own BG callbacks.
void MovementInform(PartyBotAI&,uint32,uint32) {}
void SetDuelArena(uint32 guid,uint32 map,float x,float y,float z,float radius){if(radius>=5&&radius<=100&&std::isfinite(x)&&std::isfinite(y)&&std::isfinite(z))arenas[guid]={map,x,y,z,radius,WorldTimer::getMSTime()+30000};}
Mode Detect(Player* p){
 if(!p||!p->IsAlive())return Mode::Dead;
 if(p->m_duel)return Mode::Duel;
 if(p->InBattleGround())return Mode::Battleground;
 if(p->GetMap()&&p->GetMap()->IsDungeon())return Mode::Instance;
 if(Group* g=p->GetGroup())for(auto const& member:g->GetMemberSlots())if(Player* other=ObjectAccessor::FindPlayer(member.guid))if(!other->IsBot())return Mode::Party;
 return p->GetLevel()>=60?Mode::City:Mode::World;
}
char const* Name(Mode m){switch(m){case Mode::World:return "grinding";case Mode::Party:return "party";case Mode::Battleground:return "battleground";case Mode::Instance:return "instance";case Mode::City:return "city";case Mode::Duel:return "duel";default:return "dead";}}
bool Packet(PartyBotAI& ai,WorldPacket const& packet){
 if(!ai.me)return false;
 if(packet.GetOpcode()==SMSG_DUEL_REQUESTED){uint32 now=WorldTimer::getMSTime();pending[ai.me->GetGUID()]={now,false};if(ai.me->m_duel&&ai.me->m_duel->opponent&&!ai.me->m_duel->opponent->IsBot()){auto a=arenas.find(ai.me->GetGUIDLow());if(a!=arenas.end())a->second.playerReservedUntil=now+60000;}return true;}
 if(packet.GetOpcode()==SMSG_DUEL_COMPLETE){returning.erase(ai.me->GetGUIDLow());pending.erase(ai.me->GetGUID());return false;}
 if(packet.GetOpcode()==SMSG_GROUP_INVITE){
  // Native invite creation already checked faction, capacity and group membership.
  // Never abandon a real player's existing group to accept another invitation.
  if(Group* g=ai.me->GetGroup())if(g->GetMembersCount()>1)return true;
  if(ai.me->IsInCombat()||ai.me->InBattleGround()||ai.me->IsBeingTeleported())return true;
  auto data=std::make_unique<NullClientPacket>(CMSG_GROUP_ACCEPT);ai.me->GetSession()->QueuePacket(std::move(data));return true;
 }
 return false;
}
bool Update(WorldBotAI& ai,uint32 /*diff*/){
 if(!ai.me||!ai.me->IsInWorld()||ai.me->IsBeingTeleported())return false;
 if(!ai.me->m_duel)returning.erase(ai.me->GetGUIDLow());
 // Native duel arbiter is authoritative, not a guessed fixed city center.
 // Return before the core 75yd warning; do not teleport or change its 10s timeout.
 if(ai.me->m_duel && ai.me->m_duel->startTime && !ai.me->HasUnitState(UNIT_STATE_NO_FREE_MOVE))
  if(GameObject* flag=ai.me->GetMap()->GetGameObject(ai.me->GetGuidValue(PLAYER_DUEL_ARBITER)))
   {
    // Envelope points use raw center distances; GetDistance subtracts object radii.
    float ox=ai.me->GetPositionX()-flag->GetPositionX(),oy=ai.me->GetPositionY()-flag->GetPositionY(),oz=ai.me->GetPositionZ()-flag->GetPositionZ();
    float originDistance=std::sqrt(ox*ox+oy*oy+oz*oz);
    if(WorldBotDuelPolicy::ReturnToCenter(ai.me->GetDistance(flag),returning.count(ai.me->GetGUIDLow())))returning.insert(ai.me->GetGUIDLow());else returning.erase(ai.me->GetGUIDLow());
    if(returning.count(ai.me->GetGUIDLow())){
    PathInfo route(ai.me);route.ExcludeSteepSlopes();route.calculate(flag->GetPositionX(),flag->GetPositionY(),flag->GetPositionZ());
    bool envelope=true;for(auto const& point:route.getPath()){float dx=point.x-flag->GetPositionX(),dy=point.y-flag->GetPositionY(),dz=point.z-flag->GetPositionZ();if(!WorldBotDuelPolicy::InsideReturnEnvelope(std::sqrt(dx*dx+dy*dy+dz*dz),originDistance))envelope=false;}
    if(route.getPathType()==PATHFIND_NORMAL&&envelope){
     ai.me->AttackStop();
     auto point=dynamic_cast<PointMovementGenerator<Player> const*>(ai.me->GetMotionMaster()->GetCurrent());
     if(!point||point->GetId()!=DUEL_RETURN_POINT||ai.me->movespline->Finalized())
      ai.me->GetMotionMaster()->MovePoint(DUEL_RETURN_POINT,flag->GetPositionX(),flag->GetPositionY(),flag->GetPositionZ(),MOVE_PATHFINDING|MOVE_EXCLUDE_STEEP_SLOPES);
     // Check the launched path, not Catmull control-array padding or stale roam points.
     auto spline=ai.me->movespline;
     bool actual=spline->Initialized();
     auto inside=[&](float x,float y,float z){float dx=x-flag->GetPositionX(),dy=y-flag->GetPositionY(),dz=z-flag->GetPositionZ();return WorldBotDuelPolicy::InsideReturnEnvelope(std::sqrt(dx*dx+dy*dy+dz*dz),originDistance);};
     if(actual){auto position=spline->ComputePosition();actual=inside(position.x,position.y,position.z);for(int32 i=spline->_currentSplineIdx()+1;actual&&i<=int32(spline->CountSplinePoints());++i){auto next=spline->GetPoint(i);actual=inside(next.x,next.y,next.z);}}
     if(!actual){ai.me->StopMoving();ai.me->GetMotionMaster()->Clear();ai.me->GetMotionMaster()->MoveIdle();}
     return true;
    }
    ai.me->AttackStop();ai.me->StopMoving();ai.me->GetMotionMaster()->Clear();ai.me->GetMotionMaster()->MoveIdle();return true;
    }
   }
 auto it=pending.find(ai.me->GetGUID());
 if(it==pending.end()){
  if(ai.GetProfile().purpose=="city_duel")return false;
  if(ai.NeedsService())return false;
  auto arena=arenas.find(ai.me->GetGUIDLow());if(arena==arenas.end()||Detect(ai.me)!=Mode::City||!Recovered(ai.me))return false;
  auto& a=arena->second;uint32 now=WorldTimer::getMSTime();if((a.playerReservedUntil&&int32(now-a.playerReservedUntil)<0)||int32(now-a.next)<0)return false;a.next=now+30000;
  if(ai.me->GetMapId()!=a.map||!ai.me->IsWithinDist3d(a.x,a.y,a.z,a.radius))return false;
  std::vector<Player*> choices;
  for(auto const& candidate:arenas){if(candidate.first<=ai.me->GetGUIDLow()||candidate.second.map!=a.map||(candidate.second.playerReservedUntil&&int32(now-candidate.second.playerReservedUntil)<0))continue;
   Player* other=ObjectAccessor::FindPlayer(ObjectGuid(HIGHGUID_PLAYER,candidate.first));
   if(!other||!other->IsBot()||other->GetLevel()!=60||Detect(other)!=Mode::City||!Recovered(other)||!ai.me->IsWithinDistInMap(other,20)||!other->IsWithinDist3d(a.x,a.y,a.z,a.radius))continue;
   if(other->GetTeam()==ai.me->GetTeam())choices.push_back(other);
  }
  if(!choices.empty()){ai.me->CastSpell(choices[urand(0,choices.size()-1)],7266,false);return true;}
  return false;
 }
 if(!ai.me->m_duel){pending.erase(it);return false;}
 if(it->second.answered)return false;
 if(uint32(WorldTimer::getMSTime()-it->second.created)>45000 || ai.me->InBattleGround() || (ai.me->GetMap()&&ai.me->GetMap()->IsDungeon())){
  auto data=std::make_unique<WorldPackets::Duel::DuelCancelled>();data->playerGuid=ai.me->GetObjectGuid();ai.me->GetSession()->QueuePacket(std::move(data));pending.erase(it);return true;
 }
 if(ai.me->m_duel->startTimer)return true;
 bool ready=ai.PrepareDuel();
 Player* opponent=ai.me->m_duel->opponent;
 bool opponentReady=opponent&&opponent->IsAlive()&&opponent->IsInWorld()&&opponent->GetMap()==ai.me->GetMap()&&!opponent->IsBeingTeleported()&&!opponent->IsTaxiFlying()&&!opponent->IsInCombat();
 if(opponentReady&&opponent->IsBot())opponentReady=Recovered(opponent);
 if(ai.PKTaskEnabled()&&!ai.PKTaskReady())ready=false;
 if(ready&&Recovered(ai.me)&&opponentReady){
  auto data=std::make_unique<WorldPackets::Duel::DuelAccepted>();data->playerGuid=ai.me->GetObjectGuid();ai.me->GetSession()->QueuePacket(std::move(data));it->second.answered=true;return true;
 }
 // Native owned consumables recover the bot; a human challenger chooses their own resources.
 return true;
}
}
