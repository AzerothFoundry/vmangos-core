#ifndef BG_FILL_LIFECYCLE_H
#define BG_FILL_LIFECYCLE_H
#include <array>
#include <cstdint>
#include <vector>
#include <map>
#include <algorithm>
namespace BgFillLifecycle {
// Client ids are allocated independently for every (type, bracket).
struct InstanceIdentity {uint32_t type,bracket,client,internal;bool joinable;};
inline uint32_t ResolveDesiredInstance(uint32_t type,uint32_t bracket,uint32_t client,
                                     std::vector<InstanceIdentity> const& instances)
{
    if(!client)return 0;
    uint32_t found=0;
    for(auto const& bg:instances)if(bg.type==type && bg.bracket==bracket && bg.client==client && bg.joinable){
        if(found && found!=bg.internal)return 0; // ambiguous domain fails closed
        found=bg.internal;
    }
    return found;
}
struct Location {
    bool sameQueue=false,sameBracket=false,cancelled=false,requestMember=false;
    uint32_t currentInstance=0,invitedInstance=0,desiredInstance=0;
};
inline bool PendingFor(uint32_t bound,Location const& h)
{
    if(!h.sameQueue || !h.sameBracket || h.cancelled || h.currentInstance)return false;
    if(bound) return h.invitedInstance ? h.invitedInstance==bound : (h.desiredInstance==bound || (h.requestMember && !h.desiredInstance));
    return !h.invitedInstance && !h.desiredInstance;
}
// Keep ownership across asynchronous native actor removal. Both manager and actor must be gone.
template<class Bot>
inline bool RetireOwned(std::vector<Bot>& active,std::map<uint32_t,Bot>& retiring,uint32_t guid)
{
    auto found=std::find_if(active.begin(),active.end(),[&](Bot const& b){return b.guid==guid;});
    if(found==active.end())return false;
    retiring.emplace(guid,*found);active.erase(found);return true;
}
template<class Bot,class Registered,class Present>
inline void PruneRetired(std::map<uint32_t,Bot>& retiring,Registered registered,Present present)
{
    for(auto i=retiring.begin();i!=retiring.end();) {
        if(!registered(i->first) && !present(i->first))i=retiring.erase(i);
        else ++i;
    }
}
inline bool InFlightArrival(uint32_t bound,uint32_t current,bool teleporting)
{
    return bound && bound==current && teleporting;
}
template<class Guid>
inline bool ObserveHumanPort(std::map<Guid,uint32_t>& humans,Guid guid,uint32_t level,
                             uint32_t bound,uint32_t destination,bool bot,bool sameBracket,bool joinable)
{
    if(!bound || bound!=destination || bot || !sameBracket || !joinable)return false;
    humans[guid]=level;return true;
}
struct Population {
    uint32_t target=0;bool ended=false;
    std::array<uint32_t,2> humans{{0,0}},pending{{0,0}},foreignBots{{0,0}},ownBots{{0,0}},retiringBots{{0,0}};
};
struct Plan { bool cleanup=false;std::array<uint32_t,2> budget{{0,0}},retire{{0,0}},add{{0,0}}; };
inline Plan Decide(Population const& p)
{
    Plan out;
    out.cleanup=p.ended || !(p.humans[0]+p.humans[1]+p.pending[0]+p.pending[1]);
    if(out.cleanup)return out;
    for(unsigned t=0;t<2;++t){
        uint64_t reserved=uint64_t(p.humans[t])+p.pending[t]+p.foreignBots[t];
        out.budget[t]=reserved>=p.target?0:p.target-uint32_t(reserved);
        out.retire[t]=p.ownBots[t]>out.budget[t]?1:0;
        out.add[t]=p.ownBots[t]<out.budget[t] && !p.retiringBots[t]?1:0;
    }
    return out;
}
}
#endif
