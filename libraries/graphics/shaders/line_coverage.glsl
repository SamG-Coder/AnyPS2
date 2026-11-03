// Line coverage in 12.4 coordinates. Each fragment independently tests
// the major-axis interval and nearest minor coordinate; no CPU pixel expansion.
int64_t line_floor(int64_t n,int64_t d) { return n/d-((n%d)<0 ? 1 : 0); }
bool line_endpoint_exits(ivec2 delta,bool horizontal,int direction) {
    int distance=abs(delta.x)+abs(delta.y);
    int major=horizontal ? delta.x : delta.y;
    int minor=horizontal ? delta.y : delta.x;
    return direction*major>0 && (distance>8 || (distance==8 && minor>=0));
}
bool line_sample(ivec2 pixel,ivec2 a,ivec2 b,bool antialiased,out uint alpha,
                 out int64_t numerator,out int64_t denominator) {
    ivec2 delta=b-a;
    bool horizontal=abs(delta.x)>=abs(delta.y);
    int major_delta=horizontal ? delta.x : delta.y;
    int minor_delta=horizontal ? delta.y : delta.x;
    int direction=major_delta>0 ? 1 : -1;
    ivec2 ra=ivec2(line_floor(int64_t(a.x)+8,16),line_floor(int64_t(a.y)+8,16));
    ivec2 rb=ivec2(line_floor(int64_t(b.x)+8,16),line_floor(int64_t(b.y)+8,16));
    int first=horizontal ? ra.x : ra.y, last=horizontal ? rb.x : rb.y;
    if (line_endpoint_exits(a-ra*16,horizontal,direction)) first+=direction;
    if (!line_endpoint_exits(b-rb*16,horizontal,direction)) last-=direction;
    int major=horizontal ? pixel.x : pixel.y;
    if (direction*(major-first)<0 || direction*(last-major)<0) return false;
    denominator=abs(major_delta);
    numerator=int64_t((major*16-(horizontal ? a.x : a.y))*direction);
    int64_t minor=int64_t(horizontal ? a.y : a.x)*denominator+numerator*int64_t(minor_delta);
    int rounded=int(line_floor(minor+8*denominator,16*denominator));
    int sample_minor=horizontal ? pixel.y : pixel.x;
    alpha=128;
    if (!antialiased) return sample_minor==rounded;
    // Split coverage between the nearest pixel and its neighbor. Preserve the
    // public 16-bit coverage quantization before conversion to seven-bit alpha.
    int64_t displacement=minor-int64_t(rounded)*16*denominator;
    int neighbor=rounded+(displacement>=0 ? 1 : -1);
    if (sample_minor!=rounded && sample_minor!=neighbor) return false;
    uint fraction=uint((abs(displacement)*65535)/(16*denominator));
    alpha=(sample_minor==rounded ? 65535-fraction : fraction)>>9;
    return true;
}
uint line_value(uint a,uint b,int64_t numerator,int64_t denominator) {
    int64_t value=(int64_t(a)*(denominator-numerator)+int64_t(b)*numerator)/denominator;
    return uint(clamp(value,int64_t(0),int64_t(0xffffffffUL)));
}
uint line_color(uint a,uint b,int64_t numerator,int64_t denominator) {
    uint result=0;
    for (uint shift=0;shift<32;shift+=8)
        result|=min(255u,line_value((a>>shift)&255,(b>>shift)&255,numerator,denominator))<<shift;
    return result;
}
