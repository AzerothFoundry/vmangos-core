#include "../src/game/Commands/GmSkillQueryPolicy.h"
#include <cassert>
int main(){using namespace GmSkillQueryPolicy;
assert(Forceable(true,true,false,false));assert(!Forceable(false,true,false,false));assert(!Forceable(true,false,false,false));assert(!Forceable(true,true,true,false));assert(!Forceable(true,true,false,true));
std::string text;for(int i=0;i<101;++i)text+="法";text+="|\nend";
auto parts=Parts(text);std::string joined;for(auto const& p:parts){assert(p.size()<=180);assert((static_cast<unsigned char>(p[0])&0xc0)!=0x80);assert(p.find('|')==p.npos);joined+=p;}assert(joined==Clean(text));assert(Clip(text,100).size()==99);assert(Parts("").size()==1);
}
