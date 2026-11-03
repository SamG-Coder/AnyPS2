#version 450
layout(set=0,binding=0) uniform usampler2D surfaces[2];
layout(location=0) out vec4 output_color;
struct Surface { uint base,pitch,x,y,width,height; };
layout(push_constant) uniform Display {
    Surface surfaces[2];
    uint width,height,control,background,half_height,bgra;
} display;
uint sample_surface(uint index,uint x,uint y) {
    Surface s=display.surfaces[index];
    ivec2 p=ivec2(s.x+x,s.y+(display.half_height!=0 ? y/2 : y));
    return (index==0 ? texelFetch(surfaces[0],p,0).r : texelFetch(surfaces[1],p,0).r)|0xff000000;
}
void main() {
    uint x=uint(gl_FragCoord.x),y=uint(gl_FragCoord.y);
    if (x>=display.width || y>=display.height) discard;
    uint enabled=display.control&3;
    uint color=display.background|0xff000000;
    if (enabled==3) {
        if ((display.control&128)==0 && x<display.surfaces[1].width && y<display.surfaces[1].height)
            color=sample_surface(1,x,y);
        if (x<display.surfaces[0].width && y<display.surfaces[0].height) {
            uint source=sample_surface(0,x,y),alpha=(display.control>>8)&255,blended=0xff000000;
            for (uint shift=0;shift<24;shift+=8)
                blended|=((((source>>shift)&255)*alpha+((color>>shift)&255)*(255-alpha)+127)/255)<<shift;
            color=blended;
        }
    } else color=sample_surface(enabled==2 ? 1 : 0,x,y);
    output_color=vec4(color&255,(color>>8)&255,(color>>16)&255,255)/255.0;
}
