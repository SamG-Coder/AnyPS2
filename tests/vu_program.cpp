#include <nt/vu_program.hpp>
#include <nt/vu_arithmetic.hpp>
#include <nt/vu_lower.hpp>
#include <nt/vu_upload.hpp>
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value) { if (!value) throw std::runtime_error("VU framing regression"); }
template<class F> void rejects(F call) {
    bool failed=false;
    try { call(); } catch (const std::invalid_argument&) { failed=true; }
    check(failed);
}
}
int main() {
    try {
        // Original synthetic encodings exercise framing, not operation semantics.
        std::array<std::uint8_t,16> bytes{};
        const auto put=[&](unsigned at,std::uint32_t value) {
            for (unsigned byte=0;byte<4;++byte) bytes[at+byte]=static_cast<std::uint8_t>(value>>(byte*8));
        };
        put(0,0xdeadbeef); put(4,0xf8000000U|(5U<<21)|(7U<<16)|(3U<<11)|(2U<<6)|42U);
        put(8,0x80000335); put(12,0x000002ff);
        const auto result=nt::vu::frame(bytes,0,510);
        check(result.size()==2 && result[0].address==510 && result[1].address==511);
        check(result[0].immediate==0xdeadbeef && !result[0].lower);
        check(result[0].end && result[0].interlock && result[0].debug && result[0].trap);
        check(result[0].upper.function==42 && result[0].upper.mask==5 && result[0].upper.source==3 && result[0].upper.target==7);
        check(!result[1].immediate && result[1].lower && result[1].lower->primary==64 && result[1].lower->function==53);
        check(result[1].upper.function==63 && result[1].upper.subfunction==11);
        check(nt::vu::frame(bytes,1,2046).size()==2);
        rejects([&] { nt::vu::frame(bytes,2,0); });
        rejects([&] { nt::vu::frame(bytes,0,511); });
        rejects([&] { nt::vu::frame(bytes,1,2047); });
        rejects([&] { nt::vu::frame({},0,0); });
        rejects([&] { nt::vu::frame(nt::ByteView(bytes).first(15),0,0); });
        {
            std::array<std::uint8_t,80> code{};
            const auto instruction=[&](unsigned index,std::uint32_t lower,std::uint32_t upper=0x2ff) {
                for (unsigned byte=0;byte<4;++byte) {
                    code[index*8+byte]=static_cast<std::uint8_t>(lower>>(byte*8));
                    code[index*8+4+byte]=static_cast<std::uint8_t>(upper>>(byte*8));
                }
            };
            for (unsigned index=0;index<10;++index) instruction(index,0x80000335);
            instruction(0,(40U<<25)|(2U<<11)|(3U<<16)|3); // Either pair 4 or pair 2, after pair 1.
            instruction(2,(32U<<25)|3); // To pair 6; pair 3 is a slot only.
            instruction(4,(33U<<25)|(15U<<16)|3); // Call pair 8; continuation pair 6.
            instruction(6,0x80000335,0x400002ff); // End after pair 7.
            instruction(8,(36U<<25)|(15U<<11)); // Indirect return, slot pair 9.
            auto pairs=nt::vu::frame(code,0,0);
            const std::array entries{0U};
            auto flow=nt::vu::discover(pairs,0,entries);
            check(flow.reachable.size()==10 && flow.transfers.size()==5 && flow.missing.empty());
            check(flow.indirect==std::set<unsigned>{8} && flow.complex_delay.empty());
            const auto branch=nt::vu::control(pairs[0],0);
            check(branch.branch==nt::vu::Branch::equal && branch.target==4 && branch.source==2 && branch.target_register==3);
            check(nt::vu::control(pairs[4],0).link_register==15);
            // Branch-like immediate data is never a lower branch.
            instruction(0,(32U<<25)|3,0x800002ff);
            pairs=nt::vu::frame(code,0,0); check(nt::vu::control(pairs[0],0).branch==nt::vu::Branch::none);
            // Missing slot is reported instead of inventing a terminal edge.
            const std::array last_entry{6U};
            flow=nt::vu::discover(std::span(pairs).first(7),0,last_entry);
            check(flow.missing==std::set<unsigned>{7});
            // A branch in a branch delay slot remains explicitly unresolved.
            instruction(0,32U<<25); instruction(1,32U<<25);
            pairs=nt::vu::frame(code,0,0); flow=nt::vu::discover(pairs,0,entries);
            check(flow.complex_delay==std::set<unsigned>{0});
            // Signed displacements wrap in each unit's pair-address space.
            instruction(0,(32U<<25)|0x7fe); pairs=nt::vu::frame(code,0,0);
            check(nt::vu::control(pairs[0],0).target==511 && nt::vu::control(pairs[0],1).target==2047);
            instruction(1,0x80000335); pairs=nt::vu::frame(code,0,0);
            flow=nt::vu::discover(pairs,0,entries); check(flow.missing==std::set<unsigned>{511});
            // A backwards loop terminates discovery, not native execution.
            instruction(0,(32U<<25)|0x7ff); pairs=nt::vu::frame(code,0,0);
            flow=nt::vu::discover(pairs,0,entries); check(flow.reachable.size()==2 && flow.missing.empty());
            pairs.push_back(pairs.front());
            rejects([&] { nt::vu::discover(pairs,0,entries); });
            rejects([&] { nt::vu::discover({},0,{}); });
        }
        {
            using namespace nt::vu;
            const auto decode=[&](unsigned function,unsigned destination=9) {
                put(0,0x80000335);
                put(4,(10U<<21)|(7U<<16)|(3U<<11)|(destination<<6)|function);
                return decode_upper(frame(nt::ByteView(bytes).first(8),0,0).front().upper);
            };
            const auto vector=decode(42);
            check(vector.operation==Arithmetic::multiply && vector.destination==Destination::vector && vector.result==9);
            check(vector.left==3 && vector.right==7 && vector.mask==10 && vector.operand==Operand::vector);
            for (unsigned lane=0;lane<4;++lane) {
                const auto broadcast=decode(8+lane);
                check(broadcast.operation==Arithmetic::multiply_add && static_cast<unsigned>(broadcast.operand)==static_cast<unsigned>(Operand::x)+lane);
                const auto accumulator=decode(60+lane,2);
                check(accumulator.operation==Arithmetic::multiply_add && accumulator.destination==Destination::accumulator && accumulator.operand==broadcast.operand);
                const auto conversion=decode(60+lane,4);
                check(conversion.operation==Arithmetic::integer_to_float && conversion.result==7 && conversion.left==3 && conversion.operand==Operand::none);
                check(conversion.scale==std::array<unsigned,4>{0,4,12,15}[lane]);
                check(decode(60+lane,5).operation==Arithmetic::float_to_integer);
            }
            check(decode(28).operand==Operand::quotient && decode(30).operand==Operand::immediate);
            check(decode(35).operation==Arithmetic::multiply_add && decode(35).operand==Operand::immediate);
            check(decode(37).operation==Arithmetic::multiply_subtract && decode(37).operand==Operand::quotient);
            check(decode(46).operation==Arithmetic::outer_subtract);
            check(decode(62,11).operation==Arithmetic::outer_multiply && decode(62,11).destination==Destination::accumulator);
            check(decode(61,7).operation==Arithmetic::absolute && decode(61,7).result==7);
            check(decode(63,7).operation==Arithmetic::clip && decode(63,7).destination==Destination::none);
            check(decode(63,11).operation==Arithmetic::nop && decode(63,11).destination==Destination::none);
            for (unsigned reserved=48;reserved<60;++reserved) check(decode(reserved).operation==Arithmetic::unsupported);
            check(decode(63,10).operation==Arithmetic::unsupported && decode(60,31).operation==Arithmetic::unsupported);
        }
        {
            using namespace nt::vu;
            const auto decode=[&](std::uint32_t lower,std::uint32_t upper=0x2ff) {
                put(0,lower); put(4,upper);
                return decode_lower(frame(nt::ByteView(bytes).first(8),0,0).front(),0);
            };
            const auto literal=decode(0xdeadbeef,0x800002ff);
            check(literal.operation==LowerOperation::literal && literal.literal==0xdeadbeef && literal.result.bank==Bank::immediate);
            for (const unsigned function:{48U,49U,52U,53U}) {
                const auto op=decode(0x80000000|(2U<<11)|(3U<<16)|(4U<<6)|function);
                check(op.operation!=LowerOperation::unsupported && op.left.bank==Bank::integer && op.left.index==2 && op.right.index==3 && op.result.index==4);
            }
            const auto signed_add=decode(0x80000000|(2U<<11)|(3U<<16)|(16U<<6)|50);
            check(signed_add.operation==LowerOperation::integer_add_immediate && signed_add.offset==-16 && signed_add.result.index==3 && signed_add.right.bank==Bank::none);
            const auto unsigned_add=decode((8U<<25)|(15U<<21)|(2U<<11)|(3U<<16)|0x7ff);
            check(unsigned_add.offset==32767 && unsigned_add.result.index==3);
            check(decode(9U<<25).operation==LowerOperation::integer_subtract_immediate);
            const auto move=decode(0x80000000|(10U<<21)|(7U<<16)|(3U<<11)|(12U<<6)|60);
            check(move.operation==LowerOperation::move && move.mask==10 && move.left.bank==Bank::vector && move.left.index==3 && move.result.index==7);
            check(decode(0x80000000|(12U<<6)|61).operation==LowerOperation::rotate);
            for (unsigned variant=0;variant<4;++variant) {
                const auto op=decode(0x80000000|(2U<<21)|(1U<<23)|(7U<<16)|(3U<<11)|(14U<<6)|(60+variant));
                if (variant==3) {
                    check(op.operation==LowerOperation::wait_quotient && op.left.bank==Bank::quotient && op.result.bank==Bank::none);
                } else {
                    check(op.result.bank==Bank::quotient && op.right.index==7 && op.right.component==1 && op.mask==0);
                    check(variant==1 ? op.left.bank==Bank::none : op.left.index==3 && op.left.component==2);
                }
            }
            const auto to_integer=decode(0x80000000|(2U<<21)|(7U<<16)|(3U<<11)|(15U<<6)|60);
            check(to_integer.operation==LowerOperation::vector_to_integer && to_integer.left.component==2 && to_integer.result.bank==Bank::integer);
            const auto to_vector=decode(0x80000000|(10U<<21)|(7U<<16)|(3U<<11)|(15U<<6)|61);
            check(to_vector.operation==LowerOperation::integer_to_vector && to_vector.left.bank==Bank::integer && to_vector.mask==10);
            check(decode(33U<<25).operation==LowerOperation::branch);
            check(decode(0x80000033).operation==LowerOperation::unsupported);
        }
        {
            nt::Bytes packet;
            const auto append=[&](std::uint32_t word) {
                for (unsigned byte=0;byte<4;++byte) packet.push_back(static_cast<std::uint8_t>(word>>(byte*8)));
            };
            append(0x30000000); // Row payload can resemble commands.
            for (unsigned i=0;i<4;++i) append(0x4a000000);
            append(0x4a010000); append(0x4a000000); append(0x14000000);
            append(0x4a010001); append(0); append(0);
            const auto boundary=packet.size();
            append(0x14000000); // Launch stops extraction before later data.
            append(0x4a010002); append(0); append(0);
            const auto sequence=nt::vu::extract_uploads(packet,0);
            check(sequence.uploads.size()==2 && sequence.uploads[0].destination==0 &&
                sequence.uploads[1].destination==1 && sequence.uploads[0].bytes.size()==8 &&
                sequence.consumed==boundary && sequence.stopped_command==0x14);
            const auto image=nt::vu::assemble_uploads(sequence.uploads,0);
            check(image.size()==1 && image[0].destination==0 && image[0].bytes.size()==16 && image[0].bytes[3]==0x4a);
            const std::array<nt::elf::VectorOverlay,3> declared{{
                {"first",24,0,8,24},{"second",36,8,8,36},{"after-launch",52,16,8,52}}};
            const auto groups=nt::vu::recover_declared_uploads(packet,declared);
            check(groups.size()==2 && groups[0].image.bytes==image[0].bytes && groups[0].sources.size()==2 &&
                groups[1].image.destination==2 && groups[1].image.bytes.size()==8);
            auto overwritten=sequence.uploads;
            overwritten.push_back({0,nt::Bytes(8,0xab)});
            overwritten.push_back({4,nt::Bytes(8,0xcd)});
            const auto sparse=nt::vu::assemble_uploads(overwritten,0);
            check(sparse.size()==2 && sparse[0].bytes.size()==16 && sparse[0].bytes[0]==0xab &&
                sparse[1].destination==4 && sparse[1].bytes[0]==0xcd);
            packet.resize(boundary-1);
            rejects([&] { nt::vu::extract_uploads(packet,0); });
            packet.resize(boundary-4);
            rejects([&] { nt::vu::extract_uploads(packet,0); });
            packet.clear(); append(0x4a000000);
            packet.resize(4+2048);
            check(nt::vu::extract_uploads(packet,0).uploads[0].bytes.size()==2048);
            packet[0]=1; packet[1]=1; // Destination 257 plus 256 pairs exceeds VU0.
            rejects([&] { nt::vu::extract_uploads(packet,0); });
            check(nt::vu::extract_uploads(packet,1).uploads[0].destination==257);
            packet[3]|=0x80;
            check(nt::vu::extract_uploads(packet,0).uploads.empty());
        }
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
