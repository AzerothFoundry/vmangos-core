#!/usr/bin/env python3
from pathlib import Path
import tempfile,subprocess
root=Path(__file__).resolve().parents[1];s=(root/'src/game/Maps/PathFinder.cpp').read_text();a=s.index('    if(m_searchNodes && m_navMesh){');b=s.index('\n    //DEBUG_FILTER_LOG',a);alloc=s[a:b]
a=s.index('    if(m_ownedNavQuery){dtFreeNavMeshQuery(m_ownedNavQuery);m_ownedNavQuery=nullptr;}');b=s.index('    m_pathPoints.clear();',a);reset=s[a:b]
cpp=r"""
#include <cassert>
using uint32=unsigned;constexpr unsigned PATHFIND_NOPATH=8;unsigned allocated=0,freed=0;bool failAlloc=false,failInit=false;
struct Query{int init(void const*,unsigned){return failInit?1:0;}};using dtNavMeshQuery=Query;Query* dtAllocNavMeshQuery(){if(failAlloc)return nullptr;++allocated;return new Query;}void dtFreeNavMeshQuery(Query* p){assert(p);++freed;delete p;}bool dtStatusFailed(int status){return status!=0;}
struct PathInfo{unsigned m_searchNodes=0,m_type=0;Query* m_ownedNavQuery=nullptr;Query const* m_navMeshQuery=nullptr;void const* m_navMesh=nullptr;bool setup(){
"""+alloc+r"""
return true;}void reset(){
"""+reset+r"""
}~PathInfo(){if(m_ownedNavQuery)dtFreeNavMeshQuery(m_ownedNavQuery);}};
int main(){int mesh;Query shared;{PathInfo p;p.m_navMesh=&mesh;p.m_navMeshQuery=&shared;assert(p.setup());assert(!p.m_ownedNavQuery&&allocated==0);p.m_searchNodes=8192;assert(p.setup());assert(p.m_ownedNavQuery&&p.m_navMeshQuery==p.m_ownedNavQuery);p.reset();assert(!p.m_ownedNavQuery&&!p.m_navMeshQuery&&!p.m_navMesh);assert(freed==1);p.m_navMesh=&mesh;assert(p.setup());}assert(allocated==freed);
{PathInfo p;p.m_searchNodes=8192;p.m_navMesh=&mesh;failInit=true;assert(!p.setup());assert(!p.m_ownedNavQuery&&p.m_type==PATHFIND_NOPATH);}assert(allocated==freed);failInit=false;failAlloc=true;{PathInfo p;p.m_searchNodes=8192;p.m_navMesh=&mesh;assert(!p.setup());assert(!p.m_ownedNavQuery);}assert(allocated==freed);}
"""
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'case.cpp';exe=Path(d)/'case';p.write_text(cpp);subprocess.run(['c++','-std=c++17',str(p),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
print('PASS extracted production query ownership: default shared, opt-in owned, recalc reset, destruction, alloc/init failures')
