#!/usr/bin/env python3
"""Production recovery state transitions with native motion/spell boundaries stubbed explicitly."""
import pathlib, runpy, subprocess, tempfile
root=pathlib.Path(__file__).resolve().parents[1]
fixture=runpy.run_path(str(root/'tests/run_party_recovery_regression.py'))['fixture']
fixture=fixture.replace('using uint32=std::uint32_t;', 'using uint32=std::uint32_t;')
fixture=fixture.replace('bool IsEmpty()const{return !value;}', 'uint64 GetRawValue()const{return value;} bool IsEmpty()const{return !value;}')
fixture=fixture.replace('struct SpellEntry {uint32 Id=2007;};', '''enum Powers {POWER_HEALTH=-2, POWER_MANA=0};
constexpr int SPELL_CAST_OK=0, IDLE_MOTION_TYPE=0, FOLLOW_MOTION_TYPE=1, POINT_MOTION_TYPE=2, CHASE_MOTION_TYPE=3;
struct MovementGenerator {int kind=IDLE_MOTION_TYPE;virtual ~MovementGenerator()=default;};
template<class T>struct PointMovementGenerator:MovementGenerator {uint32 id=0;uint32 GetId()const{return id;}};
struct MotionMaster {
 MovementGenerator idle,other;PointMovementGenerator<Player> point;MovementGenerator* current=&idle;
 int expired=0,moved=0;MovementGenerator* GetCurrent(){return current;}
 int GetCurrentMovementGeneratorType(){return current->kind;}
 void MovementExpired(bool){++expired;current=&idle;}void MoveIdle(){current=&idle;}
 void MovePoint(uint32 id,float,float,float,int){++moved;point.kind=POINT_MOTION_TYPE;point.id=id;current=&point;}
};
constexpr int MOVE_PATHFINDING=1,MOVE_RUN_MODE=2,MOVE_EXCLUDE_STEEP_SLOPES=4;
struct SpellEntry {uint32 Id=2007;int powerType=POWER_MANA;
 uint32 GetCastTime(Player*)const{return 10000;}bool IsTargetInRange(Player*,Player*)const;};''')
fixture=fixture.replace('int getState(){return state;}', 'int getState(){return state;} static uint32 CalculatePowerCost(const SpellEntry*,Player*){return 100;}')
fixture=fixture.replace(' BotAI* AI(){return botAI;}', ''' BotAI* AI(){return botAI;}
 MotionMaster motion;bool bg=false,nonMelee=false,ready=true,gcd=false;uint32 mana=100;int stops=0;
 MotionMaster* GetMotionMaster(){return &motion;} void StopMoving(){++stops;}
 bool IsStopped(){return motion.current->kind==IDLE_MOTION_TYPE;}
 bool InBattleGround(){return bg;}bool IsNonMeleeSpellCasted(bool,bool,bool){return nonMelee;}
 Player* GetPet(){return nullptr;} uint32 GetPower(Powers){return mana;}uint32 GetHealth(){return 100;}
 bool IsSpellReady(SpellEntry const*){return ready;}bool HasGCD(SpellEntry const*){return gcd;}
 bool IsWithinLOSInMap(Player* p){return p->los;}
 float GetPositionX(){return distance;}float GetPositionY(){return 0;}float GetPositionZ(){return 0;}
''')
fixture=fixture.replace('std::map<uint64,Player*> players;', '''bool SpellEntry::IsTargetInRange(Player*,Player* p)const{return p->distance<30;}
struct PathFinder {};
bool actualSafe=true,plannedSafe=true;int plans=0;
bool SafeResurrectionMotion(Player*){return actualSafe;}
bool SafeResurrectionApproach(Player*,Player*,SpellEntry const*,float& x,float& y,float& z){++plans;x=50;y=z=0;return plannedSafe;}
std::map<uint64,Player*> players;''')
fixture=fixture.replace(' Player* SelectResurrectionTarget()const;', ''' ObjectGuid m_resurrectionApproachTarget;uint32 m_resurrectionApproachStarted=0;
 bool m_preventCasting=false,canCast=true;int castCount=0;
 bool OwnsResurrectionApproach()const;void StopResurrectionApproach();bool TryResurrectParty();
 bool CanTryToCastSpell(Player*,SpellEntry const*){return canCast;}
 void PrepareRecoveryCast(bool){}int DoCastSpell(Player*,SpellEntry const*){++castCount;return SPELL_CAST_OK;}
 Player* SelectResurrectionTarget()const;''')
