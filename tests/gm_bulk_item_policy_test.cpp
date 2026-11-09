#include "../src/game/Commands/GmBulkItemPolicy.h"
#include <cassert>
int main(){using namespace GmBulkItemPolicy;assert(!Count(0));assert(Count(100000));assert(!Count(100001));assert(Attachments(2000,20)==100);assert(MailAllowed(2000,20,0,0));assert(MailAllowed(20000,20,0,0));assert(!MailAllowed(20001,20,0,0));assert(!MailAllowed(1001,1,0,0));assert(!MailAllowed(2,20,1,0));assert(MailAllowed(1,1,1,999));assert(!MailAllowed(1,1,0,1000));assert(!MailAllowed(1,0,0,0));}
