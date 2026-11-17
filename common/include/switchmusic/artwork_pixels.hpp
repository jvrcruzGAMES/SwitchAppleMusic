#pragma once
#include <cstdint>
#include <vector>
#include <stdexcept>
namespace smusic {
// Backend/libultrahand wire layout: RG, BA, high nibble first. NanoVG needs
// ordinary RGBA8 bytes, including a fully expanded alpha channel.
inline std::vector<uint8_t> artworkRGBA8(const std::vector<uint8_t>& source,unsigned size,unsigned target){
    if((size!=64&&size!=256)||(target!=64&&target!=256)||target>size||source.size()!=size_t(size)*size*2)
        throw std::runtime_error("Invalid artwork pixel dimensions");
    std::vector<uint8_t> pixels(size_t(target)*target*4);
    for(unsigned y=0;y<target;++y)for(unsigned x=0;x<target;++x){
        auto in=(size_t(y*size/target)*size+x*size/target)*2,out=(size_t(y)*target+x)*4;
        pixels[out]=(source[in]>>4)*17;pixels[out+1]=(source[in]&15)*17;
        pixels[out+2]=(source[in+1]>>4)*17;pixels[out+3]=(source[in+1]&15)*17;
    }
    return pixels;
}
}
