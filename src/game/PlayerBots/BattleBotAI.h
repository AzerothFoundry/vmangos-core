/*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#ifndef MANGOS_BattleBotAI_H
#define MANGOS_BattleBotAI_H

#include "CombatBotBaseAI.h"
#include "BattleBotWaypoints.h"
#include "BattleBotWaypointPolicy.h"
#include "BattleBotObjectivePolicy.h"
#include "PlayerBotMatch.h"
#include "BattleBotTactics.h"
#include "BattleBotPolicyRuntime.h"

enum BattleBotWsgWaitSpot
{
    BB_WSG_WAIT_SPOT_SPAWN,
    BB_WSG_WAIT_SPOT_LEFT,
    BB_WSG_WAIT_SPOT_RIGHT
};

enum FlagSpellsWS
{
    AURA_WARSONG_FLAG    = 23333,
    AURA_SILVERWING_FLAG = 23335
};

class BattleBotAI : public CombatBotBaseAI
{
public:
    BattleBotAI(uint8 race, uint8 class_, uint8 level, uint32 mapId, uint32 instanceId, float x, float y, float z, float o, uint8 bgId, bool temporary)
        : CombatBotBaseAI(),  m_race(race), m_class(class_), m_level(level), m_mapId(mapId), m_instanceId(instanceId), m_x(x), m_y(y), m_z(z), m_o(o), m_battlegroundId(bgId), m_temporary(temporary)
    {
        m_updateTimer.Reset(2000);
    }
    bool OnSessionLoaded(PlayerBotEntry* entry, WorldSession* sess) override
    {
        return SpawnNewPlayer(sess, m_class, m_race, m_mapId, m_instanceId, m_x, m_y, m_z, m_o);
    }
  
    void OnPlayerLogin() final;
    void UpdateAI(uint32 const diff) final;
    void OnPacketReceived(WorldPacket const* packet) final;
    void MovementInform(uint32 MovementType, uint32 Data = 0) final;

    bool CanTryToCastSpell(Unit const*,SpellEntry const*) const override;
    SpellCastResult PolicyAwareCast(Unit*,uint32,bool);
    SpellCastResult PolicyAwareCast(Unit*,SpellEntry const*,bool);
    bool AllowLegacySpell(SpellEntry const*) const override;
    bool AllowLegacyFormExit() const;
    mutable BattleBotPolicyRuntime::State m_policy;
    bool ShouldIgnoreCombat() const;
    bool DrinkAndEat();
    bool UseMount(bool preparation=false);
    bool OwnsPreparationMovement() const;
    bool UpdateBattleGroundPreparation();
    void RefreshArathiOpeningPlan();
    std::string OpeningStatus() const;
    std::string AvRouteStatus() const;
    std::string PreparationStatus() const;
    std::string MountStatus() const;
    static std::vector<std::string> ArathiFlagStatus(Player* actor);
    uint32 GetMountSpellId() const;
    bool CheckForUnreachableTarget();
    float GetMaxAggroDistanceForMap() const;
    bool AttackStart(Unit* pVictim);
    Unit* SelectAttackTarget(Unit* pExcept = nullptr) const;
    Unit* SelectFollowTarget() const;
    bool IsTacticalBattleGround() const;
    bool TacticalRank(Unit* target, BattleBotTactics::Rank& rank) const;
    Unit* SelectTacticalTarget(Unit* except = nullptr) const;
    bool UpdateTacticalTarget();
    bool IsValidHostileTarget(Unit const* target) const;
    void RefreshWarsongTask();
    std::string WarsongStatus() const;
    static std::vector<std::string> WarsongRouteStatus(Player*);
    bool OwnsWarsongMovement();
    bool OwnsLegacyWaypointMovement() const;
    bool UpdateWarsongMovement();
    bool StartWarsongObjective();
    void RefreshArathiGuardAssignment();
    bool UpdateArathiAdvanceMovement();
    bool TryCaptureNearbyBattleFlag();
    bool TryCaptureArathiFlag();
    bool OwnsArathiAdvanceMovement();
    static std::vector<std::string> ArathiRouteStatus(Player* actor);
    bool UpdateArathiGuardMovement();
    bool FindAndHealInjuredAlly(float selfHealPercent = 100.0f, float groupHealPercent = 100.0f);


    void OnJustRevived();
    void OnJustDied();
    void OnEnterBattleGround();
    void OnLeaveBattleGround();

    void UpdateFlagCarrierAI();
    void UpdateBattleGroundAI();
    void UpdateInCombatAI() final;
    void UpdateOutOfCombatAI() final;
    void UpdateInCombatAI_Paladin() final;
    void UpdateOutOfCombatAI_Paladin() final;
    void UpdateInCombatAI_Shaman() final;
    void UpdateOutOfCombatAI_Shaman() final;
    void UpdateInCombatAI_Hunter() final;
    void UpdateOutOfCombatAI_Hunter() final;
    void UpdateInCombatAI_Mage() final;
    void UpdateOutOfCombatAI_Mage() final;
    void UpdateInCombatAI_Priest() final;
    void UpdateOutOfCombatAI_Priest() final;
    void UpdateInCombatAI_Warlock() final;
    void UpdateOutOfCombatAI_Warlock() final;
    void UpdateInCombatAI_Warrior() final;
    void UpdateOutOfCombatAI_Warrior() final;
    void UpdateInCombatAI_Rogue() final;
    void UpdateOutOfCombatAI_Rogue() final;
    void UpdateInCombatAI_Druid() final;
    void UpdateOutOfCombatAI_Druid() final;

    std::shared_ptr<BotMatch::Profile const> m_matchProfile;
    uint8 m_battlegroundId = 0;
    ShortTimeTracker m_updateTimer;
    uint8 m_race = 0;
    uint8 m_class = 0;
    uint8 m_level = 0;
    uint32 m_mapId = 0;
    uint32 m_instanceId = 0;
    float m_x = 0.0f;
    float m_y = 0.0f;
    float m_z = 0.0f;
    float m_o = 0.0f;
    bool m_temporary = false;
    bool m_wasDead = false;
    bool m_wasInBG = false;
    int32 m_arathiGuardNode = -1;
    bool m_arathiGuardAnchorValid = false;
    bool m_arathiGuardChanged = false;
    Position m_arathiGuardAnchor;
    int32 m_arathiAdvanceNode=-1;
    bool m_arathiAdvanceValid=false;
    Position m_arathiAdvanceGoal;
    uint64 m_arathiPlanAt=0,m_arathiMoveAt=0,m_arathiProgressAt=0;
    Position m_arathiProgress{};
    uint64 m_lastTacticalSwitch = 0;
    BattleBotTactics::WsgSquad m_wsgSquad = BattleBotTactics::WsgSquad::Midfield;
    BattleBotTactics::WsgTask m_wsgTask = BattleBotTactics::WsgTask::Midfield;
    BattleBotObjectivePolicy::Commitment m_wsgChoice;
    std::vector<BattleBotObjectivePolicy::Candidate> m_wsgEvaluated;
    uint64 m_wsgEvaluatedAt=0,m_wsgEvaluatedEvent=0,m_wsgEvaluatedTask=0;
    Position m_wsgEvaluatedPosition{};
    uint64 m_wsgEnemySeenAt=0,m_wsgScenarioEvent=0,m_wsgMotionSpline=0,m_wsgThreatSeenAt=0,m_wsgPressureUntil=0;
    ObjectGuid m_wsgSeenEnemy;
    Position m_wsgEnemyLast{},m_wsgThreatLast{},m_wsgMotionGoal{};
    bool m_wsgRevived=false,m_wsgOwnFlagAway=false,m_wsgLocalDefense=false,m_wsgPressed=false;
    std::string m_wsgScenario="none",m_wsgMotionReason="none";
    bool m_wsgGoalValid = false;
    bool m_wsgTravel = false;
    Position m_wsgGoal;
    Position m_wsgPathGoal;
    uint64 m_wsgRepathAt = 0;
    Position m_wsgProgressPosition{};
    uint64 m_wsgProgressAt = 0;
    ObjectGuid m_wsgCarrier;


    // Movement System
    void UpdateWaypointMovement();
    void DoGraveyardJump();
    void MoveToNextPoint();
    void AdvanceLegacyWaypoint(uint32 options,bool allowTaskTravel);
    BattleBotWaypointPolicy::Ticket m_waypointTicket;
    bool StartNewPathFromBeginning();
    void StartNewPathFromAnywhere();
    bool StartNewPathToObjective();
    bool StartNewPathToPosition(Position const& position, std::vector<BattleBotPath*> const& vPaths);
    void ClearPath();
    void StopMoving();
    bool m_doingGraveyardJump = false;
    uint32 m_avRouteStaleAt = 0;
    uint64 m_unreachableRetryAt=0;
    bool m_movingInReverse = false;
    uint32 m_currentPoint = 0;
    BattleBotPath* m_currentPath = nullptr;
    bool m_wsgOpeningDone = false;
    bool m_preparationWaiting=false,m_preparationBuffDone=false;
    mutable bool m_preparationUpdating=false,m_preparationTriedSpell=false;
    unsigned m_preparationQuiet=0;
    bool m_prepSpreadArrived=false,m_prepConverge=false,m_prepGoalReady=false;
    Position m_prepTarget{};
    std::vector<std::pair<Position,bool>> m_prepCandidates;
    unsigned m_prepCandidate=0,m_prepGeneration=0,m_prepMoves=0,m_prepAttempts=0;
    std::string m_abCaptureTrace="none",m_abCaptureEnd="unobserved";
    bool m_abLandRoute=false,m_abLandViaValid=false,m_abLandEscape=false;
    Position m_abLandVia{},m_abAdvanceDestination{};
    uint32 m_abAdvancePoint=0;
    float m_abLandEscapeLimit=20;
    uint32 m_abLandSpline=0;
    std::string m_abLandTrace="none";
    int32 m_abCaptureResult=-1;
    uint64 m_abCaptureAttemptAt=0;
    uint32 m_abCaptureHealth=0;
    uint64 m_abCaptureHurtAt=0,m_abCaptureRetryAt=0,m_abCaptureActiveFlag=0;
    uint64 m_abRushUntil=0;
    uint32 m_mountCost=0;
    uint32 m_mountAttempts=0;
    bool m_mountPreparation=false;
    int32 m_mountCastResult=-1;
    std::string m_mountReason="not_called";
    uint32 m_prepSpline=0,m_prepPoint=0;
    uint64 m_prepNextIdle=0;
    std::string m_prepPhase="idle",m_prepReject="none";
    uint64 m_preparationAt=0,m_preparationMoveAt=0;
    int32 m_abOpeningNode=-1;
    bool m_abOpeningComplete=false,m_abOpeningActualRaid=false,m_abHomeOpener=false;
    uint8 m_waitingSpot = BB_WSG_WAIT_SPOT_SPAWN;
};

#endif
