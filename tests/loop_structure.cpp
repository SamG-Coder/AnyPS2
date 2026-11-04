#include "render_loop_fixture.hpp"
#include <nt/loop_structure.hpp>
#include <nt/loop_pipeline.hpp>
#include <nt/element_packet.hpp>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iostream>
#include <string>
#include <string_view>

// Original synthetic VU loops. No commercial program or SDK bytes are included.
namespace {
using render_fixture::lower_nop;
using render_fixture::upper_nop;
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
bool has(const nt::render::LoopStructure& loop,std::string_view blocker) {
    return std::find(loop.blockers.begin(),loop.blockers.end(),blocker)!=loop.blockers.end();
}
std::uint32_t load_increment(unsigned vector,unsigned base) { return 0x80000000U|(15U<<21)|(vector<<16)|(base<<11)|(13U<<6)|0x3cU; }
std::uint32_t store_increment(unsigned vector,unsigned base) { return 0x80000000U|(15U<<21)|(base<<16)|(vector<<11)|(13U<<6)|0x3dU; }
std::uint32_t subtract_immediate(unsigned index,unsigned amount) { return (9U<<25)|(index<<16)|(index<<11)|amount; }
std::uint32_t branch_not_zero(unsigned index,int displacement) { return (41U<<25)|(index<<11)|(static_cast<unsigned>(displacement)&0x7ffU); }
std::uint32_t graphics_kick(unsigned index) { return 0x80000000U|(index<<11)|(27U<<6)|0x3cU; }
std::uint32_t forward_branch(int displacement) { return (32U<<25)|(static_cast<unsigned>(displacement)&0x7ffU); }
// Q = fs.fsf / sqrt(ft.ftf); WAITQ; upper MULq (fd = fs * Q).
std::uint32_t reciprocal_square_root(unsigned fs,unsigned fsf,unsigned ft,unsigned ftf) {
    return 0x80000000U|(ftf<<23)|(fsf<<21)|(ft<<16)|(fs<<11)|0x3beU;
}
constexpr std::uint32_t wait_quotient=0x800003bfU;
std::uint32_t multiply_quotient(unsigned destination,unsigned source) { return (15U<<21)|(source<<11)|(destination<<6)|0x1cU; }
std::uint32_t float_to_fixed4(unsigned destination,unsigned source) { return (15U<<21)|(destination<<16)|(source<<11)|0x17dU; }
struct Builder {
    nt::Bytes bytes;
    void pair(std::uint32_t lower=lower_nop,std::uint32_t upper=upper_nop) { render_fixture::word(bytes,lower); render_fixture::word(bytes,upper); }
    // Backedge to pair zero plus its delay slot.
    void close(unsigned counter) {
        const auto displacement=-static_cast<int>(bytes.size()/8)-1;
        pair(branch_not_zero(counter,displacement)); pair();
    }
    nt::render::LoopStructure analyze() const { return nt::render::analyze_loop_structure(nt::vu::frame(bytes,1,0),1); }
};
// Each element scales its input row by x/sqrt(y): a quotient-unit loop whose element
// function exercises the reciprocal square root helper and its VU-stop status.
nt::render::PipelinedLoop quotient_loop() {
    Builder b;
    b.pair(load_increment(1,3));
    b.pair(reciprocal_square_root(1,0,1,1));
    b.pair(wait_quotient);
    b.pair(lower_nop,multiply_quotient(2,1));
    b.pair(store_increment(2,4));
    b.pair(subtract_immediate(9,1));
    b.close(9);
    auto recovered=nt::render::recover_pipelined_loop(nt::vu::frame(b.bytes,1,0),1,b.analyze());
    if (!recovered.loop) throw std::runtime_error("Quotient loop not recovered: "+recovered.reason);
    return *recovered.loop;
}
// Each element copies a color row and converts a position row to 12.4 fixed
// point, storing both through one pointer: a PACKED RGBAQ, XYZ2 vertex record.
nt::Bytes vertex_program() {
    Builder b;
    b.pair(load_increment(1,3));
    b.pair(load_increment(2,5));
    b.pair(lower_nop,float_to_fixed4(6,1));
    b.pair(store_increment(2,4));
    b.pair(store_increment(6,4));
    b.pair(subtract_immediate(9,1));
    b.close(9);
    return b.bytes;
}
nt::render::PipelinedLoop vertex_loop() {
    const auto bytes=vertex_program();
    const auto pairs=nt::vu::frame(bytes,1,0);
    auto recovered=nt::render::recover_pipelined_loop(pairs,1,nt::render::analyze_loop_structure(pairs,1));
    if (!recovered.loop) throw std::runtime_error("Vertex loop not recovered: "+recovered.reason);
    return *recovered.loop;
}
void run() {
    using namespace nt::render;
    // Straight-line loop: uniform rows, a vertex stream, an output stream and a countdown.
    {
        const auto c=render_fixture::configuration(0);
        const auto loop=analyze_loop_structure(nt::vu::frame(render_fixture::program(0),1,c.destination),1);
        require(loop.counter==c.count && loop.steps[c.count]==-1 && loop.blockers.empty(),"Straight-line loop structure not recovered");
        unsigned uniform=0,vertex=0,output=0;
        for (const auto& access:loop.accesses) {
            if (access.kind==AccessKind::load_vector) (access.stride ? vertex : uniform)++;
            if (access.kind==AccessKind::store_vector && access.stride==static_cast<int>(c.output_stride)) ++output;
        }
        require(uniform==4 && vertex==1 && output==1,"Stream rates not recovered");
    }
    // Software pipelining: the store at the top consumes the previous iteration's product.
    {
        Builder b;
        b.pair(store_increment(2,4));
        b.pair(load_increment(1,3));
        b.pair();
        b.pair(lower_nop,render_fixture::upper(42,2,1,5));
        b.pair(subtract_immediate(9,1));
        b.pair();
        b.close(9);
        const auto loop=b.analyze();
        require(loop.counter==9 && loop.steps[9]==-1 && loop.steps[3]==1 && loop.steps[4]==1,"Auto-increment induction not recovered");
        require(loop.carried_vectors[2]==CarriedValue::computed && loop.invariant_vectors[5],"Pipelined value or invariant not classified");
        require(loop.blockers.size()==1 && has(loop,"computed vector carried between iterations"),"Pipelining blocker missing or extra");
        const auto store=std::find_if(loop.accesses.begin(),loop.accesses.end(),[](const auto& a) { return a.kind==AccessKind::store_vector; });
        require(store!=loop.accesses.end() && store->address.base==4 && store->stride==1,"Post-increment store stream not recovered");
        // Element k is stored by iteration k from the product computed with input k-1.
        const auto recovered=recover_pipelined_loop(nt::vu::frame(b.bytes,1,0),1,loop);
        require(recovered.loop && recovered.loop->depth==1 && recovered.loop->stores.size()==4,"Pipelined element function not recovered");
        for (const auto& out:recovered.loop->stores) {
            require(out.base==4 && out.row==0 && out.stride==1,"Output element mapping not recovered");
            const auto& node=recovered.loop->nodes[out.node];
            require(node.kind==ElementNodeKind::operation && node.a==static_cast<std::uint32_t>(ElementOperation::multiply),"Product not recovered");
            const auto& input=recovered.loop->nodes[node.b];
            const auto& matrix=recovered.loop->nodes[node.c];
            require(input.kind==ElementNodeKind::load && input.a==3 && static_cast<std::int32_t>(input.b)==-1 && input.c==out.lane,
                "Stream element offset not recovered");
            require(matrix.kind==ElementNodeKind::invariant && matrix.b==5 && matrix.c==out.lane,"Invariant operand not recovered");
        }
    }
    // Two stores per iteration through one pointer: a PACKED RGBAQ/XYZ2 record.
    {
        const auto loop=vertex_loop();
        require(loop.stores.size()==8 && element_output_words(loop)==9,"Vertex record stores not recovered");
        for (unsigned n=0;n<8;++n) {
            const auto& out=loop.stores[n];
            require(out.base==4 && out.stride==2 && out.row==static_cast<std::int32_t>(n/4) && out.lane==n%4,"Vertex record layout not recovered");
            const auto& node=loop.nodes[out.node];
            if (n<4) require(node.kind==ElementNodeKind::load && node.a==5,"Color copy not recovered");
            else require(node.kind==ElementNodeKind::operation && node.a==static_cast<std::uint32_t>(ElementOperation::float_to_integer) &&
                node.value==4 && loop.nodes[node.b].kind==ElementNodeKind::load && loop.nodes[node.b].a==3,"Fixed-point position not recovered");
        }
    }
    // Bind by symbolic row/lane, never by emission order or chosen VU registers.
    {
        const auto original=vertex_loop();
        for (unsigned rotation=0;rotation<8;++rotation) {
            auto loop=original;
            loop.depth=2; loop.strides[11]=2;
            for (auto& store:loop.stores) { store.base=11; store.row-=7; }
            std::rotate(loop.stores.begin(),loop.stores.begin()+rotation,loop.stores.end());
            const auto binding=bind_element_packet(loop,11,-7,2ULL<<60,0x51);
            require(binding.minimum_element==2 && binding.base==11 && binding.first_row==-7,
                "Binding dropped its steady-state or origin constraint");
            for (unsigned row=0;row<2;++row) for (unsigned lane=0;lane<4;++lane) {
                const auto index=binding.vertex[row].store[lane];
                require(index<loop.stores.size() && loop.stores[index].row==static_cast<int>(row)-7 &&
                    loop.stores[index].lane==lane,"Binding depends on register allocation or store order");
            }
        }
        const auto rejects=[&](const PipelinedLoop& loop,std::uint64_t tag,std::uint64_t regs,const char* reason) {
            try { (void)bind_element_packet(loop,4,0,tag,regs); }
            catch (const std::invalid_argument& error) {
                require(std::string_view(error.what()).find(reason)!=std::string_view::npos,"Wrong packet-binding diagnostic"); return;
            }
            throw std::runtime_error("Unsafe element packet binding accepted");
        };
        auto loop=original; loop.stores.pop_back();
        rejects(loop,2ULL<<60,0x51,"unwritten lane");
        loop=original; loop.stores.push_back(loop.stores.front());
        rejects(loop,2ULL<<60,0x51,"Overlapping");
        loop=original; loop.stores[0].base=7;
        rejects(loop,2ULL<<60,0x51,"stream and stride");
        loop=original; loop.stores[0].stride=3;
        rejects(loop,2ULL<<60,0x51,"stream and stride");
        loop=original; loop.stores[0].row=2;
        rejects(loop,2ULL<<60,0x51,"outside");
        rejects(original,(2ULL<<60)|(1ULL<<58),0x51,"PACKED");
        rejects(original,(2ULL<<60)|(1ULL<<46),0x51,"PRE");
        rejects(original,2ULL<<60,0x15,"position must end");
        rejects(original,2ULL<<60,0x5e,"Unsupported");
        // A constant store is immutable metadata; missing unused ST padding is
        // legal, but missing coordinates remain an explicit failure.
        loop=original;
        loop.nodes.push_back({ElementNodeKind::constant,0,0,0,0,73});
        loop.stores[0].node=static_cast<unsigned>(loop.nodes.size()-1);
        const auto constant=bind_element_packet(loop,4,0,2ULL<<60,0x51);
        require(constant.vertex[0].store[0]==nt::graphics::vector_constant_word && constant.vertex[0].constant[0]==73,
            "Constant packet word was not folded");
        loop=original; loop.stores.erase(loop.stores.begin()+3);
        const auto padding=bind_element_packet(loop,4,0,2ULL<<60,0x52);
        require(padding.vertex[0].store[3]==nt::graphics::vector_constant_word,"Unused ST padding requires a store");
    }
    // A WAITQ-published quotient feeds every lane of the element's product.
    {
        const auto loop=quotient_loop();
        require(loop.stores.size()==4,"Quotient element stores not recovered");
        for (const auto& out:loop.stores) {
            const auto& product=loop.nodes[out.node];
            require(out.lane==static_cast<unsigned>(&out-loop.stores.data()),"Element stores are not in lane order");
            require(out.base==4 && out.stride==1 && product.kind==ElementNodeKind::operation &&
                product.a==static_cast<std::uint32_t>(ElementOperation::multiply),"Quotient product not recovered");
            const auto& quotient=loop.nodes[product.c];
            require(quotient.kind==ElementNodeKind::operation && quotient.a==static_cast<std::uint32_t>(ElementOperation::reciprocal_square_root) &&
                loop.nodes[quotient.b].c==0 && loop.nodes[quotient.c].c==1,"Reciprocal square root operands not recovered");
        }
        require(element_output_words(loop)==5,"Element output layout changed");
        bool rejected=false;
        try { std::ostringstream ignored; emit_element_kernel_llvm(ignored,loop,2,false); } catch (const std::invalid_argument&) { rejected=true; }
        require(rejected,"Element kernel accepted an invalid VU unit");
    }
    // A value read back from an earlier iteration's store is not independent.
    {
        Builder b;
        b.pair(store_increment(2,4));
        b.pair(render_fixture::load(1,4,-2));
        b.pair(lower_nop,render_fixture::upper(42,2,1,5));
        b.pair(subtract_immediate(9,1));
        b.close(9);
        const auto recovered=recover_pipelined_loop(nt::vu::frame(b.bytes,1,0),1,b.analyze());
        require(!recovered.loop && recovered.reason=="a value flows between iterations through memory","Memory recurrence accepted");
    }
    // A running sum never becomes independent of its entry value.
    {
        Builder b;
        b.pair(load_increment(1,3));
        b.pair(lower_nop,render_fixture::upper(40,2,2,1));
        b.pair(store_increment(2,4));
        b.pair(subtract_immediate(9,1));
        b.close(9);
        const auto recovered=recover_pipelined_loop(nt::vu::frame(b.bytes,1,0),1,b.analyze());
        require(!recovered.loop && recovered.reason=="no entry-independent steady state","Recurrence accepted as an element function");
    }
    // Internal control flow and a graphics kick are reported, not accepted.
    {
        Builder b;
        b.pair(load_increment(1,3));
        b.pair(forward_branch(1)); b.pair();
        b.pair(graphics_kick(3));
        b.pair(subtract_immediate(9,1));
        b.close(9);
        const auto loop=b.analyze();
        require(has(loop,"control flow inside the iteration") && has(loop,"graphics kick inside the iteration"),"Effects not reported");
    }
    // A region without a conditional backedge to its first pair is malformed.
    {
        Builder b;
        for (unsigned n=0;n<4;++n) b.pair();
        bool rejected=false;
        try { (void)b.analyze(); } catch (const std::invalid_argument&) { rejected=true; }
        require(rejected,"Region without a backedge was accepted");
    }
    // Discovery finds exactly the backward conditional branch.
    {
        Builder b;
        b.pair(); b.pair(subtract_immediate(9,1)); b.close(9);
        const auto regions=find_loop_regions(nt::vu::frame(b.bytes,1,0),1);
        require(regions.size()==1 && regions[0].head==0 && regions[0].branch==2,"Loop discovery failed");
    }
}
}
// --emit-element quotient|vertex <prefix>: writes a fixture loop's CPU and Vulkan
// element kernels as <prefix>.cpu.ll and <prefix>.gpu.ll for execution tests.
int main(int argc,char** argv) {
    try {
        if (argc==4 && std::string_view(argv[1])=="--emit-element") {
            const std::string_view fixture=argv[2];
            if (fixture!="quotient" && fixture!="vertex") throw std::invalid_argument("Unknown element fixture");
            auto loop=fixture=="vertex" ? vertex_loop() : quotient_loop();
            if (fixture=="vertex") {
                // Output ABI order need not equal packet memory order.
                std::reverse(loop.stores.begin(),loop.stores.end());
                const auto bytes=vertex_program();
                std::ofstream binary(std::string(argv[3])+".bin",std::ios::binary);
                binary.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
                if (!binary) throw std::runtime_error("Cannot write vertex fixture");
            }
            for (const bool vulkan:{false,true}) {
                std::ofstream out(std::string(argv[3])+(vulkan ? ".gpu.ll" : ".cpu.ll"),std::ios::binary);
                nt::render::emit_element_kernel_llvm(out,loop,1,vulkan);
                if (fixture=="vertex" && !vulkan) {
                    const auto binding=nt::render::bind_element_packet(loop,4,0,2ULL<<60,0x51);
                    nt::render::emit_element_packet_llvm(out,binding,"nt_render_vertex_layout");
                }
            }
            return 0;
        }
        if (argc!=1) throw std::invalid_argument("Usage: render-loop-structure-tests [--emit-element quotient|vertex <prefix>]");
        run();
    }
    catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
    return 0;
}
