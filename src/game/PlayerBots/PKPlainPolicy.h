#pragma once
#include <cmath>
namespace PKPlainPolicy {
inline bool Height(float center,float floor,float actual){return std::isfinite(center)&&std::isfinite(floor)&&std::isfinite(actual)&&std::fabs(floor-center)<=3.f&&std::fabs(actual-floor)<=1.5f;}
inline bool Neighbor(float floor,float sample){return std::isfinite(sample)&&std::fabs(sample-floor)<=1.2f;}
inline bool Endpoint(float xy,float z){return std::isfinite(xy)&&std::isfinite(z)&&xy<=1.f&&std::fabs(z)<=1.f;}
}
