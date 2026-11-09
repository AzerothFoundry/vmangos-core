#ifndef MANGOS_BATTLEBOT_TACTICS_H
#define MANGOS_BATTLEBOT_TACTICS_H
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>
class Unit;
namespace BattleBotTactics
{
using Guid=std::uint64_t;
struct Rank
{
    Guid guid=0;
    unsigned tier=0;
    unsigned distanceBucket=0;
    bool healer=false;
    std::uint64_t damage=0;
    bool playerBody=true;
};
enum class WsgSquad { Flag, Midfield };
enum class WsgTask { Capture, Midfield, Recover, Escort, Return };
struct WsgCandidate { Guid guid=0;unsigned playerClass=0;bool healer=false; };
bool WsgSupportEligible(float distance,float carrierX);
unsigned WsgPriority(bool enemyCarrier,bool carrierThreat,bool humanThreat);
bool WsgAttackAllowed(bool healer,bool threatensSelf,float distance);
bool WsgObjectiveCombatAllowed(WsgTask task,float targetToGoal,float selfToGoal,bool threatensSelf);
bool WsgNeedsRegroup(WsgTask task,float distance);
float WsgStandOff(WsgTask task,bool healer,bool droppedFlag=false);
std::map<Guid,WsgSquad> AssignWsgSquads(std::vector<WsgCandidate> candidates);
WsgTask PlanWsgTask(WsgSquad squad,bool carrying,bool friendlyCarrier,bool ownFlagAway,unsigned flagIndex=0);
enum class AbState { Neutral, FriendlyContested, FriendlyOccupied, EnemyContested, EnemyOccupied };
bool AbSecured(AbState state);
int AbChooseSide(std::array<AbState,5> const& states,std::array<float,5> const& costs,int previousSide);
int AbAdvanceNode(bool alliance,std::array<AbState,5> const& states,std::array<float,5> const& costs,int previous);
bool AbAdvanceAttackAllowed(float targetToGoal,bool threatensSelf);
bool RouteNeedsRefresh(bool moving,float progress,std::uint64_t elapsedMs);
bool MountSavesTime(float pathLength,float currentSpeed,float mountedSpeed,float castSeconds);
unsigned DistanceBucket(float distance);
bool Better(Rank const& a,Rank const& b);
bool ShouldSwitch(Rank const& candidate,Rank const& current,std::uint64_t heldMs);
bool InGuardLeash(float distanceToNode);
struct GuardCandidate
{
    Guid guid=0;
    bool alive=true;
    bool healer=false;
    std::array<float,5> distance;
};
bool AbBannerMatches(unsigned node,unsigned state,uint32_t entry,bool locked,bool properType,bool spawned);
struct AbCaptureRisk {unsigned friends=0,enemies=0;bool combat=false;bool threatened=false,periodic=false,recentDamage=false,healer=false;};
bool AbCaptureOpportunity(AbCaptureRisk const&);
bool AbLandSample(bool wet,bool& drySeen,float& prefixWet,float step,float limit=20);
bool AbAdvanceBeforeRecovery(bool guard,bool combat,float health,float mana,bool usesMana,bool openingRush=false);
struct AbCaptureLease {std::uint64_t owner=0,flag=0,until=0;};
bool AbClaimCapture(AbCaptureLease&,std::uint64_t owner,std::uint64_t flag,std::uint64_t now);
bool AbCaptureCandidateReady(std::uint64_t now,std::uint64_t retryAt,std::uint64_t hurtAt);
struct PrepOffset {float lateral=0,depth=0;};
PrepOffset PreparationOffset(Guid actor,uint32_t instance,uint32_t generation,unsigned candidate,bool converge);
uint32_t PreparationIdleInterval(Guid actor,uint32_t instance,uint32_t generation);
bool PreparationDetour(float length,float direct);
struct AbOpeningMember {Guid guid=0;unsigned group=99;bool bot=false,healer=false;};
struct AbOpeningPlan {std::map<Guid,int> nodes;Guid home=0;bool actualRaid=false;};
AbOpeningPlan PlanAbOpening(std::vector<AbOpeningMember> members,bool alliance);
bool InsideStartingGate(float x,float y,float gateX,float gateY,float safeX,float safeY,float margin=0.4f);
using Assignments=std::array<Guid,5>;
Assignments AssignGuards(std::array<bool,5> const& occupied,Assignments previous,std::vector<GuardCandidate> const& candidates);
class RollingDamage
{
public:
    void Add(std::uint32_t instance,Guid attacker,std::uint32_t damage,std::uint64_t nowMs);
    std::uint64_t Get(std::uint32_t instance,Guid attacker,std::uint64_t nowMs);
    void Prune(std::uint64_t nowMs);
private:
    struct Event { std::uint64_t time;std::uint32_t damage; };
    std::map<std::pair<std::uint32_t,Guid>,std::deque<Event>> rows;
};
// Passive telemetry only: caller has already applied immunity/overkill rules.
void RecordDamage(Unit const* attacker,Unit const* victim,std::uint32_t effectiveDamage);
std::uint64_t Damage15(Unit const* attacker);
}
#endif
