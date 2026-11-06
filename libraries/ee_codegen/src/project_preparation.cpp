#include "project_internal.hpp"
#include "native_layout.hpp"
#include <nt/ee_address.hpp>
#include <nt/ee_syscalls.hpp>
#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace nt::ee_codegen::project_detail {
namespace {
std::vector<CodeRange> declared_code_ranges(const elf::Executable& executable) {
    std::vector<CodeRange> result;
    if (executable.inferred_permissions || executable.writable_code) return result;
    auto data=executable.non_executable_sections;
    std::sort(data.begin(),data.end(),[](const auto& a,const auto& b) { return a.address<b.address; });
    for (const auto& segment:executable.segments) {
        if (!(segment.flags&1)) continue;
        // BSS and allocated data sections are writable application storage,
        // even when the enclosing LOAD segment also contains executable code.
        auto cursor=std::uint64_t(segment.address);
        const auto end=cursor+segment.file_size;
        for (const auto& section:data) {
            if (section.address>=end) break;
            const auto finish=std::uint64_t(section.address)+section.size;
            if (finish<=cursor) continue;
            if (section.address>cursor)
                result.push_back({static_cast<std::uint32_t>(cursor),static_cast<std::uint32_t>(section.address-cursor)});
            cursor=std::min(end,finish);
        }
        if (cursor<end) result.push_back({static_cast<std::uint32_t>(cursor),static_cast<std::uint32_t>(end-cursor)});
    }
    return result;
}
// A section that mixes code and data cannot be classified word by word, and flow
// discovery cannot prove every callback. Select all other lowerable words as well.
// A run is kept only if it transfers control or falls into proven code, which
// drops zero fill and most data. Kept words stay unprotected and self-guarded.
std::vector<CodeRange> sweep_mixed_code(ByteView bytes,const elf::Executable& executable,analysis::Program& program) {
    std::vector<CodeRange> result;
    for (const auto& section:executable.code_sections) {
        const auto segment=std::find_if(executable.segments.begin(),executable.segments.end(),[&](const auto& s) {
            return section.address>=s.address && std::uint64_t(section.address)+section.size<=std::uint64_t(s.address)+s.file_size;
        });
        if (segment==executable.segments.end()) continue;
        std::map<std::uint32_t,r5900::Instruction> candidates;
        for (auto remaining=section.size&~3U;remaining;remaining-=4) {
            const auto pc=section.address+remaining-4;
            if (program.instructions.contains(pc) || !elf::executable_address(executable,pc)) continue;
            const auto offset=std::size_t(segment->offset)+pc-segment->address;
            if (offset+4>bytes.size()) continue;
            const auto word=std::uint32_t(bytes[offset])|(std::uint32_t(bytes[offset+1])<<8)|
                (std::uint32_t(bytes[offset+2])<<16)|(std::uint32_t(bytes[offset+3])<<24);
            const auto instruction=r5900::decode(pc,word);
            const r5900::Instruction* next=nullptr;
            if (const auto found=candidates.find(pc+4);found!=candidates.end()) next=&found->second;
            else if (const auto proven=program.instructions.find(pc+4);proven!=program.instructions.end()) next=&proven->second;
            if (lowerable(instruction,next)) candidates.emplace(pc,instruction);
        }
        for (auto first=candidates.begin();first!=candidates.end();) {
            auto last=first;
            bool useful=first->second.branch() || first->second.terminal();
            for (auto next=std::next(first);next!=candidates.end() && next->first==last->first+4;++next) {
                last=next;
                useful|=next->second.branch() || next->second.terminal();
            }
            const auto end=last->first+4;
            useful|=program.instructions.contains(end) && std::any_of(first,std::next(last),[](const auto& entry) { return entry.second.word!=0; });
            if (useful) {
                program.instructions.insert(first,std::next(last));
                result.push_back({first->first,end-first->first});
            }
            first=std::next(last);
        }
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b) { return a.address<b.address; });
    return result;
}
PreparedProject prepare(ByteView bytes,std::span<const PayloadMapping> mappings,bool diagnose_flow=false,
                 std::span<const observation::Edge> observations={},bool function_symbols=false,bool whole_code=false) {
    auto executable=elf::parse(bytes);
    auto ranges=declared_code_ranges(executable);
    if (whole_code) {
        if (executable.inferred_permissions || executable.writable_code || executable.code_sections.empty())
            throw std::runtime_error("Whole-code conversion requires declared EE instruction sections; classify code/data for this image first");
        ranges.clear();
        for (const auto& range:executable.code_sections) ranges.push_back({range.address,range.size});
    }
    std::vector<std::pair<std::uint32_t,std::uint32_t>> extents;
    std::uint32_t end=0;
    for (const auto& segment:executable.segments) {
        // Initial projects accept physical RAM ELF layouts; aliases in data
        // accesses remain supported by generated code.
        if (std::uint64_t(segment.address)+segment.memory_size>0x2000000)
            throw std::runtime_error("EE project requires ELF segments in physical RAM");
        if (!segment.memory_size) continue;
        const auto finish=segment.address+segment.memory_size;
        for (const auto& [first,last]:extents)
            if (segment.address<last && first<finish) throw std::runtime_error("EE project has overlapping ELF segments");
        extents.emplace_back(segment.address,finish); end=std::max(end,finish);
    }
    for (const auto& segment:executable.overlay_segments) {
        if (std::uint64_t(segment.address)+segment.memory_size>0x2000000)
            throw std::runtime_error("EE project requires ELF segments in physical RAM");
        end=std::max(end,segment.address+segment.memory_size);
    }
    const auto initial_executable=executable;
    std::vector<CodePayload> payloads;
    std::vector<std::uint32_t> payload_entries;
    // Declared mappings always enter at their first byte; discovered copies
    // enter only at their evidenced offsets.
    const auto add_payload=[&](const PayloadMapping& mapping,bool entry_at_start) {
        const auto destination=runtime::default_ram_address(mapping.destination);
        const auto finish=std::uint64_t(destination)+mapping.size;
        if (!mapping.size || mapping.source%4 || destination%4 || mapping.size%4 || finish>0x2000000U)
            throw std::runtime_error("Invalid copied payload mapping");
        for (const auto& [first,last]:extents)
            if (destination<last && first<finish) throw std::runtime_error("Copied payload destination overlaps reserved input storage");
        const auto source=std::find_if(initial_executable.segments.begin(),initial_executable.segments.end(),[&](const auto& segment) {
            return mapping.source>=segment.address && std::uint64_t(mapping.source)+mapping.size<=std::uint64_t(segment.address)+segment.file_size;
        });
        if (source==initial_executable.segments.end()) throw std::runtime_error("Copied payload source is not file-backed ELF storage");
        const auto offset=source->offset+mapping.source-source->address;
        payloads.push_back({destination,std::vector<std::uint8_t>(bytes.begin()+offset,bytes.begin()+offset+mapping.size)});
        executable.segments.push_back({offset,destination,mapping.size,mapping.size,5,4});
        auto code=mapping.code_ranges;
        if (code.empty()) code.push_back({0,mapping.size});
        std::sort(code.begin(),code.end(),[](const auto& a,const auto& b) { return a.offset<b.offset; });
        std::uint32_t cursor=0;
        for (const auto& region:code) {
            const auto end=std::uint64_t(region.offset)+region.size;
            if (!region.size || region.offset%4 || region.size%4 || region.offset<cursor || end>mapping.size)
                throw std::runtime_error("Invalid copied payload code classification");
            if (region.offset>cursor) executable.non_executable_sections.push_back({destination+cursor,region.offset-cursor});
            // A copy found by discovery may mix data; only its reached flow compiles.
            if (whole_code && entry_at_start) executable.code_sections.push_back({destination+region.offset,region.size});
            cursor=static_cast<std::uint32_t>(end);
        }
        if (cursor<mapping.size) executable.non_executable_sections.push_back({destination+cursor,mapping.size-cursor});
        const auto code_entry=[&](std::uint32_t offset) {
            return std::any_of(code.begin(),code.end(),[&](const auto& region) {
                return offset>=region.offset && std::uint64_t(offset)+4<=std::uint64_t(region.offset)+region.size;
            });
        };
        if (entry_at_start) {
            if (!code_entry(0)) throw std::runtime_error("Copied payload entry is outside declared code");
            payload_entries.push_back(destination);
        }
        for (const auto entry_offset:mapping.entry_offsets) {
            if (entry_offset%4 || entry_offset>=mapping.size) throw std::runtime_error("Payload entry offset is outside its extent or unaligned");
            if (!code_entry(entry_offset)) throw std::runtime_error("Copied payload entry is outside declared code");
            payload_entries.push_back(destination+entry_offset);
        }
        extents.emplace_back(destination,static_cast<std::uint32_t>(finish));
    };
    for (const auto& mapping:mappings) add_payload(mapping,true);
    std::vector<std::uint32_t> continuations=payload_entries;
    if (function_symbols) continuations.insert(continuations.end(),executable.function_entries.begin(),executable.function_entries.end());
    const auto wrappers=analysis::syscall_wrappers(bytes,executable);
    continuations.insert(continuations.end(),wrappers.begin(),wrappers.end());
    analysis::Program discovered;
    std::vector<analysis::PointerReference> pointers;
    std::set<std::uint32_t> registered_calls;
    std::map<std::uint32_t,std::set<std::uint32_t>> switch_edges;
    std::vector<PreparedProject::Pass> passes;
    std::vector<bool> accepted(observations.size(),false);
    std::vector<analysis::CodeStore> code_stores;
    // Whole-code conversion still runs flow discovery: registered handlers and
    // copied code reach outside the declared sections.
    while (true) {
        const auto previous_roots=continuations.size();
        pointers.clear();
        discovered=analysis::discover(bytes,executable,program_instruction_limit,continuations,true);
        discovered.indirect_candidates=switch_edges;
        const auto switch_entries=analysis::bounded_switch_entries(bytes,executable,discovered,&switch_edges);
        discovered.indirect_candidates=switch_edges;
        bool added=false;
        for (const auto& [pc,instruction]:discovered.instructions) {
            if (instruction.op!=r5900::Op::syscall || pc<4 || pc>0xfffffffbU) continue;
            const auto previous=discovered.instructions.find(pc-4);
            if (previous==discovered.instructions.end()) continue;
            const auto& setup=previous->second;
            // This supplies compilation candidates only. Runtime dispatch still
            // validates the actual syscall number and all service arguments.
            if (setup.rs || setup.rt!=3 || (!ee::startup_syscall(static_cast<std::uint32_t>(setup.immediate)) &&
                !registered_calls.contains(static_cast<std::uint32_t>(setup.immediate))) ||
                (setup.op!=r5900::Op::addiu && setup.op!=r5900::Op::daddiu && setup.op!=r5900::Op::ori)) continue;
            if (std::find(continuations.begin(),continuations.end(),pc+4)!=continuations.end()) continue;
            continuations.push_back(pc+4); added=true;
        }
        const auto previous_calls=registered_calls.size();
        for (const auto address:analysis::syscall_callbacks(bytes,executable,discovered,0x74,&registered_calls)) {
            if (discovered.instructions.contains(address) ||
                std::find(continuations.begin(),continuations.end(),address)!=continuations.end()) continue;
            continuations.push_back(address); added=true;
        }
        for (const auto call:{0x0aU,0x10U,0x12U}) for (const auto address:analysis::syscall_callbacks(bytes,executable,discovered,call)) {
            if (discovered.instructions.contains(address) ||
                std::find(continuations.begin(),continuations.end(),address)!=continuations.end()) continue;
            continuations.push_back(address); added=true;
        }
        for (const auto address:analysis::thread_entries(bytes,executable,discovered)) {
            if (discovered.instructions.contains(address) ||
                std::find(continuations.begin(),continuations.end(),address)!=continuations.end()) continue;
            continuations.push_back(address); added=true;
        }
        for (const auto address:analysis::stored_code_pointers(bytes,executable,discovered,&pointers)) {
            if (discovered.instructions.contains(address) ||
                std::find(continuations.begin(),continuations.end(),address)!=continuations.end()) continue;
            continuations.push_back(address); added=true;
        }
        for (const auto address:switch_entries) {
            if (discovered.instructions.contains(address) ||
                std::find(continuations.begin(),continuations.end(),address)!=continuations.end()) continue;
            continuations.push_back(address); added=true;
        }
        for (const auto address:analysis::initial_indirect_entries(bytes,executable,discovered)) {
            if (discovered.instructions.contains(address) ||
                std::find(continuations.begin(),continuations.end(),address)!=continuations.end()) continue;
            continuations.push_back(address); added=true;
        }
        for (const auto address:analysis::bounded_pointer_range_entries(bytes,executable,discovered)) {
            if (discovered.instructions.contains(address) ||
                std::find(continuations.begin(),continuations.end(),address)!=continuations.end()) continue;
            continuations.push_back(address); added=true;
        }
        for (const auto address:analysis::referenced_lifecycle_entries(bytes,executable,discovered)) {
            if (discovered.instructions.contains(address) ||
                std::find(continuations.begin(),continuations.end(),address)!=continuations.end()) continue;
            continuations.push_back(address); added=true;
        }
        for (const auto address:analysis::object_table_entries(bytes,executable,discovered)) {
            if (discovered.instructions.contains(address) ||
                std::find(continuations.begin(),continuations.end(),address)!=continuations.end()) continue;
            continuations.push_back(address); added=true;
        }
        for (const auto address:analysis::registered_code_pointers(bytes,executable,discovered,&pointers)) {
            if (discovered.instructions.contains(address) ||
                std::find(continuations.begin(),continuations.end(),address)!=continuations.end()) continue;
            continuations.push_back(address); added=true;
        }
        // The unpatched image keeps installed copies from hiding later evidence.
        for (auto& copy:analysis::copied_code(bytes,initial_executable,discovered)) {
            const auto destination=runtime::default_ram_address(copy.destination);
            if (std::any_of(payloads.begin(),payloads.end(),[&](const auto& payload) { return payload.address==destination; }))
                continue;
            const auto first_entry=payload_entries.size();
            add_payload({copy.source,copy.destination,copy.size,{copy.entry_offsets.begin(),copy.entry_offsets.end()},{}},false);
            continuations.insert(continuations.end(),payload_entries.begin()+first_entry,payload_entries.end());
            added=true;
        }
        if (!payloads.empty()) for (const auto address:analysis::materialized_code_pointers(bytes,executable,discovered)) {
            const auto payload=std::find_if(payloads.begin(),payloads.end(),[&](const auto& p) {
                return address>=p.address && std::uint64_t(address)<std::uint64_t(p.address)+p.bytes.size();
            });
            if (payload==payloads.end() || discovered.instructions.contains(address) ||
                std::find(continuations.begin(),continuations.end(),address)!=continuations.end()) continue;
            continuations.push_back(address); added=true;
        }
        // Observed edges augment conversion, never runtime dispatch. A source
        // must already be reached by ordinary discovery or an earlier verified
        // observation. A collection cannot bootstrap itself from arbitrary PCs.
        for (std::size_t index=0;index<observations.size();++index) {
            const auto& edge=observations[index];
            if (accepted[index] || !discovered.instructions.contains(edge.site)) continue;
            observation::validate_site(edge,executable,discovered);
            accepted[index]=true;
            if (!discovered.instructions.contains(edge.target) &&
                std::find(continuations.begin(),continuations.end(),edge.target)==continuations.end()) {
                continuations.push_back(edge.target); added=true;
            }
        }
        code_stores=analysis::code_stores(executable,discovered);
        for (const auto& store:code_stores) {
            if (!store.value || (*store.value>>26)!=2) continue;
            const auto target=(store.address&0xf0000000U)|((*store.value&0x03ffffffU)<<2);
            if (discovered.instructions.contains(target) || !elf::executable_address(executable,target) ||
                std::find(continuations.begin(),continuations.end(),target)!=continuations.end()) continue;
            continuations.push_back(target); added=true;
        }
        added|=registered_calls.size()!=previous_calls;
        passes.push_back({1,passes.size()+1,discovered.instructions.size(),
            continuations.size()-previous_roots,registered_calls.size()-previous_calls});
        if (!added) break;
    }
    for (std::size_t index=0;index<observations.size();++index) {
        if (!accepted[index]) throw std::runtime_error("Observed source is not reached compiled flow at "+hex32(observations[index].site));
        observation::validate_code(observations[index],discovered);
    }
    if (whole_code) {
        auto selected=analysis::whole_code(bytes,executable,program_instruction_limit);
        discovered.instructions.merge(selected.instructions);
        discovered.ordinary.merge(selected.ordinary);
        discovered.entries.merge(selected.entries);
        if (discovered.instructions.size()>program_instruction_limit) throw std::runtime_error("Whole-code instruction limit exceeded");
        passes.push_back({1,passes.size()+1,discovered.instructions.size(),0,0});
    }
    if (executable.inferred_permissions || executable.writable_code) for (const auto& [pc,instruction]:discovered.instructions) {
        // Copied payloads own their destination instructions; they are not ELF code.
        if (std::any_of(payloads.begin(),payloads.end(),[&](const auto& payload) {
                return pc-payload.address<payload.bytes.size(); })) continue;
        if (!ranges.empty() && std::uint64_t(ranges.back().address)+ranges.back().size==pc) ranges.back().size+=4;
        else ranges.push_back({pc,4});
    }
    // Original native service metadata. Reserve a gap outside every ELF and
    // copied-code extent, including BSS; never overwrite application data.
    std::sort(extents.begin(),extents.end());
    std::uint32_t syscall_table=0x10000;
    for (const auto& [begin,finish]:extents) {
        if (std::uint64_t(syscall_table)+1024<=begin) break;
        if (syscall_table<finish) syscall_table=(finish+3U)&~3U;
    }
    if (std::uint64_t(syscall_table)+1024>0x2000000U)
        throw std::runtime_error("No native syscall metadata extent available");
    PreparedProject result;
    result.whole_code=whole_code;
    // Patched words and their delay slots leave write protection and guard themselves.
    std::set<std::uint32_t> patched_words;
    for (const auto& store:code_stores) {
        patched_words.insert(store.address);
        if (!store.value || (*store.value>>26)!=2) continue;
        patched_words.insert(store.address+4);
        const CodePatch patch{store.address,*store.value,(store.address&0xf0000000U)|((*store.value&0x03ffffffU)<<2)};
        const auto same=[&](const CodePatch& other) { return other.address==patch.address && other.word==patch.word; };
        if (std::none_of(result.code_patches.begin(),result.code_patches.end(),same)) result.code_patches.push_back(patch);
    }
    for (const auto address:patched_words) { // Ascending, as the emitter expects.
        if (!discovered.instructions.contains(address)) throw std::runtime_error("Patched code word is not compiled at "+hex32(address));
        result.unverified_code.push_back({address,4});
    }
    if (whole_code) for (const auto& region:executable.code_sections)
        result.selected_regions.push_back({region.address,region.size,1});
    if (function_symbols) for (const auto entry:executable.function_entries) result.function_roots.emplace_back(1,entry);
    result.observations.assign(observations.begin(),observations.end());
    result.passes=std::move(passes);
    if (diagnose_flow) result.flow_gaps=analysis::callback_flow_gaps(bytes,executable,discovered,pointers);
    result.executable=std::move(executable);
    result.calls=analysis::call_sites(bytes,result.executable,discovered);
    result.pointers=std::move(pointers);
    for (const auto& record:analysis::layout_candidates(bytes,result.executable,discovered)) result.layouts.push_back(record.fields);
    result.discovered=std::move(discovered);
    result.ranges=std::move(ranges);
    result.payloads=std::move(payloads);
    result.syscall_table=syscall_table;
    result.registered_calls=std::move(registered_calls);
    result.wrapper_candidates=wrappers.size();
    result.image.assign(end,0);
    for (const auto& segment:initial_executable.segments)
        std::copy_n(bytes.begin()+segment.offset,segment.file_size,result.image.begin()+segment.address);
    for (const auto& segment:initial_executable.overlay_segments)
        std::copy_n(bytes.begin()+segment.offset,segment.file_size,result.image.begin()+segment.address);
    return result;
}
}
// The bytes to compile for one input: a copy with the patches it holds applied.
Bytes apply_instruction_patches(ByteView source,std::span<const InstructionPatch> patches,std::vector<bool>& applied,std::vector<std::uint32_t>& addresses) {
    Bytes result(source.begin(),source.end());
    if (patches.empty()) return result;
    const auto executable=elf::parse(source);
    const auto hex=[](std::uint32_t value) { std::ostringstream text; text<<"0x"<<std::hex<<value; return text.str(); };
    for (std::size_t n=0;n<patches.size();++n) {
        const auto& patch=patches[n];
        for (const auto& segment:executable.segments) {
            if (patch.address<segment.address || std::uint64_t(patch.address)+4>std::uint64_t(segment.address)+segment.file_size) continue;
            const auto at=segment.offset+(patch.address-segment.address);
            std::uint32_t held=0;
            for (unsigned b=0;b<4;++b) held|=std::uint32_t(result.at(at+b))<<(b*8);
            if (held!=patch.expected) throw std::runtime_error("Code patch at "+hex(patch.address)+" expected "+hex(patch.expected)+" but the input holds "+hex(held));
            for (unsigned b=0;b<4;++b) result.at(at+b)=std::uint8_t(patch.replacement>>(b*8));
            applied[n]=true; addresses.push_back(patch.address);
            break;
        }
    }
    return result;
}
PreparedProject prepare_project(const std::filesystem::path& input,std::span<const PayloadMapping> mappings,
                         std::span<const std::filesystem::path> images,bool diagnose_flow,
                         std::span<const std::filesystem::path> observation_paths,bool function_symbols,bool whole_code,
                         std::span<const InstructionPatch> patches) {
    if (patches.size()>256) throw std::invalid_argument("Too many code patches");
    for (const auto& patch:patches) if (patch.address%4) throw std::invalid_argument("Code patch address is not aligned");
    std::vector<bool> applied(patches.size(),false);
    if (whole_code && !observation_paths.empty())
        throw std::invalid_argument("Whole-code conversion does not use runtime observations");
    if (images.size()>31) throw std::invalid_argument("Too many secondary executable images");
    if (observation_paths.size()>64) throw std::invalid_argument("Too many native observations");
    const auto bytes=read_file(input);
    // Versioned, path-independent identity of the ordered conversion inputs.
    // This detects stale observations; it is not a signature or authenticity
    // guarantee. Observations themselves are excluded so subsequent offline
    // builds can accumulate evidence for the same source images.
    std::ostringstream identity;
    std::vector<std::pair<std::size_t,std::uint64_t>> image_identities;
    identity<<"anyps2-ee-input-v1\n"<<bytes.size()<<' '<<fingerprint(bytes)<<'\n';
    if (whole_code) identity<<"whole-code\n";
    for (const auto& patch:patches) identity<<"patch "<<patch.address<<' '<<patch.expected<<' '<<patch.replacement<<'\n';
    if (function_symbols) identity<<"function-symbols\n";
    for (const auto& mapping:mappings) {
        identity<<"payload "<<mapping.source<<' '<<mapping.destination<<' '<<mapping.size;
        for (const auto offset:mapping.entry_offsets) identity<<' '<<offset;
        for (const auto& region:mapping.code_ranges) identity<<" code="<<region.offset<<':'<<region.size;
        identity<<'\n';
    }
    for (const auto& path:images) {
        const auto source=read_file(path);
        const auto hash=fingerprint(source);
        image_identities.emplace_back(source.size(),hash);
        identity<<"image "<<source.size()<<' '<<hash<<'\n';
    }
    const auto text=identity.str();
    const auto input_identity=fingerprint(ByteView(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));
    std::array<std::vector<observation::Edge>,32> observed;
    for (const auto& path:observation_paths) {
        auto edge=observation::read(path);
        if (edge.input_identity!=input_identity) throw std::runtime_error("Native observation input identity differs; regenerate and recapture this input");
        if (edge.image>images.size()+1) throw std::runtime_error("Native observation refers to an unknown executable image");
        observed[edge.image-1].push_back(std::move(edge));
    }
    std::vector<std::uint32_t> primary_patched;
    const auto compiled_bytes=apply_instruction_patches(bytes,patches,applied,primary_patched);
    auto prepared=prepare(compiled_bytes,mappings,diagnose_flow,observed[0],function_symbols,whole_code);
    // Additional images have native lifetimes that swept extents cannot overlap.
    if (!whole_code && prepared.executable.writable_code && images.empty())
        {
            auto swept=sweep_mixed_code(compiled_bytes,prepared.executable,prepared.discovered);
            prepared.unverified_code.insert(prepared.unverified_code.end(),swept.begin(),swept.end());
        }
    prepared.input_identity=input_identity;
    if (!images.empty()) {
        const auto snapshot=[](ByteView source,const elf::Executable& executable,std::uint32_t id,std::vector<std::uint32_t> patched) {
            ExecutableImage image{executable.entry,id,{},std::move(patched)};
            for (const auto& segment:executable.segments) if (segment.file_size)
                image.segments.push_back({segment.address,Bytes(source.begin()+segment.offset,source.begin()+segment.offset+segment.file_size)});
            return image;
        };
        for (auto& range:prepared.ranges) range.image=1;
        prepared.images.push_back(snapshot(bytes,elf::parse(bytes),1,primary_patched));
        auto occupied=prepared.executable.segments;
        for (const auto& path:images) {
            const auto source=read_file(path);
            const auto id=static_cast<std::uint32_t>(prepared.images.size()+1);
            if (source.size()!=image_identities[id-2].first || fingerprint(source)!=image_identities[id-2].second)
                throw std::runtime_error("Executable image changed during conversion; retry with stable input files");
            std::vector<std::uint32_t> image_patched;
            const auto compiled_source=apply_instruction_patches(source,patches,applied,image_patched);
            auto extra=prepare(compiled_source,{},diagnose_flow,observed[id-1],function_symbols,whole_code);
            for (const auto& [image,entry]:extra.function_roots) prepared.function_roots.emplace_back(id,entry);
            for (const auto& section:extra.executable.segments) {
                for (const auto& old:occupied)
                    if (std::uint64_t(section.address)<std::uint64_t(old.address)+old.memory_size &&
                        std::uint64_t(old.address)<std::uint64_t(section.address)+section.memory_size)
                        throw std::runtime_error("Executable image overlaps another image or resident payload");
                if (std::uint64_t(section.address)<std::uint64_t(prepared.syscall_table)+1024 &&
                    std::uint64_t(prepared.syscall_table)<std::uint64_t(section.address)+section.memory_size)
                    throw std::runtime_error("Executable image overlaps native service metadata");
            }
            occupied.insert(occupied.end(),extra.executable.segments.begin(),extra.executable.segments.end());
            for (const auto& [pc,instruction]:extra.discovered.instructions)
                if (!prepared.discovered.instructions.emplace(pc,instruction).second)
                    throw std::runtime_error("Executable image has an ambiguous compiled target");
            prepared.discovered.unresolved.insert(extra.discovered.unresolved.begin(),extra.discovered.unresolved.end());
            prepared.discovered.ordinary.insert(extra.discovered.ordinary.begin(),extra.discovered.ordinary.end());
            prepared.discovered.entries.insert(extra.discovered.entries.begin(),extra.discovered.entries.end());
            prepared.calls.insert(prepared.calls.end(),extra.calls.begin(),extra.calls.end());
            prepared.pointers.insert(prepared.pointers.end(),extra.pointers.begin(),extra.pointers.end());
            prepared.flow_gaps.insert(prepared.flow_gaps.end(),extra.flow_gaps.begin(),extra.flow_gaps.end());
            prepared.observations.insert(prepared.observations.end(),extra.observations.begin(),extra.observations.end());
            for (auto pass:extra.passes) { pass.image=id; prepared.passes.push_back(pass); }
            for (auto range:extra.ranges) { range.image=id; prepared.ranges.push_back(range); }
            for (auto region:extra.selected_regions) { region.image=id; prepared.selected_regions.push_back(region); }
            prepared.layouts.insert(prepared.layouts.end(),extra.layouts.begin(),extra.layouts.end());
            prepared.images.push_back(snapshot(source,extra.executable,id,std::move(image_patched)));
        }
        for (const auto& [pc,instruction]:prepared.discovered.instructions) prepared.discovered.unresolved.erase(pc);
    }
    for (std::size_t n=0;n<patches.size();++n) {
        if (!applied[n]) throw std::runtime_error("Code patch is not in any input");
        if (!prepared.discovered.instructions.contains(patches[n].address))
            throw std::runtime_error("Code patch is not selected EE code");
    }
    return prepared;
}
FlowDiagnostics diagnose_flow(const PreparedProject& prepared) {
    FlowDiagnostics diagnostics;
    auto& result=diagnostics.result;
    result.instructions=prepared.discovered.instructions.size(); result.passes=prepared.passes.size();
    result.unmapped_targets=prepared.discovered.unresolved.size();
    result.record_binding_candidates=prepared.flow_gaps.size();
    for (const auto& call:prepared.calls) {
        if (call.kind==analysis::CallKind::service) ++result.service_boundaries;
        else if (!call.target && call.kind!=analysis::CallKind::return_link && call.kind!=analysis::CallKind::return_exception)
            ++result.unresolved_indirect;
    }
    for (const auto& reference:prepared.pointers)
        if (!reference.callable && !prepared.discovered.instructions.contains(reference.target)) ++result.unclassified_references;
    std::vector<r5900::Instruction> instructions;
    instructions.reserve(prepared.discovered.instructions.size());
    for (const auto& [pc,instruction]:prepared.discovered.instructions) instructions.push_back(instruction);
    diagnostics.rejection=reject_unsupported(instructions);
    result.lowering_supported=!instructions.empty() && diagnostics.rejection.empty();
    try { (void)prepare_native_layout(prepared.layouts); result.layout_supported=true; }
    catch (const std::exception& error) { diagnostics.layout_error=error.what(); }
    return diagnostics;
}
}

