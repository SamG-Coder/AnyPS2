#include <nt/pad_script.hpp>
#include <iostream>
#include <string>

namespace {
int failures=0;
void check(bool value,const char* what) {
    if (!value) { std::cerr<<"pad script: "<<what<<'\n'; ++failures; }
}
}

int main() {
    nt::input::PadScript script;
    const auto error=script.parse("# wait for the title\n1000 50 start\r\n  2000 100 cross+up # combined\n2050 100 circle\n");
    check(error.empty(),"valid script rejected");
    check(script.presses().size()==3,"press count");
    check(script.end_ms()==2150,"end time");
    check(script.sample(0).connected && script.sample(0).buttons==0,"idle before the first press");
    check(script.sample(1000).buttons==(1U<<3),"start pressed at its start time");
    check(script.sample(1049).buttons==(1U<<3),"start held through its hold");
    check(script.sample(1050).buttons==0,"start released after its hold");
    check(script.sample(2060).buttons==((1U<<14)|(1U<<4)|(1U<<13)),"overlapping presses combine");
    check(script.sample(2150).buttons==0,"all released at the end");

    const auto rejected=[&](const char* text,const char* fragment,const char* what) {
        nt::input::PadScript bad;
        const auto message=bad.parse(text);
        check(message.find(fragment)!=std::string::npos,what);
    };
    rejected("","no presses","empty script accepted");
    rejected("100 50 jump\n","line 1","unknown button accepted");
    rejected("100 0 start\n","nonzero hold","zero hold accepted");
    rejected("x 50 start\n","line 1","non-numeric time accepted");
    rejected("100 50 start+\n","unknown button","trailing separator accepted");
    rejected("100 50 start extra\n","unexpected text","trailing text accepted");
    rejected("# only\n18446744073709551615 2 start\n","line 2: time overflows","overflowing time accepted");
    std::string many;
    for (unsigned i=0;i<=nt::input::PadScript::max_events;++i) many+="0 1 start\n";
    rejected(many.c_str(),"more than","unbounded script accepted");
    return failures ? 1 : 0;
}
