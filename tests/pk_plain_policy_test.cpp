#include "../src/game/PlayerBots/PKPlainPolicy.h"
#include <cassert>
#include <limits>
int main(){using namespace PKPlainPolicy;
// A complete mesh route alone does not make the 11.6yd-high screenshot hill suitable.
assert(!Height(22.9853f,34.6073f,34.6073f));assert(Height(22.9853f,23.2f,23.2f));
assert(!Height(22.9853f,26.1f,26.1f));assert(!Height(23,23,26));assert(!Height(23,std::numeric_limits<float>::quiet_NaN(),23));
// Four neighboring samples enforce local flatness, including one cliff edge.
float samples[]={23.1f,23.5f,22.7f,24.3f};bool safe=true;for(float sample:samples)safe=safe&&Neighbor(23,sample);assert(!safe);
assert(Neighbor(23,23.7f));assert(!Neighbor(23,26));assert(!Neighbor(23,std::numeric_limits<float>::infinity()));
assert(Endpoint(.5f,.3f));assert(!Endpoint(1.1f,0));assert(!Endpoint(0,1.1f));
}
