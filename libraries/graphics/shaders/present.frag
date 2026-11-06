#version 450
#extension GL_GOOGLE_include_directive : require
layout(set=0,binding=0) uniform usampler2D surfaces[2];
layout(location=0) out vec4 output_color;
#include "display_composition.glsl"
// The output sample being composed, within its console pixel of a scaled surface.
uvec2 output_sample=uvec2(0u);
uint sample_surface(uint index,uint x,uint y) {
    Surface s=display.surfaces[index];
    x-=s.offset_x; y-=s.offset_y;
    // Half-height surfaces have one row per two display rows at every scale.
    const uint samples_y=(y*display.scale+output_sample.y)/(display.half_height!=0 ? 2u : 1u);
    ivec2 p=ivec2((s.x+x)*display.scale+output_sample.x,s.y*display.scale+samples_y);
    return (index==0 ? texelFetch(surfaces[0],p,0).r : texelFetch(surfaces[1],p,0).r)|0xff000000;
}
vec3 composed_sample(uvec2 position) {
    output_sample=position%display.scale;
    const uint color=compose_circuits(position.x/display.scale,position.y/display.scale);
    return vec3(color&255,(color>>8)&255,(color>>16)&255);
}
vec3 composed(uint x,uint y) {
    uint color=compose_display(x,y);
    vec3 rgb=vec3(color&255,(color>>8)&255,(color>>16)&255);
    if (display.smoothing!=0) {
        // Fixed vertical blend standing in for the temporal blend of a
        // suppressed sub-pixel offset cycle (DisplayPlan::smoothing).
        const uint above=compose_display(x,y>0 ? y-1 : 0),below=compose_display(x,min(y+1,display.height-1));
        const float w=float(display.smoothing)/256.0;
        rgb=rgb*(1.0-2.0*w)+(vec3(above&255,(above>>8)&255,(above>>16)&255)+vec3(below&255,(below>>8)&255,(below>>16)&255))*w;
    }
    return rgb;
}
void main() {
    uint x=uint(gl_FragCoord.x),y=uint(gl_FragCoord.y);
    vec3 rgb;
    if (display.scale>1u) {
        // Filter between surface samples, which supersamples when the window is smaller.
        const uvec2 samples=uvec2(display.width,display.height)*display.scale;
        const vec2 position=max((gl_FragCoord.xy-vec2(display.output_x,display.output_y))*vec2(samples)/vec2(display.output_width,display.output_height)-0.5,vec2(0.0));
        const uvec2 first=min(uvec2(position),samples-1u),last=min(first+1u,samples-1u);
        const vec2 weight=fract(position);
        rgb=mix(mix(composed_sample(first),composed_sample(uvec2(last.x,first.y)),weight.x),
                mix(composed_sample(uvec2(first.x,last.y)),composed_sample(last),weight.x),weight.y);
    } else if (display.output_width!=0) {
        // A resized digital mode filters between composed pixels, so uneven
        // scaling does not make rows and columns visibly different sizes.
        const vec2 position=max((gl_FragCoord.xy-vec2(display.output_x,display.output_y))*vec2(display.width,display.height)/vec2(display.output_width,display.output_height)-0.5,vec2(0.0));
        const uvec2 first=min(uvec2(position),uvec2(display.width,display.height)-1u);
        const uvec2 last=min(first+1u,uvec2(display.width,display.height)-1u);
        const vec2 weight=fract(position);
        rgb=mix(mix(composed(first.x,first.y),composed(last.x,first.y),weight.x),
                mix(composed(first.x,last.y),composed(last.x,last.y),weight.x),weight.y);
    } else {
        if (x>=display.width || y>=display.height) discard;
        rgb=composed(x,y);
    }
    output_color=vec4(rgb,255)/255.0;
}
