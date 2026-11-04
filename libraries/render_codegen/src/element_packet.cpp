#include <nt/element_packet.hpp>
#include <algorithm>
#include <array>
#include <stdexcept>

namespace nt::render {
ElementPacketBinding bind_element_packet(const PipelinedLoop& loop,unsigned base,std::int32_t first_row,
    std::uint64_t tag,std::uint64_t registers) {
    const unsigned encoded=unsigned(tag>>60),rows=encoded ? encoded : 16;
    if (((tag>>58)&3)!=0) throw std::invalid_argument("Element drawing requires a PACKED GIF tag");
    // PRIM updates must remain explicit host commands, not silently discarded.
    if (tag&(1ULL<<46)) throw std::invalid_argument("Element tag PRE requires an explicit PRIM command");
    if (base==0 || base>=16 || rows>5 || loop.stores.empty() || loop.stores.size()>64)
        throw std::invalid_argument("Unsupported element packet base, register count or store count");
    ElementPacketBinding binding{base,first_row,rows,element_output_words(loop),loop.depth,{}};
    std::array<std::array<unsigned,4>,5> words;
    for (auto& row:words) row.fill(graphics::vector_constant_word);
    for (unsigned index=0;index<loop.stores.size();++index) {
        const auto& store=loop.stores[index];
        const auto row=std::int64_t(store.row)-first_row;
        if (store.base!=base || store.stride!=static_cast<std::int32_t>(rows) || loop.strides[base]!=store.stride)
            throw std::invalid_argument("Element stores do not share the declared vertex stream and stride");
        if (row<0 || row>=rows || store.lane>=4 || store.node>=loop.nodes.size())
            throw std::invalid_argument("Element store lies outside the declared vertex record");
        auto& word=words[static_cast<unsigned>(row)][store.lane];
        if (word!=graphics::vector_constant_word)
            throw std::invalid_argument("Overlapping element stores require ordered-write analysis");
        word=index;
    }
    unsigned seen=0;
    for (unsigned row=0;row<rows;++row) {
        graphics::VectorRegister output;
        output.reg=unsigned((registers>>(row*4))&15);
        unsigned kind=0,used=15;
        switch (output.reg) {
        case 1: kind=0; break;
        case 2:
            if (seen&1) throw std::invalid_argument("Element ST must precede RGBAQ");
            kind=1; used=7; break;
        case 3: kind=2; used=3; break;
        case 10: kind=3; used=8; break;
        case 4: case 5: case 12: case 13:
            if (row+1!=rows) throw std::invalid_argument("Element position must end the vertex record");
            kind=4; break;
        default: throw std::invalid_argument("Unsupported element PACKED register: "+std::to_string(output.reg));
        }
        if (seen&(1U<<kind)) throw std::invalid_argument("Duplicate element PACKED register");
        seen|=1U<<kind;
        for (unsigned lane=0;lane<4;++lane) {
            const auto word=words[row][lane];
            if (word==graphics::vector_constant_word) {
                if (used&(1U<<lane)) throw std::invalid_argument("Element packet consumes an unwritten lane");
                continue; // Unused PACKED padding has no GS register effect.
            }
            const auto& node=loop.nodes[loop.stores[word].node];
            if (node.kind==ElementNodeKind::constant) output.constant[lane]=node.value;
            else output.store[lane]=word;
        }
        binding.vertex.push_back(output);
    }
    if (!(seen&16)) throw std::invalid_argument("Element packet has no position register");
    return binding;
}
void emit_element_packet_llvm(std::ostream& out,const ElementPacketBinding& binding,std::string_view symbol) {
    const auto letter=[](char c) { return (c>='a' && c<='z') || (c>='A' && c<='Z') || c=='_'; };
    if (symbol.empty() || !letter(symbol.front()) || std::any_of(symbol.begin(),symbol.end(),[&](char c) {
            return !letter(c) && !(c>='0' && c<='9'); }))
        throw std::invalid_argument("Invalid element binding LLVM symbol");
    out<<"@"<<symbol<<"_count = constant i32 "<<binding.vertex.size()<<", align 4\n";
    out<<"@"<<symbol<<"_words = constant i32 "<<binding.element_words<<", align 4\n";
    out<<"@"<<symbol<<"_minimum_element = constant i32 "<<binding.minimum_element<<", align 4\n";
    constexpr auto type="{ i32, [4 x i32], [4 x i32] }";
    out<<"@"<<symbol<<" = constant ["<<binding.vertex.size()<<" x "<<type<<"] [\n";
    for (std::size_t index=0;index<binding.vertex.size();++index) {
        const auto& reg=binding.vertex[index];
        out<<"  "<<type<<" { i32 "<<reg.reg;
        for (const auto& values:{reg.store,reg.constant}) {
            out<<", [4 x i32] [";
            for (unsigned lane=0;lane<4;++lane) out<<(lane ? ", " : "")<<"i32 "<<values[lane];
            out<<"]";
        }
        out<<" }"<<(index+1<binding.vertex.size() ? "," : "")<<"\n";
    }
    out<<"], align 4\n";
    if (!out) throw std::runtime_error("Cannot write element packet binding");
}
}
