#version 450
#extension GL_GOOGLE_include_directive : require
layout(set=0,binding=0) uniform usampler2D surfaces[2];
layout(location=0) out vec4 output_color;
#include "display_composition.glsl"
uint sample_surface(uint index,uint x,uint y) {
    Surface s=display.surfaces[index];
    x-=s.offset_x; y-=s.offset_y;
    ivec2 p=ivec2(s.x+x,s.y+(display.half_height!=0 ? y/2 : y));
    return (index==0 ? texelFetch(surfaces[0],p,0).r : texelFetch(surfaces[1],p,0).r)|0xff000000;
}
void main() {
    uint x=uint(gl_FragCoord.x),y=uint(gl_FragCoord.y);
    if (x>=display.width || y>=display.height) discard;
    uint color=compose_display(x,y);
    output_color=vec4(color&255,(color>>8)&255,(color>>16)&255,255)/255.0;
}