source=(root/'src/game/PlayerBots/PartyBotAI.cpp').read_text()
a=source.index('struct ResurrectionReservation\n');b=source.index('\nvoid PartyBotAI::PrepareRecoveryCast(',a)
c=source.index('bool PartyBotAI::OwnsResurrectionApproach()');end=source.index('\nPlayer* PartyBotAI::SelectShieldTarget()',c)
tests=r'''
int main(){
 SpellEntry rez;Player self{1},human{2};self.alive=true;human.distance=90;
 GroupReference a{&human};Group g{&a};self.group=human.group=&g;players={{1,&self},{2,&human}};
 PartyBotAI ai{&self,&rez,ObjectGuid(2)};
 auto start=[&](){ai.m_resurrectionApproachTarget=ObjectGuid(2);ai.m_resurrectionApproachStarted=1000;
 ai.m_failedResurrectionTargets.clear();self.motion.point.kind=POINT_MOTION_TYPE;self.motion.point.id=0x5253;
 self.motion.current=&self.motion.point;};
 auto stopped=[&](){assert(ai.m_resurrectionApproachTarget.IsEmpty());assert(self.motion.current==&self.motion.idle);};
 start();self.combat=true;assert(!ai.TryResurrectParty());stopped();self.combat=false;
 start();human.combat=true;assert(!ai.TryResurrectParty());stopped();human.combat=false;
 start();self.mana=99;assert(!ai.TryResurrectParty());stopped();self.mana=100;
 start();self.gcd=true;assert(!ai.TryResurrectParty());stopped();self.gcd=false;
 start();self.ready=false;assert(!ai.TryResurrectParty());stopped();self.ready=true;
 start();self.teleport=true;assert(!ai.TryResurrectParty());stopped();self.teleport=false;
 start();human.request=true;assert(!ai.TryResurrectParty());stopped();human.request=false;
 start();ai.m_preventCasting=true;assert(!ai.TryResurrectParty());stopped();ai.m_preventCasting=false;
 start();self.group=nullptr;assert(!ai.TryResurrectParty());stopped();self.group=&g;
 start();self.motion.point.id=0x4D43;int expired=self.motion.expired;
 assert(!ai.TryResurrectParty());assert(ai.m_resurrectionApproachTarget.IsEmpty());
 assert(self.motion.current==&self.motion.point&&self.motion.point.id==0x4D43&&self.motion.expired==expired); // MC wins.
 start();self.motion.other.kind=CHASE_MOTION_TYPE;self.motion.current=&self.motion.other;
 assert(!ai.TryResurrectParty());assert(self.motion.current==&self.motion.other); // Other movement wins.
 start();actualSafe=false;plannedSafe=true;int beforePlans=plans;
 assert(!ai.TryResurrectParty());stopped();assert(plans==beforePlans); // Old actual route checked before a safer candidate.
 actualSafe=true;start();ai.StopResurrectionApproach();human.distance=90;ai.m_failedResurrectionTargets.clear();
 actualSafe=false;assert(!ai.TryResurrectParty());stopped();assert(self.motion.moved>0); // Immediate post-issue validation cancels.
 actualSafe=true;ai.m_failedResurrectionTargets.clear();human.distance=10;self.mana=100;
 assert(ai.TryResurrectParty());assert(ai.castCount==1); // One cast worth of mana, not full mana.
 start();Spell spell{&rez,{&human}};self.cast=&spell;self.nonMelee=true;
 assert(!ai.TryResurrectParty());stopped();assert(self.cast==&spell); // Stop approach, never interrupt active spell.
 ai.me=nullptr;ai.StopResurrectionApproach();assert(!ai.OwnsResurrectionApproach());
 std::cout<<"production recovery transitions: combat/group/resource/GCD/prevent cancellation, foreign motion preserved, actual-route-before-replan, immediate issued-path rejection PASS\n";
}
'''
with tempfile.TemporaryDirectory() as d:
 p=pathlib.Path(d);(p/'test.cpp').write_text(fixture+source[a:b]+source[c:end]+tests)
 subprocess.run(['c++','-std=c++14','-Wall','-Wextra','-Wno-missing-field-initializers','-I'+str(root/'src/game/PlayerBots'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
