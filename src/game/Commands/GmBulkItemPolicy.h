#pragma once
#include <cstdint>
namespace GmBulkItemPolicy {
constexpr std::uint32_t MaximumCount=100000,MaximumAttachments=1000;
inline bool Count(std::uint32_t count){return count>=1&&count<=MaximumCount;}
inline std::uint32_t Attachments(std::uint32_t count,std::uint32_t stack){return stack?count/stack+(count%stack?1u:0u):MaximumAttachments+1;}
inline bool MailAllowed(std::uint32_t count,std::uint32_t stack,std::uint32_t unique,std::uint32_t existingAttachments){return Count(count)&&stack&&(!unique||count<=unique)&&existingAttachments<=MaximumAttachments&&Attachments(count,stack)<=MaximumAttachments-existingAttachments;}
}
