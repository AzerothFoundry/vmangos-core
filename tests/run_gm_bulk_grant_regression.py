#!/usr/bin/env python3
from pathlib import Path
import tempfile,subprocess
r=Path(__file__).resolve().parents[1];s=(r/'src/game/Commands/GmUiCommands.cpp').read_text();a=s.index('        uint32 requested=uint32(count),noSpace=0;');b=s.index('\n    }',a);actual=s[a:b]
cpp=r"""
#include <vector>
#include <string>
#include <sstream>
#include <cassert>
using uint32=unsigned;constexpr unsigned NULL_BAG=0,NULL_SLOT=0,EQUIP_ERR_OK=0;
struct Dest{unsigned pos;};using ItemPosCountVec=std::vector<Dest>;
struct Item{bool bound=true;void SetBinding(bool b){bound=b;}static int GenerateItemRandomPropertyId(unsigned){return 0;}};
struct Player{bool full=false,unique=false;unsigned owned=0,storeCalls=0,saveCalls=0,sendCalls=0,capacityCalls=0;Item item;unsigned CanStoreNewItem(unsigned,unsigned,ItemPosCountVec& dest,unsigned,unsigned amount,unsigned* missing){++capacityCalls;if(full||unique){*missing=amount;return 1;}dest.push_back({1});return 0;}unsigned GetItemCount(unsigned,bool){return owned;}Item* StoreNewItem(ItemPosCountVec const&,unsigned,bool,int){++storeCalls;owned+=requested;return &item;}unsigned requested=2000;void SaveToDB(){++saveCalls;}Item*GetItemByPos(unsigned){return &item;}void SendNewItem(Item*,unsigned,bool,bool){++sendCalls;}};
struct Command{Player*self;unsigned count=2000,n=14047;std::vector<std::string> messages;bool error(char const*s){messages.push_back(s);return false;}bool ok(char const*s){messages.push_back(s);return true;}void SendSysMessage(char const*s){messages.push_back(s);}bool run(){
"""+actual+r"""
}};
int main(){Player p;Command c{&p};p.full=true;assert(!c.run());assert(p.capacityCalls==1&&p.storeCalls==0&&p.saveCalls==0&&p.sendCalls==0&&p.owned==0);p.full=false;p.unique=true;assert(!c.run());assert(p.storeCalls==0&&p.owned==0);p.unique=false;assert(c.run());assert(p.owned==2000&&p.storeCalls==1&&p.saveCalls==1&&p.sendCalls==1&&!p.item.bound);assert(c.messages[c.messages.size()-2].find("requested=2000|received=2000")!=std::string::npos);}
"""
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'case.cpp';exe=Path(d)/'case';p.write_text(cpp);subprocess.run(['c++','-std=c++17',str(p),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
mail=(r/'src/game/Mail/GmItemMail.cpp').read_text();prepare=mail[mail.index('bool Prepare('):mail.index('bool Deliver(')];assert prepare.index('MailAllowed')<prepare.index('Item::CreateItem');deliver=mail[mail.index('bool Deliver('):mail.index('bool SendTier(')];assert deliver.index('BeginTransaction')<deliver.index('item->SaveToDB')<deliver.index('CommitTransactionDirect');assert 'complete(committed)' in deliver
print('PASS actual self grant: full/unique zero mutation, complete counted saved grant; mail limit pre-creation and native atomic transaction retained')
