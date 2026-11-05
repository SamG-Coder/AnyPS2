#version 450
#extension GL_ARB_gpu_shader_int64 : require
#extension GL_GOOGLE_include_directive : require
#include "line_coverage.glsl"
#ifdef ANYPS2_ORDERED_FRAGMENTS
#extension GL_ARB_fragment_shader_interlock : require
layout(pixel_interlock_ordered) in;
#endif
struct Vertex { int x,y; uint color,u,v; float s,t,q; uint z,fog; };
struct DrawData {
    uint left,top,right,bottom,base,pitch,color,mask;
    uint texture_base,texture_pitch,texture_width,texture_height;
    uint flags,clamp_u,clamp_v,texa;
    int x0,y0,dx,dy,u0,v0,du,dv;
    uint min_u,max_u,min_v,max_v;
    uint test,blend;
    uint depth_base,depth;
    Vertex vertices[3];
    uint target_width,target_height,fog_color,fog;
    uint lod_control; float lod_q;
    // Levels with bit set in indexed_levels are decoded by native.frag;
    // level_buffers holds MIPTBP levels 1-6 as base | pitch << 16.
    uint palette,palette_bank,indexed_format,indexed_levels;
    uint level_buffers[6]; uint reserved0,reserved1;
};
layout(std430,set=0,binding=3) readonly buffer DrawRecords { DrawData items[]; } records;

