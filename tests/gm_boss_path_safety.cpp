#include "GmBossPathSafety.h"
#include <vector>
#include <cassert>
struct Point {float x,y,z;};
int main() {
 std::vector<Point> corner{{0,0,0},{0,10,0},{10,10,0}};std::size_t failed;int calls=0;
 auto turnSafe=[&](float ax,float ay,float az,float bx,float by,float bz){
  ++calls;assert(az==1 && bz==1);return ax==bx || ay==by; // walls forbid the diagonal, legal corner path passes.
 };
 assert(!turnSafe(0,0,1,10,10,1));calls=0;
 assert(GmInstanceBoss::PathSegmentsClear(corner,2,turnSafe,failed));assert(calls==2);
 auto closedDoor=[](float ax,float,float,float bx,float,float){return ax==bx;};
 assert(!GmInstanceBoss::PathSegmentsClear(corner,2,closedDoor,failed));assert(failed==2);
 auto badFirst=[](float,float ay,float,float,float by,float){return ay==by;};
 assert(!GmInstanceBoss::PathSegmentsClear(corner,2,badFirst,failed));assert(failed==1);
 assert(!GmInstanceBoss::PathSegmentsClear(corner,0,turnSafe,failed));
 corner.resize(1);assert(!GmInstanceBoss::PathSegmentsClear(corner,2,turnSafe,failed));
}
