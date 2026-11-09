#include "CharacterOverrides.h"
#include "Player.h"
#include "World.h"
#include "WorldSession.h"
#include "Config/Config.h"
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
namespace CharacterOverrides {
namespace {
struct Record { std::uint32_t account=0;std::uint64_t created=0;Limits limits; };
std::mutex mutex;std::map<std::uint32_t,Record> rows;bool loaded=false,valid=true;
std::string Path(){return sConfig.GetStringDefault("CharacterOverrides.File","character-overrides.tsv");}
void Load(){
    if(loaded)return;
    loaded=true;
    errno=0;std::ifstream in(Path());if(!in){valid=errno==ENOENT;return;}
    std::string line;while(std::getline(in,line)){
        if(line.empty()||line[0]=='#')continue;
        std::istringstream fields(line);std::uint32_t guid;Record record;auto& value=record.limits;std::string extra;
        if(!(fields>>guid>>record.account>>record.created>>value.talents>>value.professions)||fields>>extra||!guid||value.talents < -1||value.talents>300||value.professions < -1||value.professions>9||value.professions==0){valid=false;return;}
        if(!rows.emplace(guid,record).second){valid=false;return;}
    }
    if(in.bad())valid=false;
}
}
Limits Get(Player const* player){std::lock_guard<std::mutex> lock(mutex);Load();if(!valid)return Limits{};auto i=rows.find(player->GetGUIDLow());
    if(i==rows.end() || i->second.created!=player->GetCreationTime() || !player->GetSession() || i->second.account!=player->GetSession()->GetAccountId())return Limits{};
    return i->second.limits;}

std::uint32_t TalentPoints(Player const* player,std::uint32_t normal){auto v=Get(player);return v.talents<0?normal:std::uint32_t(v.talents);}
std::uint32_t ProfessionLimit(Player const* player){auto v=Get(player);return v.professions<0?sWorld.getConfig(CONFIG_UINT32_MAX_PRIMARY_TRADE_SKILL):std::uint32_t(v.professions);}
bool Set(Player const* player,Limits value,std::string& error){
    std::uint32_t guid=player->GetGUIDLow();
    std::lock_guard<std::mutex> lock(mutex);Load();
    if(!valid){error="Character override file is invalid or unreadable";return false;}
    if(!guid||value.talents < -1||value.talents>300||value.professions < -1||value.professions>9||value.professions==0){error="Invalid character limits";return false;}
    auto next=rows;if(value.talents==-1&&value.professions==-1)next.erase(guid);else {Record record;record.account=player->GetSession()->GetAccountId();record.created=player->GetCreationTime();record.limits=value;next[guid]=record;}
    std::string temp=Path()+".tmp";
    {std::ofstream out(temp,std::ios::trunc);if(!out){error="Cannot write character override file";return false;}
        out<<"# GUID account created talent_total(-1=default) primary_professions(-1=default)\n";
        for(auto const& row:next)out<<row.first<<'\t'<<row.second.account<<'\t'<<row.second.created<<'\t'<<row.second.limits.talents<<'\t'<<row.second.limits.professions<<'\n';
        out.flush();if(!out){error="Cannot save character override file";std::remove(temp.c_str());return false;}}
    if(std::rename(temp.c_str(),Path().c_str())!=0){error="Cannot replace character override file";std::remove(temp.c_str());return false;}
    rows.swap(next);return true;
}
unsigned ProfessionCount(Player const* player){unsigned count=0;for(unsigned id:{171,164,333,202,182,165,186,393,197})if(player->HasSkill(id))++count;return count;}
void RefreshProfessionPoints(Player* player){unsigned cap=ProfessionLimit(player),used=ProfessionCount(player);player->SetFreePrimaryProfessions(cap>used?cap-used:0);}
}
