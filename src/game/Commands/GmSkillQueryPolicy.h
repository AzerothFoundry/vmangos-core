#pragma once
#include <string>
#include <vector>
namespace GmSkillQueryPolicy {
inline bool Forceable(bool admin,bool valid,bool known,bool unsafe){return admin&&valid&&!known&&!unsafe;}
inline std::string Clean(std::string text){for(char& c:text)if(c=='|'||c=='\t'||c=='\r'||c=='\n'||static_cast<unsigned char>(c)<32)c=' ';return text;}
inline std::string Clip(std::string const& text,std::size_t limit){if(text.size()<=limit)return text;std::size_t n=limit;while(n&&(static_cast<unsigned char>(text[n])&0xc0)==0x80)--n;return text.substr(0,n);}
inline std::vector<std::string> Parts(std::string text,std::size_t limit=180){std::vector<std::string> result;text=Clean(text);while(!text.empty()){auto part=Clip(text,limit);if(part.empty())break;result.push_back(part);text.erase(0,part.size());}if(result.empty())result.push_back("No effect description available");return result;}
}
