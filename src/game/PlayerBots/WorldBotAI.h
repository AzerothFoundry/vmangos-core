#ifndef MANGOS_WORLD_BOT_AI_H
#define MANGOS_WORLD_BOT_AI_H
#include "PartyBotAI.h"
#include "WorldBotRouteRecovery.h"
#include "WorldBotPKTaskState.h"
#include "WorldPvpFlavorPolicy.h"
#include "BattleBotPolicyRuntime.h"
#include <functional>
#include <array>
#include <atomic>
#include <mutex>
#include <deque>
struct WorldBotProfile {
 std::string purpose="grinding";
 std::string id; uint32 characterGuid=0,accountId=0; uint64 createdAt=0; uint8 seedLevel=1; uint32 map=0;
 float x=0,y=0,z=0,radius=120; bool initialize=false,allowLevel60=false; uint32 specId=0,gearId=0,ammoItem=0; bool presetInitialize=false;
 std::function<void()> initializedCallback,presetInitializedCallback;
};
struct WorldBotNavDiagnostic {int32 type=-1;uint32 at=0,map=0,points=0;std::array<float,3> start{},goal{},end{};};
struct WorldBotServiceObservation {uint32 at=0,entry=0,flags=0,trainerType=0,trainerClass=0,faction=0,affordable=0;float distance=0;bool alive=false,friendly=false,los=false,trainerCompatible=false;};
class WorldBotAI : public PartyBotAI {
 friend class WorldBotTowerTask;
public:
 WorldBotAI() : PartyBotAI() {}
 ~WorldBotAI() override;
 void Configure(WorldBotProfile const& profile){m_profile=profile;}
 WorldBotProfile const& GetProfile() const{return m_profile;}
 void SetDestination(uint32 map,float x,float y,float z,float radius){if(map!=m_profile.map||WorldBotRouteRecovery::Changed({{x,y,z}},{{m_profile.x,m_profile.y,m_profile.z}},1))m_routeStall.Clear();m_profile.map=map;m_profile.x=x;m_profile.y=y;m_profile.z=z;m_profile.radius=radius;}
 bool AllowsPersistentSave() const override {return true;}
 bool OnSessionLoaded(PlayerBotEntry*,WorldSession*) override;
 void OnPlayerLogin() override;
 void OnPacketReceived(WorldPacket const*) override;
 void UpdateAI(uint32 diff) override;
 void UpdateWorld(uint32 diff);
 void MovementInform(uint32 type,uint32 data) override;
 Player* GetPartyLeader() const override;
 void AddHunterAmmo() override;
 bool ExternalPvpScope() const;
 bool AllowLegacySpell(SpellEntry const* spell) const override;
 std::string WorldPvpFlavorReport() const;
 std::string PolicyReport() const {return BattleBotPolicyRuntime::ExplainExternal(m_pvpPolicy);}
 bool PresetVerified() const;
 std::vector<std::string> EquipmentReport() const;
 bool PKTaskEnabled() const;
 bool PKTaskCanChallenge() const;
 bool PKTaskReady();
 std::string PKTaskReport() const;
 uint32 PKFoodItem() const;
 uint32 PKWaterItem() const;
 bool PrepareDuel();
 bool NativePracticeBuff();bool PracticeCombatAction();
 void StopRationAuras();bool HasRationAura() const;
 uint32 TalentPointsReport() const;
 bool RecoverWithRations();
 bool NeedsPracticeSupplies() const;
 bool ApplyIdlePreset(WorldBotProfile const& desired);
 bool PresetMigrationPending() const{return m_profile.presetInitialize;}
 bool NeedsService() const{return m_needsVendor||m_needsTrainer||NeedsPracticeSupplies();}
 bool NeedsVendor() const{return m_needsVendor;}
 bool NeedsTrainer() const{return m_needsTrainer;}
 void SetServiceDestination(uint32 map,float x,float y,float z,uint32 entry=0){if(!m_hasService||entry!=m_serviceEntry||map!=m_serviceMap||WorldBotRouteRecovery::Changed({{x,y,z}},{{m_serviceX,m_serviceY,m_serviceZ}},1))m_routeStall.Clear();m_serviceEntry=entry;m_serviceMap=map;m_serviceX=x;m_serviceY=y;m_serviceZ=z;m_hasService=true;}
 bool HasServiceGoal() const{return m_hasService;}
 uint32 ServiceEntry() const{return m_serviceEntry;}
 uint32 ServiceMap() const{return m_serviceMap;}
 std::array<float,3> ServiceGoal() const{return {{m_serviceX,m_serviceY,m_serviceZ}};}
 WorldBotRouteRecovery::Stall const& RouteStall() const{return m_routeStall;}
 std::string const& RouteRecoveryReason() const{return m_routeRecoveryReason;}
 bool RouteHearthPending() const{return m_routeHearthPending;}
 uint32 RouteHearthCastTime() const{return m_routeHearthCastMs;}
 WorldBotNavDiagnostic const& NavDiagnostic() const{return m_navDiagnostic;}
 WorldBotServiceObservation const& ServiceObservation() const{return m_serviceObservation;}
 void ClearServiceDestination(){if(m_hasService)m_routeStall.Clear();m_hasService=false;}
 std::string const& GetActivity() const{return m_activity;}
private:
 bool UpdatePKTask(uint32 diff);
 void SavePKTask();
 void PKTaskEvent(char const* event,uint32 target=0);
 WorldBotPKTaskState m_pkTask;
 bool MaybeStartWorldPvp();bool MaintainWorldPvp(uint32 diff);void CancelWorldPvpOwnedActions();
 WorldPvpFlavor::Clock m_worldPvpClock;
 ObjectGuid m_voluntaryPvpTarget;uint32 m_voluntaryPvpAt=0,m_voluntaryPvpMap=0,m_worldPvpMaintenance=0;
 float m_voluntaryPvpX=0,m_voluntaryPvpY=0,m_voluntaryPvpZ=0;bool m_voluntaryPvpReturning=false;
 Item* StackHearth();
 void CompleteWorldLogin();
 void HandleWorldPacket(WorldPacket const*);
 std::atomic<bool> m_worldLoginPending{false};
 std::mutex m_worldPacketMutex;std::deque<std::unique_ptr<WorldPacket>> m_worldPackets;
 bool RecoverPreset();
 bool TryAmbientHeal();bool TrySpiritRecovery();
 uint32 m_practiceActionAt=0,m_practiceBuffAt=0;std::set<uint32> m_practiceBuffTried;
 uint32 m_rationUseAt=0;
 uint32 m_helpAt=0,m_deadAt=0,m_spiritAt=0,m_duelPrepAt=0,m_duelQuiet=0;ObjectGuid m_duelPrepArbiter;bool m_duelPrepDone=false;
 bool TryRouteHearth();
 WorldBotRouteRecovery::Stall m_routeStall;bool m_routeHearthPending=false;uint32 m_routeHearthCastMs=0;
 std::string m_routeRecoveryReason="idle";
 bool Navigate(float x,float y,float z);
 bool Bootstrap();  void RefreshGrowth(); bool ServiceNearby(); bool LootNearby();
 mutable BattleBotPolicyRuntime::State m_pvpPolicy;
 bool m_externalScope=false,m_externalDuel=false;uint32 m_fastElapsed=0;
 uint32 m_navRetryAt=0,m_navProgressAt=0;float m_navProgressX=0,m_navProgressY=0,m_navProgressZ=0;
 bool m_navGoalValid=false;float m_navX=0,m_navY=0,m_navZ=0;
 WorldBotNavDiagnostic m_navDiagnostic;WorldBotServiceObservation m_serviceObservation;uint32 m_serviceEntry=0;
 bool m_needsVendor=false,m_needsTrainer=true,m_hasService=false;uint32 m_serviceMap=0;float m_serviceX=0,m_serviceY=0,m_serviceZ=0;
 WorldBotProfile m_profile; uint32 m_tick=0,m_save=0,m_service=0,m_roam=0,m_patrolDelay=8000;uint8 m_knownLevel=0;
 std::string m_activity="loading";ObjectGuid m_looting;
};
#endif
