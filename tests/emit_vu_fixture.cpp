#include <nt/vu_codegen.hpp>
#include <array>
#include <sstream>
#include <stdexcept>

int main(int argc,char** argv) {
    if (argc!=2) return 2;
    nt::Bytes bytes;
    const auto pair=[&](std::uint32_t lower,std::uint32_t upper=0x2ff) {
        for (const auto word:{lower,upper}) for (unsigned byte=0;byte<4;++byte)
            bytes.push_back(static_cast<std::uint8_t>(word>>(byte*8)));
    };
    pair(0x80000000|(1U<<11)|(3U<<16)|(2U<<6)|48); // VI2 = VI1 + VI3.
    pair(0x80000000|(15U<<21)|(3U<<16)|(1U<<11)|(12U<<6)|60,
        (15U<<21)|(1U<<16)|(2U<<11)|(7U<<6)|61); // ABS VF1 + MOVE VF3 from old VF1.
    pair(0x80000000|(15U<<21)|(3U<<16)|(3U<<11)|(12U<<6)|61); // In-place rotation.
    pair(0x80000000|(2U<<11)|(2U<<16)|(30U<<6)|50); // VI2 -= 2.
    pair(0x80000000|(15U<<21)|(4U<<16)|(2U<<11)|(15U<<6)|61); // Signed integer broadcast.
    pair(0x12345678,0x800002ff); // I constant.
    pair(0x80000335,(15U<<21)|(6U<<16)|(5U<<11)|(4U<<6)|61); // ITOF4.
    pair(0x80000335,(15U<<21)|(7U<<16)|(6U<<11)|(5U<<6)|61); // FTOI4.
    pair(0x80000335,(15U<<21)|(10U<<16)|(9U<<11)|(8U<<6)|43); // MAX.
    pair(0x80000335,(10U<<21)|(10U<<16)|(9U<<11)|(12U<<6)|21); // Masked MINy.
    pair(0x3f000000,0x80000000|(15U<<21)|(9U<<11)|(11U<<6)|29); // MAXi reads old I.
    pair(0x80000335,(15U<<21)|(15U<<16)|(14U<<11)|(16U<<6)|40); // ADD.
    pair(0x80000335,(5U<<21)|(15U<<16)|(14U<<11)|(17U<<6)|4); // Masked SUBx.
    pair(0x80000335,(10U<<21)|(14U<<11)|(7U<<6)|62); // Masked MULAi.
    pair(0x80000335,(13U<<21)|(15U<<16)|(14U<<11)|(18U<<6)|41); // MADD, preserving z.
    pair(0x80000335,(8U<<21)|(15U<<16)|(14U<<11)|(19U<<6)|45); // MSUB x.
    pair(0x80000335,(8U<<21)|(15U<<16)|(14U<<11)|(2U<<6)|60); // MADDAx.
    pair(0x80000335,(8U<<21)|(14U<<11)|(9U<<6)|63); // MSUBAi.
    pair(0x80000335,(15U<<21)|(15U<<16)|(14U<<11)|42); // MUL to VF0 still updates flags.
    pair(0x80000000|(3U<<21)|(5U<<16)|(0U<<11)|(15U<<6)|60,0x400002ff); // VF0.w -> VI5, E.
    pair(0x80000000|(5U<<11)|(5U<<16)|(1U<<6)|50); // End delay slot increments VI5.
    const auto program=nt::vu::frame(bytes,0,0);
    std::ostringstream output; nt::vu::emit_llvm(output,program,0);
    auto unsupported=program; unsupported[0].upper.function=48;
    std::ostringstream rejected; bool failed=false;
    try { nt::vu::emit_llvm(rejected,unsupported,0); } catch (const std::invalid_argument&) { failed=true; }
    if (!failed || !rejected.str().empty()) return 1;
    const auto save_block=[&](const char* suffix,const char* symbol,unsigned destination) {
        std::ostringstream block;
        nt::vu::emit_llvm(block,nt::vu::frame(bytes,0,destination),0,symbol);
        const auto text=block.str();
        nt::write_file(std::string(argv[1])+suffix,nt::ByteView(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));
    };
    bytes.clear();
    pair(0x80000335,(15U<<21)|(2U<<16)|(1U<<11)|(3U<<6)|42); // MUL VF3.
    pair(0x80000335,0x40000000|(15U<<21)|(2U<<16)|(1U<<11)|(4U<<6)|41); // MADD VF4, E.
    pair(0x80000335);
    save_block(".range","nt_vu_range",0);
    bytes.clear();
    pair(0x80000000|(1U<<11)|(1U<<16)|(1U<<6)|50); // VI1 += 1.
    pair((40U<<25)|(1U<<11)|(2U<<16)|3,(8U<<21)|(5U<<16)|(4U<<11)|(6U<<6)|40); // IBEQ sees old VI1; upper ADD.
    pair(0x80000000|(3U<<11)|(3U<<16)|(1U<<6)|50); // Always executed slot.
    save_block(".branch","nt_vu_branch",0);
    bytes.clear();
    pair((37U<<25)|(1U<<11)|(1U<<16)); // JALR captures old VI1 then writes link into VI1.
    pair(0x80000000|(1U<<11)|(3U<<6)|48); // Slot reads new link.
    save_block(".call","nt_vu_call",100);
    bytes.clear();
    pair(0x80000000|(4U<<11)|(4U<<16)|(1U<<6)|50,0x400002ff);
    pair(0x80000335);
    save_block(".end_false","nt_vu_end_false",3);
    save_block(".end_true","nt_vu_end_true",5);
    bytes.clear();
    pair(0x80000000|(1U<<11)|(2U<<16)|(14U<<6)|60); // DIV Q, VF1.x, VF2.x.
    pair((32U<<25)|198); // Transfer to pair 200 while Q is pending.
    pair(0x80000335);
    save_block(".q_start","nt_vu_q_start",0);
    bytes.clear();
    const auto waitq=0x80000000U|(14U<<6)|63;
    const auto mulq=[](unsigned destination) { return (8U<<21)|(3U<<11)|(destination<<6)|28; };
    pair(waitq,mulq(4));
    pair(0x80000000|(2U<<16)|(14U<<6)|61); // SQRT Q, VF2.x.
    pair(waitq,mulq(5));
    pair(0x80000000|(1U<<11)|(2U<<16)|(14U<<6)|62,0x400002ff); // RSQRT and E.
    pair(waitq,mulq(6));
    save_block(".q_finish","nt_vu_q_finish",200);
    bytes.clear();
    pair(0x80000335,0x40000000|mulq(4)); // Reading pending Q without synchronization stops.
    pair(0x80000335);
    save_block(".q_early","nt_vu_q_early",200);
    bytes.clear();
    for (unsigned i=0;i<4;++i) pair(0x80000335);
    pair(0x80000335,0x40000000|mulq(4));
    pair(0x80000335);
    save_block(".q_elapsed","nt_vu_q_elapsed",200);
    bytes.clear();
    for (unsigned operation=0;operation<3;++operation) {
        pair(0x80000000|(1U<<11)|(2U<<16)|(14U<<6)|(60+operation));
        for (unsigned i=1;i<(operation==2 ? 13U : 7U);++i) pair(0x80000335);
        pair(0x80000335,mulq(4+operation));
    }
    pair(0x80000335,0x400002ff); pair(0x80000335);
    save_block(".q_scheduled","nt_vu_q_scheduled",0);
    bytes.clear();
    pair((40U<<25)|(1U<<11)|(2U<<16)|3); // Conditional branch to 4; otherwise 2.
    pair(0x80000000|(3U<<11)|(3U<<16)|(1U<<6)|50);
    pair(0x80000000|(4U<<11)|(4U<<16)|(1U<<6)|50,0x400002ff);
    pair(0x80000335);
    pair(0x80000000|(5U<<11)|(5U<<16)|(1U<<6)|50,0x400002ff);
    pair(0x80000335);
    const std::array<unsigned,1> entries{0};
    const auto multi=nt::vu::frame(bytes,0,0);
    std::ostringstream module;
    nt::vu::emit_program_llvm(module,multi,0,entries);
    const auto module_text=module.str();
    nt::write_file(std::string(argv[1])+".program",nt::ByteView(reinterpret_cast<const std::uint8_t*>(module_text.data()),module_text.size()));
    std::ostringstream module1;
    auto alternative=bytes;
    alternative[4*8]^=0xc0; // VI5 increment immediate 1 -> 2, same destination.
    const std::array images{nt::vu::frame(bytes,1,0),nt::vu::frame(alternative,1,0)};
    nt::vu::emit_catalog_llvm(module1,images,1,"nt_vu_program1");
    const auto module1_text=module1.str();
    nt::write_file(std::string(argv[1])+".program1",nt::ByteView(reinterpret_cast<const std::uint8_t*>(module1_text.data()),module1_text.size()));
    auto invalid_multi=multi;
    invalid_multi[4].upper.function=48;
    std::ostringstream partial;
    bool rejected_module=false;
    try { nt::vu::emit_program_llvm(partial,invalid_multi,0,entries); }
    catch (const std::invalid_argument&) { rejected_module=true; }
    if (!rejected_module || !partial.str().empty()) return 1;
    for (const bool complex:{false,true}) {
        auto invalid=multi;
        if (complex) invalid[1].end=true;
        else invalid.pop_back();
        std::ostringstream rejected;
        bool failed=false;
        try { nt::vu::emit_program_llvm(rejected,invalid,0,entries); }
        catch (const std::invalid_argument&) { failed=true; }
        if (!failed || !rejected.str().empty()) return 1;
    }
    const auto text=output.str();
    bytes.clear();
    pair(0x80000000|(10U<<21)|(1U<<11)|(1U<<16)|(13U<<6)|61); // SQI.xz.
    pair(0x80000000|(15U<<21)|(1U<<11)|(2U<<16)|(13U<<6)|62); // LQD.
    pair(0x80000000|(1U<<21)|(3U<<16)|(13U<<6)|63); // SQD.w VF0.
    pair(0x80000000|(15U<<21)|(3U<<11)|(3U<<16)|(13U<<6)|60); // LQI.
    pair((1U<<25)|(15U<<21)|(4U<<11)|256); // SQ at wrapped offset.
    pair((15U<<21)|(5U<<16)|256); // LQ at wrapped offset.
    pair((15U<<21)|(2U<<11)); // LQ to VF0 does not access mapped memory.
    pair((15U<<21)|(2U<<11)|(6U<<16)|0x7ff); // Signed displacement -1.
    pair((5U<<25)|(10U<<21)|(8U<<16)|1); // ISW.xz, zero extends VI8 into selected lanes.
    pair((4U<<25)|(10U<<21)|(6U<<16)|1); // ILW.xz, last selected lane wins.
    pair(0x80000000|(15U<<21)|(7U<<16)|(15U<<6)|62); // ILWR all lanes, selects w.
    pair(0x80000000|(1U<<21)|(8U<<16)|(15U<<6)|63); // ISWR.w.
    pair((1U<<25)|(8U<<21)|(1U<<11)|(2U<<16),0x400002ff); // Late store may fail.
    pair(0x80000335);
    save_block(".memory","nt_vu_memory",0);
    std::ostringstream memory1;
    nt::vu::emit_llvm(memory1,nt::vu::frame(bytes,1,0),1,"nt_vu_memory1");
    const auto memory1_text=memory1.str();
    nt::write_file(std::string(argv[1])+".memory1",nt::ByteView(reinterpret_cast<const std::uint8_t*>(memory1_text.data()),memory1_text.size()));
    bytes.clear();
    pair(0x80000000|(2U<<21)|(1U<<16)|(15U<<6)|62); // ILWR.z VI1.
    pair((40U<<25)|(1U<<11)|(2U<<16)|3);
    pair(0x80000335);
    save_block(".integer_branch","nt_vu_integer_branch",0);
    bytes.clear();
    pair(0x80000000|(1U<<16)|(26U<<6)|60); // XTOP VI1.
    pair(0x80000000|(2U<<16)|(26U<<6)|61); // XITOP VI2.
    pair(0x80000335,(2U<<16)|(1U<<11)|(11U<<6)|62); // OPMULA.xyz, encoded mask ignored.
    pair(0x80000335,0x40000000|(1U<<16)|(2U<<11)|(3U<<6)|46); // OPMSUB reverse operands.
    pair(0x80000335);
    save_block(".geometry","nt_vu_geometry",0);
    bytes.clear();
    pair(0x80000000|(1U<<11)|(28U<<6)|63); // ERLENG.
    pair((32U<<25)|1); // Branch to 3 with pending P.
    pair(0x80000335);
    pair(0x80000000|(30U<<6)|63); // WAITP.
    pair(0x80000000|(10U<<21)|(2U<<16)|(25U<<6)|60,0x400002ff); // MFP.xz + E.
    pair(0x80000335);
    pair(0x80000000|(15U<<21)|(2U<<16)|(25U<<6)|60,0x400002ff); // Unsynchronized alternative entry.
    pair(0x80000335);
    pair(0x80000000|(2U<<21)|(1U<<11)|(31U<<6)|60); // ESIN VF1.z.
    pair(0x80000000|(30U<<6)|63); // WAITP.
    pair(0x80000000|(8U<<21)|(2U<<16)|(25U<<6)|60,0x400002ff);
    pair(0x80000335);
    pair((40U<<25)|(1U<<11)|4); // Pair 12: VI1==0 selects delay pair 17, otherwise 14.
    pair((41U<<25)|(2U<<11)|6); // Pair 13: VI2!=0 selects 20 after the selected delay.
    pair((8U<<25)|(3U<<11)|(3U<<16)|1); // Pair 14 increments VI3 by 1.
    pair(0x80000335,0x400002ff); pair(0x80000335);
    pair((8U<<25)|(3U<<11)|(3U<<16)|2); // Pair 17 increments VI3 by 2.
    pair(0x80000335,0x400002ff); pair(0x80000335);
    pair(0x80000335,0x400002ff); pair(0x80000335);
    std::ostringstream p_module;
    pair(0x80000000|(3U<<21)|(1U<<11)|(30U<<6)|60); // ESQRT VF1.w.
    pair(0x80000000|(30U<<6)|63);
    pair(0x80000000|(8U<<21)|(2U<<16)|(25U<<6)|60,0x400002ff);
    pair(0x80000335);
    const std::array<unsigned,5> p_entries{0,6,8,12,22};
    nt::vu::emit_program_llvm(p_module,nt::vu::frame(bytes,1,0),1,p_entries,"nt_vu_p");
    const auto p_text=p_module.str();
    nt::write_file(std::string(argv[1])+".p",nt::ByteView(reinterpret_cast<const std::uint8_t*>(p_text.data()),p_text.size()));
    bytes.clear();
    pair(0x80000335,(8U<<21)|(1U<<6)|40); // ADD.x VF1 = VF0 + VF0, MAC zero bit x.
    pair((32U<<25)|1);
    pair(0x80000335);
    pair(0x80000335);
    pair((26U<<25)|(1U<<11)|(2U<<16));
    pair((27U<<25)|(3U<<11)|(4U<<16));
    pair((24U<<25)|(2U<<11)|(5U<<16),0x400002ff);
    pair(0x80000335);
    pair((26U<<25)|(1U<<11)|(2U<<16),0x400002ff); // Too-early alternative.
    pair(0x80000335);
    pair((22U<<25)|(1U<<21)|(6U<<16)|0x41); // FSAND, split 12-bit immediate.
    pair((23U<<25)|(1U<<21)|(7U<<16)|2); // FSOR.
    pair((20U<<25)|(1U<<21)|(8U<<16)|0x41,0x400002ff); // FSEQ + E.
    pair(0x80000335);
    std::ostringstream flags_module;
    pair(0x80000335,(8U<<21)|(2U<<16)|(1U<<11)|(3U<<6)|41); // MADD.x, product underflow with normal ACC.
    pair((22U<<25)|(1U<<21)|(9U<<16)|0x7ff); // Early FSAND before flags mature.
    pair((32U<<25)|1); // Split at native block boundary.
    pair(0x80000335);
    pair((22U<<25)|(1U<<21)|(10U<<16)|0x7ff,0x400002ff); // Mature FSAND.
    pair(0x80000335);
    const std::array<unsigned,4> flag_entries{0,8,10,14};
    nt::vu::emit_program_llvm(flags_module,nt::vu::frame(bytes,1,0),1,flag_entries,"nt_vu_flags");
    const auto flags_text=flags_module.str();
    nt::write_file(std::string(argv[1])+".flags",nt::ByteView(reinterpret_cast<const std::uint8_t*>(flags_text.data()),flags_text.size()));
    bytes.clear();
    pair(0x80000335,(8U<<21)|(1U<<6)|40); // zero, negative, positive, zero: four independent producers.
    pair(0x80000335,(8U<<21)|(10U<<16)|(2U<<6)|44);
    pair((32U<<25)|1,(8U<<21)|(10U<<11)|(10U<<16)|(3U<<6)|40);
    pair(0x80000335,(8U<<21)|(4U<<6)|40); // Branch delay pair publishes the fourth result later.
    for (unsigned destination=2;destination<=5;++destination)
        pair((26U<<25)|(1U<<11)|(destination<<16),destination==5 ? 0x400002ff : 0x2ff);
    pair(0x80000335);
    std::ostringstream mac_module;
    nt::vu::emit_program_llvm(mac_module,nt::vu::frame(bytes,0,0),0,entries,"nt_vu_mac_pipeline");
    const auto mac_text=mac_module.str();
    nt::write_file(std::string(argv[1])+".mac_pipeline",nt::ByteView(reinterpret_cast<const std::uint8_t*>(mac_text.data()),mac_text.size()));
    bytes.clear();
    pair(0x80000000|(1U<<11)|(27U<<6)|60); // XGKICK: split before following store.
    pair((1U<<25)|(15U<<21)|(1U<<11),0x400002ff);
    pair(0x80000335);
    std::ostringstream graphics_module;
    nt::vu::emit_program_llvm(graphics_module,nt::vu::frame(bytes,1,0),1,entries,"nt_vu_graphics");
    const auto graphics_text=graphics_module.str();
    nt::write_file(std::string(argv[1])+".graphics",nt::ByteView(reinterpret_cast<const std::uint8_t*>(graphics_text.data()),graphics_text.size()));
    bytes.clear();
    pair(0x80000000|(1U<<11)|(27U<<6)|60);
    pair((1U<<25)|(15U<<21)|(1U<<11)); // Change data between two graphics requests.
    pair(0x80000000|(1U<<11)|(27U<<6)|60,0x400002ff);
    pair(0x80000335);
    std::ostringstream graphics_batch;
    nt::vu::emit_program_llvm(graphics_batch,nt::vu::frame(bytes,1,0),1,entries,"nt_vu_graphics_batch");
    const auto batch_text=graphics_batch.str();
    nt::write_file(std::string(argv[1])+".graphics_batch",nt::ByteView(reinterpret_cast<const std::uint8_t*>(batch_text.data()),batch_text.size()));
    bytes.clear();
    pair(0x80000000|(1U<<11)|(27U<<6)|60,0x400002ff);
    pair(0x80000000|(1U<<11)|(1U<<16)|(1U<<6)|50); // E delay pair increments once.
    const auto end_graphics=nt::vu::frame(bytes,1,0);
    std::ostringstream end_graphics_module;
    nt::vu::emit_program_llvm(end_graphics_module,end_graphics,1,entries,"nt_vu_graphics_end");
    const auto end_graphics_text=end_graphics_module.str();
    nt::write_file(std::string(argv[1])+".graphics_end",nt::ByteView(reinterpret_cast<const std::uint8_t*>(end_graphics_text.data()),end_graphics_text.size()));
    bytes.clear();
    pair(0x80000000|(27U<<6)|60,0x400002ff);
    pair((1U<<25)|(15U<<21)); // A delay-pair store cannot silently alter the outgoing packet.
    std::ostringstream unsafe_graphics;
    bool unsafe_rejected=false;
    try { nt::vu::emit_program_llvm(unsafe_graphics,nt::vu::frame(bytes,1,0),1,entries); }
    catch (const std::invalid_argument&) { unsafe_rejected=true; }
    if (!unsafe_rejected || !unsafe_graphics.str().empty()) return 1;
    bytes.clear();
    const auto clip_upper=(2U<<16)|(1U<<11)|(7U<<6)|63;
    pair(0x80000335,clip_upper); pair(0x80000335,clip_upper);
    pair((32U<<25)|1); pair(0x80000335); pair(0x80000335);
    pair((28U<<25)|(2U<<16)); // FCGET.
    pair((18U<<25)|8); // FCAND.
    pair(0x80000000|(1U<<11)|(3U<<6)|53);
    pair((19U<<25)|0xfffdb6); // FCOR fills the complement of 0x249.
    pair(0x80000000|(1U<<11)|(4U<<6)|53);
    pair((16U<<25)|0x249); // FCEQ.
    pair(0x80000000|(1U<<11)|(5U<<6)|53);
    pair((17U<<25)|0xabcdef); // FCSET.
    pair(0x80000335); pair(0x80000335); pair(0x80000335);
    pair((28U<<25)|(6U<<16),0x400002ff); pair(0x80000335);
    pair((28U<<25)|(2U<<16),0x400002ff); pair(0x80000335); // Early-read alternative.
    std::ostringstream clip_module;
    const std::array<unsigned,2> clip_entries{0,18};
    nt::vu::emit_program_llvm(clip_module,nt::vu::frame(bytes,1,0),1,clip_entries,"nt_vu_clip");
    const auto clip_text=clip_module.str();
    nt::write_file(std::string(argv[1])+".clip",nt::ByteView(reinterpret_cast<const std::uint8_t*>(clip_text.data()),clip_text.size()));
    bytes.clear();
    pair((15U<<21)|(1U<<16),0x400002ff); // Load uploaded data into VF1, then complete.
    pair(0x80000335);
    std::ostringstream data_module;
    nt::vu::emit_program_llvm(data_module,nt::vu::frame(bytes,1,0),1,entries,"nt_vu_data");
    const auto data_text=data_module.str();
    nt::write_file(std::string(argv[1])+".data",nt::ByteView(reinterpret_cast<const std::uint8_t*>(data_text.data()),data_text.size()));
    bytes.clear();
    pair(0x80000000|(1U<<11)|(1U<<16)|(1U<<6)|50,
        0x40000000|(15U<<21)|(2U<<16)|(1U<<11)|(3U<<6)|40);
    pair(0x80000335);
    std::ostringstream shared_module;
    nt::vu::emit_program_llvm(shared_module,nt::vu::frame(bytes,0,0),0,entries,"nt_vu_shared");
    const auto shared_text=shared_module.str();
    nt::write_file(std::string(argv[1])+".shared",nt::ByteView(reinterpret_cast<const std::uint8_t*>(shared_text.data()),shared_text.size()));
    bytes.clear();
    pair(0x80000000|(1U<<11)|(1U<<16)|(1U<<6)|50,0x400002ff);
    pair(0x80000335); // First stop resumes at pair two.
    pair(0x80000000|(27U<<6)|60,0x400002ff); // XGKICK at data zero.
    pair(0x80000000|(1U<<11)|(1U<<16)|(1U<<6)|50);
    pair(0x80000000|(1U<<11)|(1U<<16)|(1U<<6)|50,0x400002ff);
    pair(0x80000335);
    std::ostringstream continuation_module;
    const std::array<unsigned,1> continuation_entries{0};
    nt::vu::emit_program_llvm(continuation_module,nt::vu::frame(bytes,1,0),1,continuation_entries,"nt_vu_continuation",2);
    std::ostringstream missing_continuation;
    bool missing_rejected=false;
    try {
        nt::vu::emit_program_llvm(missing_continuation,nt::vu::frame(bytes,1,0),1,
            continuation_entries,"nt_vu_missing_continuation",3);
    } catch (const std::invalid_argument& error) {
        missing_rejected=std::string(error.what()).find("missing pair 6")!=std::string::npos;
    }
    if (!missing_rejected || !missing_continuation.str().empty()) return 1;
    const auto continuation_text=continuation_module.str();
    nt::write_file(std::string(argv[1])+".continuation",nt::ByteView(reinterpret_cast<const std::uint8_t*>(continuation_text.data()),continuation_text.size()));
    bytes.clear();
    const auto increment=0x80000000U|(1U<<11)|(1U<<16)|(1U<<6)|50;
    pair(0x80000335,0x400002ff); pair(0x80000335);
    pair(increment); pair(increment,0x400002ff); pair(increment);
    pair((36U<<25)|(2U<<11)); pair(0x80000335); // Runtime VI2 chooses a compiled entry.
    pair(increment); // Unterminated tail must execute, then report the image boundary.
    std::ostringstream whole_module;
    nt::vu::emit_program_llvm(whole_module,nt::vu::frame(bytes,0,0),0,{},"nt_vu_whole",0,true);
    const auto whole_text=whole_module.str();
    nt::write_file(std::string(argv[1])+".whole",nt::ByteView(reinterpret_cast<const std::uint8_t*>(whole_text.data()),whole_text.size()));
    for (const bool missing_delay:{false,true}) {
        bytes.clear(); pair(0x80000335,0x400002ff); pair(0x80000335);
        pair(missing_delay ? (36U<<25) : 0x80000033); // Invalid code after a completed routine is not hidden.
        std::ostringstream invalid;
        bool rejected=false;
        try { nt::vu::emit_program_llvm(invalid,nt::vu::frame(bytes,0,0),0,{},"nt_vu_invalid_whole",0,true); }
        catch (const std::invalid_argument&) { rejected=true; }
        if (!rejected || !invalid.str().empty()) return 1;
    }
    nt::write_file(argv[1],nt::ByteView(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));
}
