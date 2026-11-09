#!/usr/bin/env python3
"""Exercise production path-validation diagnostics, including the empty-path assertion case."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/game/Commands/GmInstanceBossTeleport.h').read_text()
a=s.index('        bool calculated=path.calculate(');b=s.index('        auto cast=',a)
fixture=r'''
#include <cassert>
#include <cmath>
#include <vector>
#include <stdexcept>
constexpr int PATHFIND_NORMAL=1;
struct Vec {float x=0,y=0,z=0;Vec operator-(Vec r)const{return {x-r.x,y-r.y,z-r.z};}float length()const{return std::sqrt(x*x+y*y+z*z);}};
struct Path {
 bool success=true;std::vector<int> points{1,2};int type=1;float distance=20;Vec actual{10,0,0};
 int lengthCalls=0,endCalls=0;
 bool calculate(Vec,Vec,bool,bool){return success;}
 auto const& getPath()const{return points;}
 int getPathType()const{return type;}
 Vec getActualEndPosition(){if(!success||points.empty())throw std::runtime_error("invalid endpoint read");++endCalls;return actual;}
 float Length(){if(!success||points.empty())throw std::runtime_error("native empty-path assertion");++lengthCalls;return distance;}
};
struct Diagnostic {int path=0,firstPathType=0;float firstPathLength=0,firstPathError=0;};
bool check(Path& path,Diagnostic& d) {Vec start{},end{10,0,0},found;for(int n=0;n<1;++n){
'''
tests=r'''
}return false;}
int main(){
 Path empty;empty.success=false;empty.points.clear();Diagnostic d;
 assert(!check(empty,d)&&d.path==1&&d.firstPathLength==0&&d.firstPathError==-1);
 assert(empty.lengthCalls==0&&empty.endCalls==0);
 Path failed;failed.success=false;Diagnostic f;assert(!check(failed,f));assert(!failed.lengthCalls&&!failed.endCalls);
 Path noPoints;noPoints.points.clear();Diagnostic n;assert(!check(noPoints,n));assert(!noPoints.lengthCalls&&!noPoints.endCalls);
 Path good;Diagnostic g;assert(check(good,g)&&g.path==0&&good.lengthCalls==1&&good.endCalls==1);
 Path tooLong;tooLong.distance=321;Diagnostic t;assert(!check(tooLong,t)&&t.firstPathLength==321);
 Path wrongEnd;wrongEnd.actual={20,0,0};Diagnostic w;assert(!check(wrongEnd,w)&&w.firstPathError==10);
}
'''
with tempfile.TemporaryDirectory() as folder:
 p=Path(folder);(p/'test.cpp').write_text(fixture+s[a:b]+'return true;\n'+tests)
 subprocess.run(['c++','-std=c++14',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS production path guard: failed/empty path never reads endpoint or Length; valid, too-long and wrong-endpoint paths retained')
