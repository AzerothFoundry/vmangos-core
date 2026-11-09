// Native serialization regression. See run_spline_packet_regression.py.
#include "MoveSpline.h"
#include "packet_builder.h"
#include "WorldPacket.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
char const* g_mainLogFileName = nullptr;
using namespace Movement;
static void require(bool ok, char const* why) { if (!ok) { std::cerr << why << "\n"; std::exit(1); } }
static bool near(Vector3 a, Vector3 b) { return (a-b).length() < 0.01f; }
struct Packet { Vector3 start; uint32 duration; std::vector<Vector3> nodes; int last; };
static Packet decode(WorldPacket& wire,int last)
{
    Packet p;p.last=last;
    uint32 id, flags, count; uint8 type;
    wire >> p.start.x >> p.start.y >> p.start.z >> id >> type >> flags >> p.duration >> count;
    require(type==0, "unexpected facing");
    Vector3 dest; wire >> dest.x >> dest.y >> dest.z;
    for (uint32 i=1; i<count; ++i) {
        uint32 packed; wire >> packed;
        auto sign=[](uint32 v,unsigned bits) { return int32(v << (32-bits)) >> (32-bits); };
        Vector3 offset(sign(packed&2047,11)*.25f, sign((packed>>11)&2047,11)*.25f, sign(packed>>22,10)*.25f);
        p.nodes.push_back(dest-offset);
    }
    p.nodes.push_back(dest);
    require(wire.rpos()==wire.size(), "unparsed bytes");
    return p;
}
static Packet packet(MoveSpline const& spline, unsigned budget=80)
{
    WorldPacket wire;
    int last=PacketBuilder::WriteMonsterMove(spline,wire,spline._currentSplineIdx()+1,budget);
    return decode(wire,last);
}
static void verify(MoveSpline const& spline, Packet const& p)
{
    require(near(p.start,spline.ComputePosition()), "packet starts at stale origin");
    int first=spline._currentSplineIdx()+1;
    for (unsigned i=0;i<p.nodes.size();++i)
        require(near(p.nodes[i],spline.getPath()[first+i]), "omitted or reordered unvisited corner");
    int end=first+int(p.nodes.size())-1;
    require(p.duration==uint32(spline._Spline().length(end)-spline.timePassed()), "duration does not match transmitted endpoint");
    double length=(p.nodes[0]-p.start).length();
    for(unsigned i=1;i<p.nodes.size();++i) length+=(p.nodes[i]-p.nodes[i-1]).length();
    require(std::abs(length*1000/p.duration-7.0)<.06, "wire movement accelerated");
}
static void verifyQuantized(MoveSpline const& spline, Packet const& p)
{
    require(near(p.start,spline.ComputePosition()), "clipped packet has stale start");
    require(!p.nodes.empty() && p.nodes.size()<=80, "invalid clipped node budget");
    int first=spline._currentSplineIdx()+1;
    double wireLength=0,serverLength=0;Vector3 wirePrevious=p.start,serverPrevious=p.start;
    for(unsigned i=0;i<p.nodes.size();++i) {
        Vector3 expected=spline.getPath()[first+i];Vector3 delta=p.nodes[i]-expected;
        require(fabs(delta.x)<=.126 && fabs(delta.y)<=.126 && fabs(delta.z)<=.636, "packed coordinate overflow or skipped node");
        wireLength+=(p.nodes[i]-wirePrevious).length();serverLength+=(expected-serverPrevious).length();
        wirePrevious=p.nodes[i];serverPrevious=expected;
    }
    int end=first+int(p.nodes.size())-1;
    require(p.duration==uint32(spline._Spline().length(end)-spline.timePassed()),"clipped duration mismatches endpoint");
    require(p.last==(end<spline._Spline().last()?end:-1), "continuation marker lost remainder");
    require(fabs(wireLength-serverLength)<=p.nodes.size()*1.3, "wire path spuriously accelerates");
    require(near(p.nodes.back(),spline.getPath()[end]), "float endpoint changed");
}
static void completePath(PointsArray const& points)
{
    MoveSplineInitArgs args;args.velocity=7;args.path=points;MoveSpline move;move.Initialize(args);
    Packet p=packet(move);verifyQuantized(move,p);move.setLastPointSent(p.last);
    unsigned updates=0,packets=1,checks=0;
    // Mirror Unit::UpdateSplineMovement: keep the real spline and advertise successive prefixes.
    while(!move.Finalized() && updates++<1000000) {
        move.updateState(100);
        if(move.NeedsPacketContinuation()) {
            ++checks;WorldPacket wire;
            if(PacketBuilder::WriteContinuation(move,wire,80)) {
                p=decode(wire,move.getLastPointSent());verifyQuantized(move,p);++packets;
            }
            WorldPacket repeat;require(!PacketBuilder::WriteContinuation(move,repeat,80),"same segment rechecked or rebroadcast");
        }
    }
    require(move.Finalized(),"clipped movement never reaches destination");
    require(near(move.ComputePosition(),points.back()),"clipped movement stopped mid-route");
    require(packets<=points.size() && checks<=points.size(),"continuation traffic scales with tick count");
    std::cout<<"route nodes="<<points.size()<<" packets="<<packets<<" completed at 7 yd/s\n";
}
static void boundary(Vector3 const& offset,unsigned expected)
{
    MoveSplineInitArgs args;args.velocity=7;
    args.path={Vector3(-1,-1,-1),Vector3(0,0,0),offset};MoveSpline move;move.Initialize(args);
    Packet p=packet(move);verifyQuantized(move,p);
    require(p.nodes.size()==expected,"rounded signed packing boundary clipped incorrectly");
}
int main()
{
    MoveSplineInitArgs args; args.velocity=7; args.splineId=123;
    // Alternating right angle corners expose endpoint skipping; all pack offsets are representable.
    for(int i=0;i<201;++i) args.path.push_back(Vector3((i+1)/2, i/2, 0));
    MoveSpline spline; spline.Initialize(args);
    Packet initial=packet(spline); verify(spline,initial);
    require(initial.nodes.size()==80 && initial.last==81, "80 endpoint budget not honored");
    spline.updateState(spline._Spline().length(78)+71);
    Packet continuation=packet(spline); verify(spline,continuation);
    require(continuation.nodes.size()==80 && continuation.last==158, "continuation budget wrong");
    spline.updateState(spline._Spline().length(156)+71-spline.timePassed());
    Packet tail=packet(spline); verify(spline,tail); require(tail.last==-1,"tail should finish");
    std::cout << "201-node path: initial=" << initial.duration << "ms, continuation=" << continuation.duration << "ms, tail=" << tail.duration << "ms; speed matches 7 yd/s (integer segment timing); all corners retained\n";
    for (unsigned count : {80u, 81u, 82u}) {
        MoveSplineInitArgs boundary = args; boundary.path.resize(count);
        MoveSpline check; check.Initialize(boundary);
        Packet p = packet(check); verify(check,p);
        require(p.nodes.size()==std::min(count-1,80u), "budget boundary count wrong");
        require((p.last==-1)==(count<=81), "budget boundary completion wrong");
    }
    // Same geometry and timestamps with a nonzero public path offset must be identical.
    args.path_Idx_offset=900; MoveSpline offset; offset.Initialize(args); offset.updateState(spline.timePassed());
    verify(offset,packet(offset));
    args.path_Idx_offset=0; args.path.resize(3); MoveSpline shortPath; shortPath.Initialize(args);
    Packet shortPacket=packet(shortPath); verify(shortPath,shortPacket);
    require(shortPacket.nodes.size()==2 && shortPacket.last==-1,"short path lost first corner");
    shortPath.updateState(71); verify(shortPath,packet(shortPath));
    args.path.resize(2); MoveSpline oneSegment; oneSegment.Initialize(args); verify(oneSegment,packet(oneSegment));
    verify(oneSegment,packet(oneSegment,1));
    std::cout << "80/81/82-node boundaries, 2/3-node paths and offset=900: passed\n";
    PointsArray ordinary;for(int i=0;i<201;++i)ordinary.push_back(Vector3(float(i),0,0));completePath(ordinary);
    for(float scale:{8.f,32.f,128.f}) {
        PointsArray longRoute;
        for(int i=0;i<401;++i)longRoute.push_back(Vector3(scale*((i+1)/2),scale*(i/2),0));
        completePath(longRoute);
        for(auto& p:longRoute){p.x=-p.x;p.y=-p.y;}completePath(longRoute);
    }
    PointsArray zRoute,returning;
    for(int i=0;i<161;++i){zRoute.push_back(Vector3(i*2.f,0,16.f*i));returning.push_back(Vector3(float(i%20)*32, float(i/20)*8, float(i%7)*24));}
    completePath(zRoute);for(auto& p:zRoute)p.z=-p.z;completePath(zRoute);completePath(returning);
    completePath({Vector3(0,0,0),Vector3(1000,0,0),Vector3(-1000,0,0),Vector3(0,0,0)});
    MoveSplineInitArgs restart;restart.velocity=7;restart.path={Vector3(0,0,0),Vector3(1000,0,0),Vector3(-1000,0,0),Vector3(0,0,0)};
    MoveSpline reused;reused.Initialize(restart);Packet init=packet(reused);reused.setLastPointSent(init.last);
    require(!reused.NeedsPacketContinuation(),"initial packet triggers same-segment resend");
    reused.updateState(reused._Spline().length(2)+100);require(reused.NeedsPacketContinuation(),"new segment failed to request continuation");
    WorldPacket continuedWire;require(PacketBuilder::WriteContinuation(reused,continuedWire,80),"new segment did not extend prefix");
    reused._Interrupt();require(!reused.NeedsPacketContinuation(),"interrupted spline still emits packets");
    restart.velocity=14;reused.Initialize(restart);Packet reset=packet(reused);verifyQuantized(reused,reset);reused.setLastPointSent(reset.last);
    require(!reused.NeedsPacketContinuation() && reset.duration<init.duration,"replacement/speed-change spline retained old cursor or duration");
    // The maximum valid prefix is not monotonic in endpoint distance.
    MoveSplineInitArgs folded;folded.velocity=7;folded.path={Vector3(-1,0,0),Vector3(0,0,0),Vector3(300,0,0),Vector3(150,0,0)};
    MoveSpline fold;fold.Initialize(folded);Packet fp=packet(fold);verifyQuantized(fold,fp);require(fp.nodes.size()==3,"failed to choose later representable prefix");
    boundary(Vector3(255.874f,0,0),2);boundary(Vector3(255.875f,0,0),1);
    boundary(Vector3(-256.124f,0,0),2);boundary(Vector3(-256.125f,0,0),1);
    boundary(Vector3(0,255.875f,0),1);boundary(Vector3(0,-256.125f,0),1);
    boundary(Vector3(0,0,127.874f),2);boundary(Vector3(0,0,127.875f),1);
    boundary(Vector3(0,0,-128.124f),2);boundary(Vector3(0,0,-128.125f),1);
    std::cout<<"signed XY/Z quarter-yard rounding, folded paths and full continuation progress: passed\n";
}