namespace nt::ee_codegen {
EeAssessment assess(ByteView bytes,std::span<const PayloadMapping> mappings) {
    EeAssessment result;
    try {
        const auto prepared=project_detail::prepare(bytes,mappings);
        (void)prepare_native_layout(prepared.layouts);
        result.instructions=prepared.discovered.instructions.size();
        result.wrapper_candidates=prepared.wrapper_candidates;
        std::set<std::uint32_t> unclassified;
        for (const auto& reference:prepared.pointers)
            if (!reference.callable && !prepared.discovered.instructions.contains(reference.target)) unclassified.insert(reference.target);
        result.unclassified_pointer_count=unclassified.size();
        for (const auto target:unclassified) {
            if (result.unclassified_pointer_sample.size()==16) break;
            result.unclassified_pointer_sample.push_back(target);
        }
        result.registered_calls.assign(prepared.registered_calls.begin(),prepared.registered_calls.end());
        for (const auto& segment:prepared.executable.segments)
            if (segment.flags&1) result.declared_executable_words+=segment.memory_size/4;
        result.unresolved_count=prepared.discovered.unresolved.size();
        for (const auto address:prepared.discovered.unresolved) {
            if (result.unresolved_sample.size()==16) break;
            result.unresolved_sample.push_back(address);
        }
        std::vector<r5900::Instruction> instructions;
        instructions.reserve(prepared.discovered.instructions.size());
        for (const auto& [pc,instruction]:prepared.discovered.instructions) {
            instructions.push_back(instruction);
            using r5900::Op;
            const bool transfer=(instruction.op==Op::mtc0 || instruction.op==Op::mfc0) &&
                (instruction.rd==0 || instruction.rd==1 || instruction.rd==2 || instruction.rd==3 || instruction.rd==5 || instruction.rd==6 || instruction.rd==10);
            if (transfer || instruction.op==Op::tlbr || instruction.op==Op::tlbwi ||
                instruction.op==Op::tlbwr || instruction.op==Op::tlbp) {
                if (!result.address_space_requests) result.first_address_space_request=instruction.pc;
                ++result.address_space_requests;
            }
            if (!instruction.indirect()) continue;
            if (!result.indirect_sites) result.first_indirect=instruction.pc;
            ++result.indirect_sites;
        }
        for (const auto& instruction:instructions) {
            if (instruction.op!=r5900::Op::syscall) continue;
            const auto previous=prepared.discovered.instructions.find(instruction.pc-4);
            const auto& setup=previous==prepared.discovered.instructions.end() ? instruction : previous->second;
            if (previous==prepared.discovered.instructions.end() || setup.rs || setup.rt!=3 ||
                (setup.op!=r5900::Op::addiu && setup.op!=r5900::Op::daddiu && setup.op!=r5900::Op::ori)) {
                ++result.unknown_syscall_sites;
                continue;
            }
            ++result.known_syscalls[static_cast<std::uint32_t>(setup.immediate)];
        }
        if (!instructions.empty() && instructions.size()<=program_instruction_limit)
            result.rejection=reject_unsupported(instructions);
        result.convertible=result.unresolved_count==0 && result.rejection.empty() && !instructions.empty() &&
            instructions.size()<=program_instruction_limit;
    } catch (const std::exception& error) {
        result.layout_error=error.what();
        result.convertible=false;
    }
    return result;
}}
