struct Surface { uint base,pitch,x,y,width,height,offset_x,offset_y; };
layout(push_constant) uniform Display {
    Surface surfaces[2];
    uint width,height,control,background,half_height,bgra,field,history_valid,smoothing;
} display;
uint sample_surface(uint index,uint x,uint y);
bool contains_surface(uint index,uint x,uint y) {
    Surface s=display.surfaces[index];
    return x>=s.offset_x && y>=s.offset_y && x-s.offset_x<s.width && y-s.offset_y<s.height;
}
uint compose_circuits(uint x,uint y) {
    uint enabled=display.control&3;
    uint color=display.background|0xff000000;
    if (enabled==3 || (enabled==1 && (display.control&32)!=0)) {
        if ((enabled&2)!=0 && (display.control&128)==0 && contains_surface(1,x,y))
            color=sample_surface(1,x,y);
        if (contains_surface(0,x,y)) {
            uint source=sample_surface(0,x,y),alpha=(display.control>>8)&255,blended=0xff000000;
            for (uint shift=0;shift<24;shift+=8)
                blended|=((((source>>shift)&255)*alpha+((color>>shift)&255)*(255-alpha)+127)/255)<<shift;
            color=blended;
        }
    } else {
        uint index=enabled==2 ? 1 : 0;
        if (contains_surface(index,x,y)) color=sample_surface(index,x,y);
    }
    return color;
}
uint compose_display(uint x,uint y) {
    if (display.field==0) return compose_circuits(x,y);
    uint phase=display.field==2 ? 1:0;
    uint row=(y>phase ? y-phase : 0)/2*2;
    uint color=compose_circuits(x,row);
    if (y>=phase && ((y-phase)&1)!=0) {
        uint next=compose_circuits(x,min(row+2,display.height-1)),blended=0xff000000;
        for (uint shift=0;shift<24;shift+=8)
            blended|=(((((color>>shift)&255)+((next>>shift)&255)+1)/2)<<shift);
        color=blended;
    }
    return color;
}
