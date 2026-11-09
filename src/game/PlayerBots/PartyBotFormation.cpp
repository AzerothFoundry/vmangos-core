#include "PartyBotFormation.h"
#include "PartyBotFormationPolicy.h"
#include "PartyBotAI.h"
#include "RaidCoordination.h"
#include "BotDamagePolicy.h"
#include "Player.h"
#include "Creature.h"
#include "Pet.h"
#include "Map.h"
#include "MotionMaster.h"
#include "PointMovementGenerator.h"
#include "PathFinder.h"
#include "SpellMgr.h"
#include "Spell.h"
#include "ObjectMgr.h"
#include "Timer.h"
#include "Log.h"
#include <mutex>
#include <map>
#include <algorithm>
namespace PartyBotFormation {namespace {
constexpr unsigned MotionId=0x5042464d;
struct State {uint32 at=0,target=0,checked=0,map=0,instance=0;float x=0,y=0,z=0;bool moving=false,hold=false;};
std::recursive_mutex mutex;std::map<uint64,State> states;
bool Own(Player* p){auto* point=dynamic_cast<PointMovementGenerator<Player> const*>(p->GetMotionMaster()->GetCurrent());return point&&point->GetId()==MotionId;}
bool GroupVictim(Player* p,Unit* enemy){auto* v=enemy?enemy->GetVictim():nullptr;auto* owner=v?v->GetCharmerOrOwnerPlayerOrPlayerItself():nullptr;return owner&&owner->GetGroup()==p->GetGroup();}
float Range(PartyBotAI& ai,bool healing,float& minimum){Player* p=ai.me;float maximum=0;minimum=0;
 SpellEntry const* core=nullptr;
 if(!healing)switch(p->GetClass()){
  case CLASS_WARLOCK:core=ai.m_spells.warlock.pShadowBolt;break;
  case CLASS_MAGE:core=ai.m_spells.mage.pFrostbolt?ai.m_spells.mage.pFrostbolt:ai.m_spells.mage.pFireball;break;
  case CLASS_PRIEST:core=ai.m_spells.priest.pMindFlay?ai.m_spells.priest.pMindFlay:ai.m_spells.priest.pMindBlast;break;
  case CLASS_SHAMAN:core=ai.m_spells.shaman.pLightningBolt;break;
  case CLASS_DRUID:core=ai.m_spells.druid.pStarfire?ai.m_spells.druid.pStarfire:ai.m_spells.druid.pWrath;break;
  case CLASS_HUNTER:core=ai.m_spells.hunter.pAimedShot?ai.m_spells.hunter.pAimedShot:ai.m_spells.hunter.pArcaneShot;break;
 }
 if(core){auto* range=sSpellRangeStore.LookupEntry(core->rangeIndex);if(range){maximum=Spells::GetSpellMaxRange(range);minimum=Spells::GetSpellMinRange(range);p->ApplySpellMod(core->Id,SPELLMOD_RANGE,maximum,nullptr,false);if(maximum>=15&&maximum<=60)return maximum;maximum=0;}}

 for(auto const& known:p->GetSpellMap())if(known.second.state!=PLAYERSPELL_REMOVED&&!known.second.disabled){auto* spell=sSpellMgr.GetSpellEntry(known.first);if(!spell||spell->IsPassiveSpell()||spell->IsAreaOfEffectSpell()||spell->HasAreaAuraEffect())continue;
  if(healing?!spell->IsHealSpell():!BotDamagePolicy::IsDamage(spell))continue;
  auto* range=sSpellRangeStore.LookupEntry(spell->rangeIndex);if(!range)continue;float max=Spells::GetSpellMaxRange(range),min=Spells::GetSpellMinRange(range);
  p->ApplySpellMod(spell->Id,SPELLMOD_RANGE,max,nullptr,false);if(max<15||max>60)continue;
  // Preserve the longest native offensive/healing cast range; shorter
  // situational spells do not force a caster into their range.
  if(max>maximum){maximum=max;minimum=min;}
 }
 return maximum;
}
bool Validate(PartyBotAI& ai,Unit* anchor,float x,float y,float& z){Player* p=ai.me;
 z=p->GetMap()->GetHeight(x,y,anchor->GetPositionZ()+6,true,12);if(!std::isfinite(z)||std::fabs(z-anchor->GetPositionZ())>8)return false;
 if(!p->GetMap()->isInLineOfSight(x,y,z+1.5f,anchor->GetPositionX(),anchor->GetPositionY(),anchor->GetPositionZ()+1.5f,true,false))return false;
 PathInfo path(p);path.ExcludeSteepSlopes();if(!path.calculate(x,y,z)||path.getPath().empty())return false;auto end=path.getPath().back();float dx=end.x-x,dy=end.y-y,dz=end.z-z;
 // Inspect the entire native route against nearby idle NPC aggro radii.
 std::list<Unit*> nearby;p->GetEnemyListInRadiusAround(p,90,nearby);bool idle=false;
 for(Unit* enemy:nearby)if(enemy&&enemy->GetTypeId()==TYPEID_UNIT&&enemy->IsAlive()&&!enemy->IsInCombat()&&!enemy->GetCharmerOrOwnerPlayerOrPlayerItself()&&p->IsValidAttackTarget(enemy)&&enemy->IsVisibleForOrDetect(p,p,false)){
  auto* creature=static_cast<Creature*>(enemy);float aggro=creature->GetAttackDistance(p)+3;
  for(unsigned i=1;i<path.getPath().size();++i){auto a=path.getPath()[i-1],b=path.getPath()[i];if(PartyBotFormationPolicy::SegmentDistance(enemy->GetPositionX(),enemy->GetPositionY(),enemy->GetPositionZ(),a.x,a.y,a.z,b.x,b.y,b.z)<aggro+enemy->GetObjectBoundingRadius()){idle=true;break;}}
  if(idle)break;
 }
 return PartyBotFormationPolicy::SafePath(path.getPathType(),path.Length(),std::sqrt(dx*dx+dy*dy+dz*dz),idle,true);
}
}
bool Update(PartyBotAI& ai){Player* p=ai.me;if(p){std::lock_guard<std::recursive_mutex> lock(mutex);uint32 now=WorldTimer::getMSTime();for(auto it=states.begin();it!=states.end();)if(uint32(now-it->second.checked)>300000)it=states.erase(it);else ++it;states[p->GetGUID()].hold=false;}if(!p||!p->IsInWorld()||p->IsBeingTeleported()||!p->GetMap())return false;
 if(!PartyBotFormationPolicy::Scene(p->GetMap()->IsDungeon(),p->IsAlive(),p->GetGroup()!=nullptr,p->InBattleGround(),p->m_duel!=nullptr,p->IsNonMeleeSpellCasted(),p->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL|UNIT_STATE_CAN_NOT_MOVE),p->GetTransport()!=nullptr||p->IsTaxiFlying()))return false;
 auto motion=p->GetMotionMaster()->GetCurrentMovementGeneratorType();if(motion!=IDLE_MOTION_TYPE&&motion!=FOLLOW_MOTION_TYPE&&motion!=CHASE_MOTION_TYPE&&!Own(p))return false;
 Unit* enemy=p->GetVictim();if(!enemy&&ai.GetRole()==ROLE_HEALER)for(GroupReference* ref=p->GetGroup()->GetFirstMember();ref;ref=ref->next())if(auto member=ref->getSource())if(member->GetVictim()&&member->GetVictim()->GetVictim()==member){enemy=member->GetVictim();break;}
 if(!enemy||!enemy->IsAlive()||enemy->GetTypeId()!=TYPEID_UNIT||enemy->GetCharmerOrOwnerPlayerOrPlayerItself()||!enemy->IsInCombat()||!GroupVictim(p,enemy)||!ai.IsValidHostileTarget(enemy)||enemy->HasBreakableByDamageCrowdControlAura()){if(Own(p)){p->StopMoving();p->GetMotionMaster()->Clear();}return false;}
 for(auto mark:ai.m_marksToCC)if(enemy->GetObjectGuid()==p->GetGroup()->GetTargetWithIcon(mark))return false;
 Unit* anchor=enemy;bool healer=ai.GetRole()==ROLE_HEALER,tank=ai.GetRole()==ROLE_TANK,ranged=ai.GetRole()==ROLE_RANGE_DPS;
 if(tank&&enemy->GetVictim()!=p)return false;
 float minimum=0,radius=0,angle=enemy->GetOrientation();
 if(healer){Player* patient=RaidCoordination::Enabled(p)?RaidCoordination::HealingTank(ai):nullptr;float hp=101;for(GroupReference* ref=p->GetGroup()->GetFirstMember();ref;ref=ref->next())if(Player* member=ref->getSource())if(member->IsInWorld()&&member->IsAlive()&&member->GetMap()==p->GetMap()&&member->GetHealthPercent()<hp&&member->GetHealthPercent()<85){patient=member;hp=member->GetHealthPercent();}if(!patient)patient=enemy->GetVictim()?enemy->GetVictim()->ToPlayer():nullptr;if(!patient)return false;anchor=patient;}
 if(ranged||healer){float maximum=Range(ai,healer,minimum);radius=PartyBotFormationPolicy::CastRadius(minimum,maximum);if(!radius)return false;angle=std::atan2(p->GetPositionY()-anchor->GetPositionY(),p->GetPositionX()-anchor->GetPositionX());}
 else if(tank){float cx=0,cy=0;unsigned count=0;for(GroupReference* ref=p->GetGroup()->GetFirstMember();ref;ref=ref->next())if(Player* member=ref->getSource())if(member!=p&&member->IsInWorld()&&member->IsAlive()&&member->GetMap()==p->GetMap()&&member->GetDistance(enemy)<50){cx+=member->GetPositionX();cy+=member->GetPositionY();++count;}if(!count)return false;angle=std::atan2(enemy->GetPositionY()-cy/count,enemy->GetPositionX()-cx/count);radius=std::max(2.f,enemy->GetCombatReach()+p->GetCombatReach()-.75f);}
 else {angle=PartyBotFormationPolicy::Rear(enemy->GetOrientation(),p->GetGUIDLow());radius=std::max(2.f,enemy->GetCombatReach()+p->GetCombatReach()-.75f);}
 float distance=p->GetCombatDistance(anchor);bool crowded=false;
 if(ranged||healer)for(GroupReference* ref=p->GetGroup()->GetFirstMember();ref;ref=ref->next())if(Player* member=ref->getSource())if(member!=p&&member->IsInWorld()&&member->IsAlive()&&member->GetMap()==p->GetMap()&&p->GetDistance(member)<3){crowded=true;break;}
 bool autoRepeat=p->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL)!=nullptr;
 bool need=(ranged||healer)?PartyBotFormationPolicy::Move(distance<radius-7,distance>radius+1,p->IsWithinLOSInMap(anchor),crowded):std::fabs(std::atan2(std::sin(enemy->GetAngle(p)-angle),std::cos(enemy->GetAngle(p)-angle)))>.5f||!p->CanReachWithMeleeAutoAttack(enemy);
 if(healer&&anchor->GetHealthPercent()<30&&distance<=radius+1&&p->IsWithinLOSInMap(anchor))return false;
 if(autoRepeat&&ranged&&distance>=minimum+1&&distance<=radius+1&&p->IsWithinLOSInMap(anchor))return false;
 std::lock_guard<std::recursive_mutex> lock(mutex);auto& state=states[p->GetGUID()];uint32 now=WorldTimer::getMSTime();
 if(Own(p)&&state.moving&&state.target==anchor->GetGUIDLow()&&uint32(now-state.at)<3000){state.hold=true;state.checked=now;state.map=p->GetMapId();state.instance=p->GetInstanceId();return true;}
 state.hold=!need;state.target=anchor->GetGUIDLow();state.checked=now;state.map=p->GetMapId();state.instance=p->GetInstanceId();
 if(!need){state.x=p->GetPositionX();state.y=p->GetPositionY();state.z=p->GetPositionZ();if(motion==FOLLOW_MOTION_TYPE||motion==CHASE_MOTION_TYPE){p->StopMoving();p->GetMotionMaster()->Clear();}return false;}if(state.at&&uint32(now-state.at)<3000)return false;
 for(unsigned attempt=0;attempt<16;++attempt){float perturb=(int((p->GetGUIDLow()+attempt*7)%17)-8)*.10f;auto point=PartyBotFormationPolicy::Around(anchor->GetPositionX(),anchor->GetPositionY(),angle+perturb,radius);float z=anchor->GetPositionZ();if(tank&&std::hypot(point.x-p->GetPositionX(),point.y-p->GetPositionY())>12.f)continue;if(!Validate(ai,anchor,point.x,point.y,z))continue;
  bool separated=true;if(ranged||healer){
   for(GroupReference* ref=p->GetGroup()->GetFirstMember();ref;ref=ref->next())if(Player* member=ref->getSource())if(member!=p&&member->IsInWorld()&&member->IsAlive()&&member->GetMap()==p->GetMap()){
    auto* other=dynamic_cast<PartyBotAI*>(member->AI());bool relevant=!member->IsBot()||(other&&(other->GetRole()==ROLE_RANGE_DPS||other->GetRole()==ROLE_HEALER));
    if(relevant&&!PartyBotFormationPolicy::Spaced(point.x,point.y,z,member->GetPositionX(),member->GetPositionY(),member->GetPositionZ(),3))separated=false;
   }
   for(auto const& reserved:states)if(reserved.first!=p->GetGUID()&&reserved.second.hold&&reserved.second.map==p->GetMapId()&&reserved.second.instance==p->GetInstanceId()&&uint32(now-reserved.second.checked)<3000&&!PartyBotFormationPolicy::Spaced(point.x,point.y,z,reserved.second.x,reserved.second.y,reserved.second.z,3))separated=false;
  }if(!separated)continue;
  bool coverage=true;if(healer)for(GroupReference* ref=p->GetGroup()->GetFirstMember();ref;ref=ref->next())if(Player* member=ref->getSource())if(member->IsInWorld()&&member->IsAlive()&&member->GetMap()==p->GetMap()&&member->GetHealthPercent()<65&&member->GetDistance(anchor)<35&&member->GetDistance(point.x,point.y,z)>radius+2){coverage=false;break;}if(!coverage)continue;
  if(std::sqrt(p->GetDistanceSqr(point.x,point.y,z))<2)return false;
  state.at=now;state.target=anchor->GetGUIDLow();state.checked=now;state.map=p->GetMapId();state.instance=p->GetInstanceId();state.x=point.x;state.y=point.y;state.z=z;state.moving=true;state.hold=true;if(autoRepeat)p->InterruptSpell(CURRENT_AUTOREPEAT_SPELL,true);p->GetMotionMaster()->MovePoint(MotionId,point.x,point.y,z,MOVE_PATHFINDING|MOVE_EXCLUDE_STEEP_SLOPES);
  sLog.Out(LOG_BASIC,LOG_LVL_DEBUG,"[PartyFormation] actor=%u role=%u target=%u xyz=%.2f,%.2f,%.2f",p->GetGUIDLow(),unsigned(ai.GetRole()),anchor->GetGUIDLow(),point.x,point.y,z);return true;
 }
 state.at=now;return false;
}
bool Holding(PartyBotAI& ai){
 std::lock_guard<std::recursive_mutex> lock(mutex);auto it=states.find(ai.me->GetGUID());
 return it!=states.end()&&it->second.hold&&uint32(WorldTimer::getMSTime()-it->second.checked)<3000&&it->second.map==ai.me->GetMapId()&&it->second.instance==ai.me->GetInstanceId()&&ai.me->GetMap()->IsDungeon()&&!ai.me->InBattleGround()&&!ai.me->m_duel;
}

}
