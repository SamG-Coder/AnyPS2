#pragma once
#include <nt/analysis.hpp>
#include <nt/binary.hpp>
#include <array>
#include <charconv>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>

namespace nt::ee_codegen::observation {
struct Edge {
    std::filesystem::path memory;
    std::uint64_t input_identity=0;
    std::uint32_t image=0,site=0,target=0,ram_bytes=0;
    std::array<std::uint32_t,32> registers{};
};
inline std::map<std::string,std::string> fields(const std::filesystem::path& path) {
    const auto bytes=read_file(path,16384);
    std::istringstream input(std::string(bytes.begin(),bytes.end()));
    std::map<std::string,std::string> result;
    for (std::string line;std::getline(input,line);) {
        if (!line.empty() && line.back()=='\r') line.pop_back();
        if (line.empty()) continue;
        const auto equal=line.find('=');
        if (line.size()>256 || equal==std::string::npos || !equal || result.size()>=128 ||
            !result.emplace(line.substr(0,equal),line.substr(equal+1)).second)
            throw std::runtime_error("Malformed or duplicate native observation field");
    }
    return result;
}
inline const std::string& required(const std::map<std::string,std::string>& values,const std::string& key) {
    const auto found=values.find(key);
    if (found==values.end()) throw std::runtime_error("Native observation lacks "+key);
    return found->second;
}
inline std::uint64_t number(std::string_view text) {
    std::uint64_t value=0;
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
    if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size())
        throw std::runtime_error("Invalid native observation number");
    return value;
}
inline std::uint32_t word(const std::map<std::string,std::string>& values,const std::string& key) {
    const auto value=number(required(values,key));
    if (value>std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("Native observation word overflows");
    return static_cast<std::uint32_t>(value);
}
inline Edge read(const std::filesystem::path& directory) {
    const auto manifest=fields(directory/"capture.txt"),cpu=fields(directory/"cpu.txt");
    if (required(manifest,"format")!="anyps2-stop-observation-v2" ||
        required(manifest,"complete")!="true" || required(manifest,"resumable")!="false")
        throw std::runtime_error("Expected a complete v2 native stop observation; rebuild and recapture older output");
    Edge edge;
    edge.memory=directory/"ram.bin";
    edge.input_identity=number(required(manifest,"input_identity"));
    edge.image=word(manifest,"active_image"); edge.ram_bytes=word(manifest,"ram_bytes");
    edge.site=word(cpu,"branch_pc"); edge.target=word(cpu,"pc");
    if (!edge.input_identity || !edge.image || edge.image>32 || !edge.ram_bytes || edge.ram_bytes>0x2000000U ||
        required(cpu,"format")!="anyps2-cpu-observation-v1" || word(cpu,"stop")!=2 ||
        word(cpu,"delay_slot") || word(cpu,"pending_target")!=edge.target || !number(required(cpu,"executed")) ||
        word(cpu,"fault_address") || edge.site%4 || edge.target%4 ||
        std::uint64_t(edge.site)+8>edge.ram_bytes || std::uint64_t(edge.target)+4>edge.ram_bytes ||
        std::filesystem::file_size(edge.memory)!=edge.ram_bytes)
        throw std::runtime_error("Observation is not a completed physical-RAM indirect transfer to unmapped native code");
    for (unsigned reg=0;reg<edge.registers.size();++reg) {
        const auto& value=required(cpu,"gpr"+std::to_string(reg));
        const auto comma=value.find(',');
        if (comma==std::string::npos) throw std::runtime_error("Malformed observed register");
        edge.registers[reg]=static_cast<std::uint32_t>(number(std::string_view(value).substr(0,comma)));
        (void)number(std::string_view(value).substr(comma+1));
    }
    return edge;
}
inline void validate_site(const Edge& edge,const elf::Executable& executable,const analysis::Program& program) {
    using r5900::Op;
    const auto site=program.instructions.find(edge.site),slot=program.instructions.find(edge.site+4);
    if (site==program.instructions.end() || slot==program.instructions.end())
        throw std::runtime_error("Observed source is not reached compiled flow at "+hex32(edge.site));
    const auto& call=site->second;
    if (!call.indirect() || (call.op==Op::jr && call.rs==31) || slot->second.branch() ||
        (call.op==Op::jalr && call.rd && edge.registers[call.rd]!=edge.site+8))
        throw std::runtime_error("Observed source is not a supported indirect call or tail transfer at "+hex32(edge.site));
    if (!elf::executable_address(executable,edge.site,8,true) ||
        !elf::executable_address(executable,edge.target,4,true))
        throw std::runtime_error("Observed call or target is outside the active file-backed EE code image");
}
inline void validate_code(const Edge& edge,const analysis::Program& program) {
    const auto ram=read_file(edge.memory,0x2000000U);
    // Bind the observation to original instructions, including newly reached
    // targets. Modified or dynamically generated code requires its own input;
    // it must never be copied out of RAM and silently compiled as the ELF.
    for (const auto& [pc,instruction]:program.instructions)
        if (std::uint64_t(pc)+4>ram.size() || le32(ram,pc)!=instruction.word)
            throw std::runtime_error("Observed instruction bytes differ from the conversion image at "+hex32(pc));
}
}