layout(location=0) flat in uint draw_index;
#define draw records.items[draw_index]
layout(set=0,binding=0) uniform usampler2D texture_levels[7];
// Indexed level-0 textures are decoded here: index data comes from the GPU
// copy of console memory and colors from a GPU palette slot (palette.comp's
// layout), so no separate decode pass runs per draw.
layout(std430,set=0,binding=4) readonly buffer Mirror { uint words[]; } mirror;
layout(std430,set=0,binding=5) readonly buffer Palettes { uint words[]; } palettes;
// Palettes known on the CPU, written by the host per draw (palette bit 31 set).
layout(std430,set=0,binding=6) readonly buffer HostPalettes { uint words[]; } host_palettes;
uint rgba_word(uint base,uint pitch,uint x,uint y) {
    x&=2047u; y&=2047u;
    const uint page=(y/32u)*pitch+x/64u;
    const uint block=((x>>3)&1u)|(((y>>3)&1u)<<1)|(((x>>4)&1u)<<2)|(((y>>4)&1u)<<3)|(((x>>5)&1u)<<4);
    const uint word=(x&1u)|((y&1u)<<1)|((x&6u)<<1)|((y&6u)<<3);
    return (base*64u+page*2048u+block*64u+word)&0xfffffu;
}
uint indexed_byte(uint base,uint pitch,uint x,uint y) {
    x&=2047u; y&=2047u;
    const uint page=(y/64u)*(pitch/2u)+x/128u;
    const uint block=((x>>4)&1u)|(((y>>4)&1u)<<1)|(((x>>5)&1u)<<2)|(((y>>5)&1u)<<3)|(((x>>6)&1u)<<4);
    const uint column=((x&1u)<<2)|((x&2u)<<3)|((((x>>2)^(y>>1)^(y>>2))&1u)<<5)|
        ((x&8u)>>2)|((y&1u)<<3)|((y&2u)>>1)|((y&4u)<<4)|((y&8u)<<4);
    return (base*256u+page*8192u+block*256u+column)&0x3fffffu;
}
uint indexed_nibble(uint base,uint pitch,uint x,uint y) {
    x&=2047u; y&=2047u;
    const uint page=(y/128u)*(pitch/2u)+x/128u;
    const uint block=((y>>4)&1u)|(((x>>5)&1u)<<1)|(((y>>5)&1u)<<2)|(((x>>6)&1u)<<3)|(((y>>6)&1u)<<4);
    const uint column=((x&1u)<<3)|((x&2u)<<4)|((((x>>2)^(y>>1)^(y>>2))&1u)<<6)|
        ((x&8u)>>2)|((x&16u)>>2)|((y&1u)<<4)|((y&2u)>>1)|((y&4u)<<5)|((y&8u)<<5);
    return (base*512u+page*16384u+block*512u+column)&0x7fffffu;
}
// indexed_format: 1 PSMT8, 2 PSMT4, 3 PSMT8H, 4 PSMT4HL, 5 PSMT4HH; 6 PSMCT32
// and 7 PSMCT32 in the depth page layout read memory without a palette.
uint indexed_texel(uint level,ivec2 p) {
    const uint format=draw.indexed_format-1u,x=uint(p.x),y=uint(p.y);
    const uint base=level==0u ? draw.texture_base : draw.level_buffers[level-1u]&0xffffu;
    const uint pitch=level==0u ? draw.texture_pitch : draw.level_buffers[level-1u]>>16;
    uint word,shift,mask=255u;
    if (format>=5u) return mirror.words[rgba_word(base,pitch,x,y)^(format==6u ? 0x600u : 0u)];
    if (format==0u) { const uint b=indexed_byte(base,pitch,x,y); word=b/4u; shift=(b%4u)*8u; }
    else if (format==1u) { const uint n=indexed_nibble(base,pitch,x,y); word=n/8u; shift=(n%8u)*4u; mask=15u; }
    else { word=rgba_word(base,pitch,x,y); shift=format==4u ? 28u : 24u; mask=format==2u ? 255u : 15u; }
    const uint index=(mirror.words[word&0xfffffu]>>shift)&mask;
    // nt::graphics::snapshot_palette for 32-bit palettes: CSA selects the bank;
    // eight-bit indices keep their low four bits within a clamped bank.
    const uint entry=mask==255u ? min(draw.palette_bank+index/16u,15u)*16u+(index&15u) : draw.palette_bank*16u+index;
    return draw.palette>=0x80000000u ? host_palettes.words[(draw.palette&0x7fffffffu)*256u+entry] : palettes.words[draw.palette*256u+entry];
}
#ifdef ANYPS2_ORDERED_FRAGMENTS
layout(r32ui,set=0,binding=1) coherent uniform uimage2D color_image;
layout(r32ui,set=0,binding=2) coherent uniform uimage2D depth_image;
uint output_color,output_depth;
uint read_color() { return imageLoad(color_image,ivec2(gl_FragCoord.xy)).r; }
uint read_depth() { return imageLoad(depth_image,ivec2(gl_FragCoord.xy)).r; }
#else
layout(input_attachment_index=0,set=0,binding=1) uniform usubpassInput previous_color;
layout(input_attachment_index=1,set=0,binding=2) uniform usubpassInput previous_depth;
layout(location=0) out uint output_color;
layout(location=1) out uint output_depth;
uint read_color() { return subpassLoad(previous_color).r; }
uint read_depth() { return subpassLoad(previous_depth).r; }
#endif
int floor_div(int n,int d) {int q=n/d;return q-((n%d)!=0 && ((n<0)!=(d<0)) ? 1:0);}
uint wrap(int t,uint size,uint mode,uint low,uint high) {
    if(mode==0) return uint(t)&(size-1);
    if(mode==1) return uint(clamp(t,0,int(size)-1));
    if(mode==2) return uint(clamp(t,int(low),int(high)));
    return (uint(t)&low)|high;
}
// Constant indices keep level selection valid without descriptor indexing.
uint level_texel(uint level,ivec2 p) {
    if (((draw.indexed_levels>>level)&1u)!=0u) return indexed_texel(level,p);
    switch (level) {
    case 0: return texelFetch(texture_levels[0],p,0).r;
    case 1: return texelFetch(texture_levels[1],p,0).r;
    case 2: return texelFetch(texture_levels[2],p,0).r;
    case 3: return texelFetch(texture_levels[3],p,0).r;
    case 4: return texelFetch(texture_levels[4],p,0).r;
    case 5: return texelFetch(texture_levels[5],p,0).r;
    default: return texelFetch(texture_levels[6],p,0).r;
    }
}
uint texel_at(uint level,int u,int v) {
        uint tx=wrap(u,max(draw.texture_width>>level,1u),draw.clamp_u,draw.min_u>>level,draw.max_u>>level);
        uint ty=wrap(v,max(draw.texture_height>>level,1u),draw.clamp_v,draw.min_v>>level,draw.max_v>>level);
        uint texel=level_texel(level,ivec2(tx,ty));
        if((draw.flags&2)!=0) {
            texel &= 0xffffff;
            uint alpha=((draw.texa&0x8000)!=0 && texel==0) ? 0 : (draw.texa&255);
            texel |= alpha<<24;
        }
        return texel;
}
uint interpolate(uint a,uint b,uint fraction) {
    uint result=0;
    for(uint channel=0;channel<4;++channel) {
        uint av=(a>>(channel*8))&255, bv=(b>>(channel*8))&255;
        result |= ((av*(16-fraction)+bv*fraction)>>4)<<(channel*8);
    }
    return result;
}
// uf/vf are level-0 texel coordinates with four fractional bits.
uint sample_level(uint level,int uf,int vf,bool linear) {
    uf>>=level; vf>>=level;
    if(linear) {uf-=8;vf-=8;}
    int u=floor_div(uf,16), v=floor_div(vf,16);
    uint texel=texel_at(level,u,v);
    if(linear) {
        uint fu=uint(uf-u*16), fv=uint(vf-v*16);
        uint top=interpolate(texel,texel_at(level,u+1,v),fu);
        uint bottom=interpolate(texel_at(level,u,v+1),texel_at(level,u+1,v+1),fu);
        texel=interpolate(top,bottom,fv);
    }
    return texel;
}
// GS LOD = K for LCM=1, otherwise -log2(|Q|)*2^L+K. LOD <= 0 magnifies level 0.
// The level position uses 16 fractional bits as in the pinned reference;
// adjacent-level blending uses its top 8 fractional bits.
uint sample_texture(int uf,int vf,float q) {
    uint control=draw.lod_control;
    if ((control&1u)==0) return sample_level(0,uf,vf,(draw.flags&128)!=0);
    float k=float(int(control<<4)>>20)/16.0;
    float lod=(control&128u)!=0 ? k : -log2(abs(q))*float(1u<<((control>>8)&3u))+k;
    if (!(lod>0.0)) return sample_level(0,uf,vf,(draw.flags&128)!=0);
    bool linear=(control&16u)!=0;
    uint levels=(control>>1)&7u,mode=(control>>5)&3u;
    uint position=uint(min(lod,float(levels))*65536.0);
    if (mode==0) return sample_level(0,uf,vf,linear);
    if (mode==1) return sample_level(min((position+32768u)>>16,levels),uf,vf,linear);
    uint level=position>>16,fraction=(position>>8)&255u;
    uint near=sample_level(level,uf,vf,linear);
    if (level>=levels || fraction==0) return near;
    uint far=sample_level(level+1,uf,vf,linear),result=0;
    for(uint channel=0;channel<4;++channel) {
        uint a=(near>>(channel*8))&255,b=(far>>(channel*8))&255;
        result|=((a*(256-fraction)+b*fraction)>>8)<<(channel*8);
    }
    return result;
}
bool alpha_pass(uint alpha) {
    uint mode=(draw.test>>1)&7,reference=(draw.test>>4)&255;
    if(mode==0) return false;
    if(mode==1) return true;
    if(mode==2) return alpha<reference;
    if(mode==3) return alpha<=reference;
    if(mode==4) return alpha==reference;
    if(mode==5) return alpha>=reference;
    if(mode==6) return alpha>reference;
    return alpha!=reference;
}
int blend_channel(uint select,int source,int destination) {
    return select==0 ? source : select==1 ? destination : 0;
}
ivec2 vertex_position(uint index) { return ivec2(draw.vertices[index].x,draw.vertices[index].y); }
vec3 vertex_stq(uint index) { return vec3(draw.vertices[index].s,draw.vertices[index].t,draw.vertices[index].q); }
int64_t edge(ivec2 a,ivec2 b,ivec2 p) {
    return int64_t(b.x-a.x)*int64_t(p.y-a.y)-int64_t(b.y-a.y)*int64_t(p.x-a.x);
}
bool covered(int64_t weight,ivec2 a,ivec2 b) {
    bool inclusive=b.y<a.y || (b.y==a.y && b.x>a.x);
    return weight>0 || (weight==0 && inclusive);
}
// Colour and affine texture interpolation normally have small quotients.
// Float division proposes a quotient; integer checks establish the exact
// result. Unusual ranges or estimates retain the full integer division path.
uint64_t divide_small(uint64_t numerator,uint64_t denominator) {
    float estimate=float(numerator)/float(denominator);
    if (!(estimate>=0.0 && estimate<65535.0)) return numerator/denominator;
    uint64_t quotient=uint64_t(estimate);
    // Check multiplication range before forming the product.
    if (denominator>uint64_t(0x0000ffffffffffffUL)) return numerator/denominator;
    uint64_t product=quotient*denominator;
    if (product>numerator) {
        if (quotient==0) return numerator/denominator;
        --quotient; product-=denominator;
    } else if (numerator-product>=denominator) {
        ++quotient; product+=denominator;
    }
    if (product>numerator || numerator-product>=denominator) return numerator/denominator;
    return quotient;
}
// The critical section contains only ordered colour/depth tests and writes.
// Texture sampling and interpolation remain parallel outside it.
bool commit_pixel(uint color,uint depth,uint mask,bool write_depth) {
    output_depth=read_depth();
    uint destination=0;
    if((draw.flags&256)!=0 || (draw.test&0x4000)!=0) destination=read_color();
    if((draw.test&0x4000)!=0 && (draw.flags&512)==0 &&
        (destination>>31)!=((draw.test>>15)&1)) return false;
    if ((draw.flags&131072)!=0) {
        uint method=(draw.test>>17)&3;
        if (method==0) return false;
        uint depth_mask=(draw.flags&524288)!=0 ? 0xffffff : 0xffffffff;
        depth&=depth_mask;
        uint previous=output_depth&depth_mask;
        if ((method==2 && depth<previous) || (method==3 && depth<=previous)) return false;
        if (write_depth) {
            output_depth=(output_depth&~depth_mask)|depth;
        }
    }
    if((draw.flags&256)!=0 && ((draw.flags&1024)==0 || (color&0x80000000)!=0)) {
        uint a=draw.blend&3,b=(draw.blend>>2)&3,c=(draw.blend>>4)&3,d=(draw.blend>>6)&3;
        int factor=int(c==0 ? color>>24 : c==1 ? ((draw.flags&512)!=0 ? 128 : destination>>24) : (draw.blend>>8)&255);
        uint blended=color&0xff000000;
        for(uint channel=0;channel<3;++channel) {
            int source=int((color>>(channel*8))&255),dest=int((destination>>(channel*8))&255);
            int value=floor_div((blend_channel(a,source,dest)-blend_channel(b,source,dest))*factor,128)+blend_channel(d,source,dest);
            uint result=(draw.flags&2048)!=0 ? uint(value)&255 : uint(clamp(value,0,255));
            blended|=result<<(channel*8);
        }
        color=blended;
    }
    if((draw.flags&32)!=0) color |= 0x80000000;
    output_color=(read_color()&mask)|(color&~mask);
    if ((draw.flags&134217728)!=0) output_color&=0x80f8f8f8;
#ifdef ANYPS2_ORDERED_FRAGMENTS
    imageStore(color_image,ivec2(gl_FragCoord.xy),uvec4(output_color));
    if (write_depth && (draw.flags&131072)!=0)
        imageStore(depth_image,ivec2(gl_FragCoord.xy),uvec4(output_depth));
#endif
    return true;
}
void main() {
    uint x=uint(gl_FragCoord.x), y=uint(gl_FragCoord.y);
    if ((draw.flags&4096)!=0 && (y&1)==((draw.flags>>13)&1)) discard;
    if (x<draw.left || y<draw.top || x>=draw.right || y>=draw.bottom) discard;
    uint color=draw.color;
    uint depth=draw.depth,coverage=16,line_alpha=128;
    uint64_t fog_weight=uint64_t(draw.fog),fog_area=uint64_t(1);
    bool triangle_interior=true;
    int uf=0,vf=0;
    float lod_q=draw.lod_q;
    if ((draw.flags&16777216)!=0) {
        int64_t numerator,denominator;
        bool aa=(draw.flags&1048576)!=0;
        if (!line_sample(ivec2(x,y),vertex_position(0),vertex_position(1),aa,line_alpha,numerator,denominator)) discard;
        if (aa) triangle_interior=false; // Coverage edges test depth but do not replace it.
        depth=line_value(draw.vertices[0].z,draw.vertices[1].z,numerator,denominator);
        if ((draw.flags&32768)!=0)
            color=line_color(draw.vertices[0].color,draw.vertices[1].color,numerator,denominator);
    }
    if ((draw.flags&16384)!=0) {
        ivec2 a=vertex_position(0),b=vertex_position(1),c=vertex_position(2),p=ivec2(x,y)*16;
        int64_t w0=edge(b,c,p),w1=edge(c,a,p),w2=edge(a,b,p);
        triangle_interior=covered(w0,b,c) && covered(w1,c,a) && covered(w2,a,b);
        if ((draw.flags&1048576)!=0) {
            if (!triangle_interior) {
                // Native 4x4 coverage sampling. Interpolate at the covered-sample
                // centroid under the supported coverage rule.
                // An affine edge reaches its minimum over the sample rectangle at
                // a corner. Fully covered pixels have the exact sample centroid
                // (8,8), so they do not need sixteen repeated edge evaluations.
                // Move each linear edge to the sample rectangle corner where it
                // is minimal. No sampling or floating-point approximation needed.
                ivec2 e0=c-b,e1=a-c,e2=b-a;
                int64_t min0=w0+int64_t(-e0.y)*int64_t(e0.y>0 ? 14 : 2)+int64_t(e0.x)*int64_t(e0.x<0 ? 14 : 2);
                int64_t min1=w1+int64_t(-e1.y)*int64_t(e1.y>0 ? 14 : 2)+int64_t(e1.x)*int64_t(e1.x<0 ? 14 : 2);
                int64_t min2=w2+int64_t(-e2.y)*int64_t(e2.y>0 ? 14 : 2)+int64_t(e2.x)*int64_t(e2.x<0 ? 14 : 2);
                bool interior=covered(min0,b,c) && covered(min1,c,a) && covered(min2,a,b);
                if (interior) {
                    coverage=16;
                    w0=edge(b,c,p+ivec2(8)); w1=edge(c,a,p+ivec2(8)); w2=edge(a,b,p+ivec2(8));
                } else {
                    coverage=0; w0=0; w1=0; w2=0;
                    for (int sy=2;sy<16;sy+=4) for (int sx=2;sx<16;sx+=4) {
                        ivec2 position=p+ivec2(sx,sy);
                        int64_t s0=edge(b,c,position),s1=edge(c,a,position),s2=edge(a,b,position);
                        if (covered(s0,b,c) && covered(s1,c,a) && covered(s2,a,b)) {
                            ++coverage; w0+=s0; w1+=s1; w2+=s2;
                        }
                    }
                    if (coverage==0) discard;
                    w0/=int64_t(coverage); w1/=int64_t(coverage); w2/=int64_t(coverage);
                }
                // Exterior coverage must not replace interior depth/opacity.
                coverage=min(coverage,15u);
            }
        } else if (!covered(w0,b,c) || !covered(w1,c,a) || !covered(w2,a,b)) discard;
        int64_t area=w0+w1+w2;
        if ((draw.flags&67108864)!=0) {
            fog_area=uint64_t(area);
            fog_weight=uint64_t(draw.vertices[0].fog)*uint64_t(w0)+
                uint64_t(draw.vertices[1].fog)*uint64_t(w1)+uint64_t(draw.vertices[2].fog)*uint64_t(w2);
        }
        if ((draw.flags&131072)!=0)
            depth=uint((uint64_t(draw.vertices[0].z)*uint64_t(w0)+
                uint64_t(draw.vertices[1].z)*uint64_t(w1)+uint64_t(draw.vertices[2].z)*uint64_t(w2))/uint64_t(area));
        if ((draw.flags&1)!=0) {
            if ((draw.lod_control&1u)!=0) {
                vec3 weights=vec3(float(w0),float(w1),float(w2))/float(area);
                lod_q=draw.vertices[0].q*weights.x+draw.vertices[1].q*weights.y+draw.vertices[2].q*weights.z;
            }
            if ((draw.flags&65536)!=0) {
                vec3 weights=vec3(float(w0),float(w1),float(w2))/float(area);
                vec3 stq=vertex_stq(0)*weights.x+vertex_stq(1)*weights.y+vertex_stq(2)*weights.z;
                uf=int(floor(stq.x/stq.z*float(draw.texture_width)*16.0));
                vf=int(floor(stq.y/stq.z*float(draw.texture_height)*16.0));
            } else {
                uf=int(divide_small(uint64_t(int64_t(draw.vertices[0].u)*w0+int64_t(draw.vertices[1].u)*w1+int64_t(draw.vertices[2].u)*w2),uint64_t(area)));
                vf=int(divide_small(uint64_t(int64_t(draw.vertices[0].v)*w0+int64_t(draw.vertices[1].v)*w1+int64_t(draw.vertices[2].v)*w2),uint64_t(area)));
            }
        }
        if ((draw.flags&32768)!=0) color=draw.vertices[0].color;
        if ((draw.flags&32768)!=0 && (draw.vertices[0].color!=draw.vertices[1].color || draw.vertices[1].color!=draw.vertices[2].color)) {
            uint ca=draw.vertices[0].color,cb=draw.vertices[1].color,cc=draw.vertices[2].color;
            color=0;
            for (uint channel=0;channel<4;++channel) {
                int64_t sum=int64_t((ca>>(8*channel))&255)*w0+
                    int64_t((cb>>(8*channel))&255)*w1+int64_t((cc>>(8*channel))&255)*w2;
                color|=uint(divide_small(uint64_t(sum),uint64_t(area)))<<(8*channel);
            }
        }
    }
    if ((draw.flags&1)!=0) {
        if ((draw.flags&16384)==0) {
            uf=draw.u0+floor_div((int(x)*16-draw.x0)*draw.du,draw.dx);
            vf=draw.v0+floor_div((int(y)*16-draw.y0)*draw.dv,draw.dy);
        }
        uint texel=sample_texture(uf,vf,lod_q);
        uint function=(draw.flags>>3)&3;
        uint vertex_color=color;
        color=0;
        for(uint channel=0;channel<4;++channel) {
            uint t=(texel>>(channel*8))&255, c=(vertex_color>>(channel*8))&255;
            uint result=function==1 ? t : (t*c)>>7;
            if(function>=2) result=channel==3 ? t+(function==2 ? vertex_color>>24 : 0) : result+(vertex_color>>24);
            if(channel==3 && (draw.flags&4)==0) result=c;
            result=min(result,255);
            color |= result<<(channel*8);
        }
    }
    if ((draw.flags&67108864)!=0) {
        const uint64_t scale=uint64_t(256)*fog_area;
        uint result=color&0xff000000;
        for (uint channel=0;channel<3;++channel) {
            const uint source=(color>>(channel*8))&255, background=(draw.fog_color>>(channel*8))&255;
            result|=uint(divide_small(uint64_t(source)*fog_weight+uint64_t(background)*(scale-fog_weight),scale))<<(channel*8);
        }
        color=result;
    }
    if ((draw.flags&1048576)!=0 && ((draw.flags&2097152)==0 || (color>>24)==128))
        color=(color&0xffffff)|(((draw.flags&16777216)!=0 ? line_alpha : coverage*8)<<24);
    uint mask=draw.mask;
    bool write_depth=(draw.flags&262144)!=0 && triangle_interior;
    if((draw.test&1)!=0 && !alpha_pass(color>>24)) {
        uint failure=(draw.test>>12)&3;
        if(failure==0) discard;
        if(failure==1 || failure==3) write_depth=false;
        if(failure==2) mask=0xffffffff;
        if(failure==3) mask|=0xff000000;
    }
#ifdef ANYPS2_ORDERED_FRAGMENTS
    beginInvocationInterlockARB();
#endif
    bool written=commit_pixel(color,depth,mask,write_depth);
#ifdef ANYPS2_ORDERED_FRAGMENTS
    endInvocationInterlockARB();
#endif
    if (!written) discard;
}
