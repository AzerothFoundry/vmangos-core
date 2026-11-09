#ifndef MANGOS_BATTLEBOT_OBJECTIVE_POLICY_H
#define MANGOS_BATTLEBOT_OBJECTIVE_POLICY_H
#include <cstdint>
#include <vector>
#include <cmath>
#include <limits>
namespace BattleBotObjectivePolicy {
using Id=std::uint64_t;
enum class Execution {Shared,SingleOwner};
struct Candidate {Id id=0;double benefit=0,eta=0,friendlyForce=0,enemyForce=0;bool reachable=false;Id expires=0;};
struct Commitment {Id task=0,event=0,candidate=0,until=0,draws=0;};
inline Id Hash(Id n){n+=0x9e3779b97f4a7c15ULL;n=(n^(n>>30))*0xbf58476d1ce4e5b9ULL;n=(n^(n>>27))*0x94d049bb133111ebULL;return n^(n>>31);}
inline double Weight(Candidate const& c,Id now){
 if(!c.id||!c.reachable||c.expires<=now||!std::isfinite(c.benefit)||c.benefit<=0||!std::isfinite(c.eta)||c.eta<0||!std::isfinite(c.friendlyForce)||c.friendlyForce<0||!std::isfinite(c.enemyForce)||c.enemyForce<0)return 0;
 return c.benefit*(1+c.friendlyForce)/(1+c.enemyForce)/(1+c.eta/10);
}
inline void Forget(Commitment& s){s.task=0;s.event=0;s.candidate=0;s.until=0;}
// Candidates must already be constructed from legal observed information/native
// paths. This layer has no world pointers and cannot authorize hidden knowledge.
inline Id Choose(Commitment& s,Id actor,Id task,Id event,Id now,Id holdMs,std::vector<Candidate> const& candidates,bool completed=false,bool preserveValid=false){
 if(completed){Forget(s);return 0;}
 if(s.task==task&&s.event==event&&(now<s.until||preserveValid))for(auto const& c:candidates)if(c.id==s.candidate&&Weight(c,now)>0)return c.id;
 s.task=task;s.event=event;s.candidate=0;s.until=now+holdMs;
 double total=0;for(auto const& c:candidates)total+=Weight(c,now);
 if(!(total>0)||!std::isfinite(total))return 0;
 ++s.draws;double ticket=double(Hash(actor^Hash(task)^Hash(event)^Hash(s.draws))>>11)*(1.0/9007199254740992.0)*total;
 for(auto const& c:candidates){double w=Weight(c,now);if(w<=0)continue;if(ticket<w){s.candidate=c.id;return c.id;}ticket-=w;}
 return 0;
}
struct Ownership {Id owner=0,until=0;Execution execution=Execution::Shared;};
inline bool CanLeaveGuard(bool finalGuard,bool replacementReady){return !finalGuard||replacementReady;}
inline bool CanExecute(Ownership const& o,Id actor,Id now){return o.execution==Execution::Shared||(o.owner==actor&&now<o.until);}
inline bool ObservationFresh(Id seenAt,Id now,Id ttl){return seenAt&&now>=seenAt&&now-seenAt<=ttl;}
inline bool PursuitUseful(bool objectiveActive,bool observedFresh,double eta,double usefulUntil){return objectiveActive&&observedFresh&&std::isfinite(eta)&&eta>=0&&std::isfinite(usefulUntil)&&eta<=usefulUntil;}
}
#endif
