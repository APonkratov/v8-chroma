#pragma once
#include <stdint.h>
#include <math.h>
#include <initializer_list>

namespace ChromaContrast {
inline double luminance(uint32_t color) {
    auto channel=[&](unsigned shift) {
        double value=((color>>shift)&255)/255.;
        return value<=.04045?value/12.92:pow((value+.055)/1.055,2.4);
    };
    return .2126*channel(16)+.7152*channel(8)+.0722*channel(0);
}
inline double ratio(uint32_t left,uint32_t right) {
    double a=luminance(left),b=luminance(right);
    return (fmax(a,b)+.05)/(fmin(a,b)+.05);
}
// Preserve the syntax hue as far as possible; the validated base text is the
// fallback for the changed-block fill. The public palette remains untouched.
inline uint32_t readableInk(uint32_t ink,uint32_t background,uint32_t fallback) {
    for(unsigned amount=0;amount<=100;++amount) {
        uint32_t color=0;
        for(unsigned shift:{16u,8u,0u})
            color|=(((((ink>>shift)&255)*(100-amount)+((fallback>>shift)&255)*amount+50)/100)<<shift);
        if(ratio(color,background)>=4.5) return color;
    }
    return fallback;
}
}
