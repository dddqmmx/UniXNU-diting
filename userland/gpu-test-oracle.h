/* SPDX-License-Identifier: MIT */
#pragma once
#include <algorithm>
#include <cmath>
#include <stdint.h>
#include <string.h>
namespace DitingGpuOracle {
struct Color { float r, g, b, a; };
inline Color bars(unsigned x, unsigned y) {
    static constexpr Color palette[8] = {
        {1,0,0,1},{0,1,0,1},{0,0,1,1},{1,1,0,1},
        {0,1,1,1},{1,0,1,1},{1,1,1,1},{0,0,0,1}};
    return palette[(x/17 + y/11) % 8];
}
inline Color over(Color source, Color dest) {
    return {source.r*source.a+dest.r*(1-source.a), source.g*source.a+dest.g*(1-source.a),
            source.b*source.a+dest.b*(1-source.a), source.a+dest.a*(1-source.a)};
}
inline uint8_t unorm(float value, bool srgb = false) {
    double v = std::clamp(double(value), 0.0, 1.0);
    if (srgb) v = v <= 0.0031308 ? v*12.92 : 1.055*std::pow(v,1.0/2.4)-0.055;
    return uint8_t(std::lround(v*255));
}
inline void bytes(Color color, uint8_t out[4], bool bgra, bool srgb) {
    out[bgra?2:0] = unorm(color.r,srgb); out[1] = unorm(color.g,srgb);
    out[bgra?0:2] = unorm(color.b,srgb); out[3] = unorm(color.a);
}
inline float half(uint16_t raw) {
    const float sign = raw & 0x8000 ? -1 : 1;
    const unsigned exponent = (raw >> 10) & 31, fraction = raw & 1023;
    if (!exponent) return sign*std::ldexp(float(fraction),-24);
    if (exponent == 31) return fraction ? NAN : sign*INFINITY;
    return sign*std::ldexp(float(1024+fraction),int(exponent)-25);
}
inline uint32_t compute(uint32_t index) { return (index*1664525u + 1013904223u) ^ 0x5a5a5a5au; }
}
