#version 450
#extension GL_ARB_gpu_shader_int64 : require
struct Vertex { int x,y; uint color,u,v; float s,t,q; uint z; };
struct DrawData {
    uint left,top,right,bottom,base,pitch,color,mask;
    uint texture_base,texture_pitch,texture_width,texture_height;
    uint flags,clamp_u,clamp_v,texa;
    int x0,y0,dx,dy,u0,v0,du,dv;
    uint min_u,max_u,min_v,max_v;
    uint test,blend;
    uint depth_base,depth;
    Vertex vertices[3];
    uint target_width,target_height;
};
layout(std430,set=0,binding=3) readonly buffer DrawRecords { DrawData items[]; } records;

layout(location=0) flat out uint draw_index;
#define draw records.items[gl_InstanceIndex]
void main() {
    draw_index=gl_InstanceIndex;
    vec2 p;
    if ((draw.flags&16384)!=0) {
        Vertex v=draw.vertices[gl_VertexIndex];
        p=vec2(v.x,v.y)/16.0;
        if ((draw.flags&1048576)==0) p+=vec2(0.5);
    } else {
        const ivec2 corners[6]=ivec2[](ivec2(0,0),ivec2(1,0),ivec2(0,1),ivec2(0,1),ivec2(1,0),ivec2(1,1));
        p=mix(vec2(draw.left,draw.top),vec2(draw.right,draw.bottom),vec2(corners[gl_VertexIndex]));
    }
    gl_Position=vec4(p/vec2(draw.target_width,draw.target_height)*2.0-1.0,0.0,1.0);
}
