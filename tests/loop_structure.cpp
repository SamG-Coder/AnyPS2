#include "render_loop_fixture.hpp"
#include <nt/loop_structure.hpp>
#include <nt/loop_pipeline.hpp>
#include <algorithm>
#include <iostream>
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
int main() {
    try { run(); }
    catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
    return 0;
}
