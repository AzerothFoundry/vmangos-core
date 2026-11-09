#include "WorldBotCorpsePolicy.h"
#include <cassert>
#include <iostream>
int main(){using WorldBotCorpsePolicy::LegalApproach;
assert(LegalApproach(4,26,4.2f,39));assert(LegalApproach(4,18,1,39));assert(LegalApproach(1,2,37,39));
assert(!LegalApproach(4,26,38,39));assert(!LegalApproach(8,26,4,39));assert(!LegalApproach(2,26,4,39));assert(!LegalApproach(4,1,4,39));assert(!LegalApproach(4,26,NAN,39));
std::cout<<"PASS production corpse approach: actual partial endpoints, native radius margin, reject shortcut/no-path/invalid endpoints\n";}
