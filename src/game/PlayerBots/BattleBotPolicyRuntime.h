#ifndef BATTLEBOT_POLICY_RUNTIME_H
#define BATTLEBOT_POLICY_RUNTIME_H
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>
class BattleBotAI;
class CombatBotBaseAI;
class Player;
class Unit;
class WorldPacket;
struct SpellEntry;
namespace PvPCombatPolicy
{
struct Snapshot;
}
namespace BattleBotPolicyRuntime
{
struct Event
{
    uint64_t caster = 0;
    uint32_t spell = 0, time = 0;
    bool start = false;
};
struct State
{
    mutable std::recursive_mutex mutex;
    std::mutex eventMutex;
    std::atomic<bool> acceptEvents{false};
    uint32_t mapId = 0, instanceId = 0, scopeTag = 0;
    uint64_t duelFlag = 0;
    std::deque<Event> pending, history;
    std::map<uint64_t, uint64_t> starts;
    std::map<uint64_t, uint32_t> startSpells;
    std::map<uint64_t, std::pair<uint64_t, uint32_t>> channelLengths;
    std::map<std::string, uint32_t> lastUse;
    std::map<uint32_t, uint32_t> ownerRanks, petRanks;
    std::map<uint32_t, bool> petAuto;
    uint64_t petGuid = 0, ally = 0;
    uint32_t cacheAt = 0, fastAt = 0;
    unsigned enemies = 0;
    bool sensedOverflow = false;
    std::vector<uint64_t> sensedEnemies;
    enum Counter
    {
        OwnerCast,
        PetCast,
        RejectCooldown,
        RejectGcd,
        RejectRange,
        RejectResource,
        RejectControl,
        RejectShape,
        RejectUnknown,
        RejectOther,
        CounterCount
    };
    std::array<uint64_t, CounterCount> counters{};
    std::string last = "尚无决策", lastCast, legacy;
    bool executing = false;
    uint32_t permitRoot = 0;
    std::string permitActor;
};
// Production timing gate; tests exercise stable samples across changing snapshots.
bool DeclaresManaged(PvPCombatPolicy::Snapshot const &, unsigned classId, unsigned role, uint32_t root,
                     std::string const &actor, uint32_t (*normalize)(uint32_t));
bool TimingReady(double sampledChance, double sampledDepth, uint32_t sampledReaction, double chance, double minPct,
                 double maxPct, uint32_t minMs, uint32_t maxMs, double progress, double elapsed);
bool ExternalEnemyAllowed(bool duel, bool nativeDuelActive, bool matchesOpponent, bool playerControlled,
                          bool nativeAttackable);
bool ExternalAllyAllowed(bool duel, bool self, bool sameGroup, bool nativeAttackable);
// Combat-only facade: caller owns navigation, enemy acquisition and scene changes.
bool TickExternal(CombatBotBaseAI &, State &, bool duel, bool fast);
bool CanLegacyExternal(CombatBotBaseAI const &, State &, SpellEntry const *, bool duel);
void ObserveExternal(State &, WorldPacket const *);
void ForgetExternal(CombatBotBaseAI &, State &);
std::string ExplainExternal(State const &);
void EnsureConfigured();
bool Tick(BattleBotAI &, bool fast);
void ObservePacket(BattleBotAI &, WorldPacket const *);
bool CanLegacy(BattleBotAI const &, SpellEntry const *);
uint32_t ActualCastTime(Unit *, SpellEntry const *);
bool NativeCheck(Unit *, Unit *, SpellEntry const *, std::string &);
void Forget(BattleBotAI &);
std::string Explain(Player *);
} // namespace BattleBotPolicyRuntime
#endif
