#include "BattleBotObjectivePolicy.h"
#include <cassert>
#include <iostream>
using namespace BattleBotObjectivePolicy;
int main(){
 Commitment state;std::vector<Candidate> c{{1,1,5,2,1,true,100000},{2,2,10,2,1,true,100000}};
 auto first=Choose(state,10,1,1,100,4000,c);assert(first&&state.draws==1);
 for(unsigned i=101;i<4100;++i)assert(Choose(state,10,1,1,i,4000,c)==first);assert(state.draws==1);
 Choose(state,10,1,2,200,4000,c);assert(state.draws==2); // urgent state event breaks commitment
 for(auto& row:c)if(row.id==state.candidate)row.reachable=false;
 Choose(state,10,1,2,201,4000,c);assert(state.draws==3);
 assert(Choose(state,10,1,2,202,4000,c,true)==0&&state.candidate==0);
 Commitment hold;std::vector<Candidate> anchors{{1,1,1,0,0,true,100000},{2,1,1,0,0,true,100000}};
 auto anchor=Choose(hold,1,1,1,100,5000,anchors,false,true);assert(anchor);
 assert(Choose(hold,1,1,1,10000,5000,anchors,false,true)==anchor&&hold.draws==1);
 Choose(hold,1,1,2,10001,5000,anchors,false,true);assert(hold.draws==2); // pressure/flag event always breaks hold
 assert(!CanLeaveGuard(true,false)&&CanLeaveGuard(true,true)&&CanLeaveGuard(false,false));
 assert(CanExecute({0,0,Execution::Shared},10,100));assert(CanExecute({10,101,Execution::SingleOwner},10,100));assert(!CanExecute({10,100,Execution::SingleOwner},10,100));assert(!CanExecute({11,101,Execution::SingleOwner},10,100));
 assert(ObservationFresh(100,3100,3000)&&!ObservationFresh(100,3101,3000)&&!ObservationFresh(0,100,3000));
 assert(PursuitUseful(true,true,5,10)&&!PursuitUseful(true,false,5,10)&&!PursuitUseful(false,true,5,10)&&!PursuitUseful(true,true,11,10));
 Candidate x{1,1,10,1,0,true,10000},y=x;y.eta=30;assert(Weight(x,100)>Weight(y,100));y=x;y.enemyForce=2;assert(Weight(x,100)>Weight(y,100));y=x;y.friendlyForce=2;assert(Weight(x,100)<Weight(y,100));y=x;y.eta=NAN;assert(!Weight(y,100));y=x;y.expires=100;assert(!Weight(y,100));
 unsigned counts[3]={};for(Id actor=1;actor<=500;++actor){Commitment s;std::vector<Candidate> rows{{1,1,0,0,0,true,10000},{2,3,0,0,0,true,10000}};++counts[Choose(s,actor,1,1,100,1000,rows)];}assert(counts[1]>50&&counts[2]>counts[1]&&counts[2]<450);
 std::cout<<"PASS objective commitment/event/expiry/force/ETA/ownership/guard/freshness/pursuit\n";
}
