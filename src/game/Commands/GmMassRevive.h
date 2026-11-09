#pragma once
#include "Player.h"
#include "Group.h"
#include "WorldSession.h"
#include "ObjectAccessor.h"
#include "PlayerBotAI.h"
#include "Packets/Spell.h"
#include <vector>
namespace GmMassRevive {
inline bool Eligible(bool online,bool alive,bool sameGroup,bool sameContext,bool teleport,bool external,bool friendly,bool pending){return online&&!alive&&sameGroup&&sameContext&&!teleport&&!external&&friendly&&!pending;}
struct Counts{unsigned eligible=0,bots=0,players=0,skipped=0,revived=0,offered=0;};
inline bool Candidate(Player* gm,Player* target){if(!target||!target->IsInWorld()||target->IsBeingTeleported())return false;return Eligible(target->IsInWorld(),target->IsAlive(),target->GetGroup()==gm->GetGroup(),target->GetMap()==gm->GetMap()&&target->GetMapId()==gm->GetMapId()&&target->GetInstanceId()==gm->GetInstanceId(),target->IsBeingTeleported(),target->InBattleGround()||target->GetTransport()||target->IsTaxiFlying()||target->m_duel,target->GetTeam()==gm->GetTeam(),target->IsRessurectRequested());}
inline Counts Run(Player* gm,bool confirm){Counts counts;
 // Snapshot GUIDs: native resurrection/pet operations must not invalidate the
 // group-reference iteration or accidentally include a newly joined character.
 std::vector<ObjectGuid> members;for(auto const& slot:gm->GetGroup()->GetMemberSlots())members.push_back(slot.guid);
 for(auto guid:members){Player* target=ObjectAccessor::FindPlayer(guid);if(!Candidate(gm,target)){++counts.skipped;continue;}
  ++counts.eligible;if(target->IsBot())++counts.bots;else ++counts.players;if(!confirm)continue;
  if(target->IsBot()){
   target->ResurrectPlayer(1.0f);target->SetPower(POWER_MANA,target->GetMaxPower(POWER_MANA));target->SpawnCorpseBones();target->AutoReSummonPet();
   auto* bot=target->AI()?dynamic_cast<PlayerBotAI*>(target->AI()):nullptr;
   if(bot&&bot->AllowsPersistentSave())target->SaveToDB();if(target->IsAlive())++counts.revived;
  }else if(target->GetSession()){
   target->SetResurrectRequestData(gm->GetObjectGuid(),target->GetMapId(),target->GetInstanceId(),target->GetPositionX(),target->GetPositionY(),target->GetPositionZ(),target->GetOrientation(),std::max(1u,target->GetMaxHealth()),target->GetMaxPower(POWER_MANA));
   auto packet=std::make_unique<WorldPackets::Spell::ResurrectRequest>();packet->casterGuid=gm->GetObjectGuid();packet->sickness=false;packet->delayed=false;target->GetSession()->SendPacket(std::move(packet));++counts.offered;
  }
 }
 return counts;
}
}
