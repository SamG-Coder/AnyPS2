#include <nt/pad_script.hpp>
#include <algorithm>
#include <array>
#include <charconv>

namespace nt::input {
namespace {
constexpr std::array<std::string_view,16> button_names{
    "select","l3","r3","start","up","right","down","left",
    "l2","r2","l1","r1","triangle","circle","cross","square"};
std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front()==' ' || text.front()=='\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back()==' ' || text.back()=='\t' || text.back()=='\r')) text.remove_suffix(1);
    return text;
}
std::string_view field(std::string_view& text) {
    text=trim(text);
    const auto end=std::min(text.find_first_of(" \t"),text.size());
    const auto value=text.substr(0,end);
    text.remove_prefix(end);
    return value;
}
bool number(std::string_view text,std::uint64_t& value) {
    const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);
    return error==std::errc{} && end==text.data()+text.size() && !text.empty();
}
bool buttons(std::string_view text,std::uint16_t& mask) {
    mask=0;
    while (!text.empty()) {
        const auto end=std::min(text.find('+'),text.size());
        const auto name=text.substr(0,end);
        const auto found=std::find(button_names.begin(),button_names.end(),name);
        if (found==button_names.end()) return false;
        mask|=static_cast<std::uint16_t>(1U<<(found-button_names.begin()));
        const bool separated=end<text.size();
        text.remove_prefix(separated ? end+1 : end);
        if (separated && text.empty()) return false;
    }
    return mask!=0;
}
}

std::string PadScript::parse(std::string_view text) {
    presses_.clear();
    std::size_t line_number=0;
    while (!text.empty()) {
        const auto end=std::min(text.find('\n'),text.size());
        auto line=text.substr(0,end);
        text.remove_prefix(end<text.size() ? end+1 : end);
        ++line_number;
        line=trim(line.substr(0,std::min(line.find('#'),line.size())));
        if (line.empty()) continue;
        const auto where="pad script line "+std::to_string(line_number)+": ";
        PadPress press;
        const auto start=field(line),hold=field(line),names=field(line);
        if (!number(start,press.start_ms) || !number(hold,press.hold_ms) || press.hold_ms==0)
            return where+"expected <start-ms> <hold-ms> <buttons> with a nonzero hold";
        if (press.start_ms>UINT64_MAX-press.hold_ms) return where+"time overflows";
        if (!buttons(names,press.buttons)) return where+"unknown button in '"+std::string(names)+"'";
        if (!trim(line).empty()) return where+"unexpected text after the buttons";
        if (presses_.size()==max_events) return where+"more than "+std::to_string(max_events)+" presses";
        presses_.push_back(press);
    }
    if (presses_.empty()) return "pad script contains no presses";
    return {};
}

PadSnapshot PadScript::sample(std::uint64_t elapsed_ms) const {
    PadSnapshot snapshot; snapshot.connected=true;
    for (const auto& press:presses_)
        if (elapsed_ms>=press.start_ms && elapsed_ms-press.start_ms<press.hold_ms) snapshot.buttons|=press.buttons;
    return snapshot;
}

std::uint64_t PadScript::end_ms() const {
    std::uint64_t end=0;
    for (const auto& press:presses_) end=std::max(end,press.start_ms+press.hold_ms);
    return end;
}
}
