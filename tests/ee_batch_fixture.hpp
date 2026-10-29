#pragma once
#include <array>
#include <cstdint>
#include <string_view>
#include <stdexcept>

namespace ee_batch {
struct Case { std::string_view name; std::uint32_t word; };
inline constexpr std::array packed{
    Case{"pmulth",0x71095709U},
    Case{"pmaddh",0x71095409U},
    Case{"pmsubh",0x71095509U},
    Case{"phmadh",0x71095449U},
    Case{"phmsbh",0x71095549U},
    Case{"pdivbw",0x71095749U},

    Case{"pmfhl",0x70005030U},
    Case{"pmfhl",0x70005070U},
    Case{"pmfhl",0x700050b0U},
    Case{"pmfhl",0x700050f0U},
    Case{"pmfhl",0x70005130U},
    Case{"pmthl",0x71000031U},
    Case{"paddw",0x71095008U},
    Case{"psubw",0x71095048U},
    Case{"pcgtw",0x71095088U},
    Case{"pmaxw",0x710950c8U},
    Case{"pminw",0x710950e8U},
    Case{"pabsw",0x71095068U},
    Case{"paddsw",0x71095408U},
    Case{"psubsw",0x71095448U},
    Case{"padduw",0x71095428U},
    Case{"psubuw",0x71095468U},
    Case{"paddh",0x71095108U},
    Case{"psubh",0x71095148U},
    Case{"pcgth",0x71095188U},
    Case{"pmaxh",0x710951c8U},
    Case{"pminh",0x710951e8U},
    Case{"pabsh",0x71095168U},
    Case{"paddsh",0x71095508U},
    Case{"psubsh",0x71095548U},
    Case{"padduh",0x71095528U},
    Case{"psubuh",0x71095568U},
    Case{"paddb",0x71095208U},
    Case{"psubb",0x71095248U},
    Case{"pcgtb",0x71095288U},
    Case{"paddsb",0x71095608U},
    Case{"psubsb",0x71095648U},
    Case{"paddub",0x71095628U},
    Case{"psubub",0x71095668U},
    Case{"padsbh",0x71095128U},
    Case{"psllvw",0x71095089U},
    Case{"psrlvw",0x710950c9U},
    Case{"psravw",0x710950e9U},
    Case{"pexeh",0x71095689U},
    Case{"prevh",0x710956c9U},
    Case{"pexew",0x71095789U},
    Case{"prot3w",0x710957c9U},
    Case{"pexch",0x710956a9U},
    Case{"pexcw",0x710957a9U},
    Case{"pinth",0x71095289U},
    Case{"pinteh",0x710952a9U},
    Case{"qfsrv",0x710956e8U},
    Case{"psllh",0x710957f4U},
    Case{"psrlh",0x710957f6U},
    Case{"psrah",0x710957f7U},
    Case{"psllw",0x710957fcU},
    Case{"psrlw",0x710957feU},
    Case{"psraw",0x710957ffU},
    Case{"madd",0x71095000U},
    Case{"maddu",0x71095001U},
    Case{"madd1",0x71095020U},
    Case{"maddu1",0x71095021U},
    Case{"pmultw",0x71095309U},
    Case{"pdivw",0x71095349U},
    Case{"pmultuw",0x71095329U},
    Case{"pdivuw",0x71095369U},
    Case{"pmadduw",0x71095029U},
    Case{"pmfhi",0x71095209U},
    Case{"pmflo",0x71095249U},
    Case{"pmthi",0x71095229U},
    Case{"pmtlo",0x71095269U}
};
inline constexpr std::uint32_t packed_base=0x300000;
inline constexpr std::array<unsigned,4> destinations{10,8,9,0};
inline std::uint32_t packed_pc(std::size_t index,unsigned variant=0) {
    return packed_base+static_cast<std::uint32_t>(index)*32+variant*8;
}
inline constexpr std::array scalar{
    Case{"sqrt_s",0x46030104},
    Case{"cvt_s_w",0x46801120}, Case{"cvt_w_s",0x46001124},
    Case{"min_s",0x46031129}, Case{"max_s",0x46031128},
    Case{"div_s",0x46031103}, Case{"suba_s",0x46031019}, Case{"mula_s",0x4603101a},
    Case{"mtsa",0x01000029}, Case{"eret",0x42000018},
    Case{"cache_writeback_invalidate",0xbc980000}, Case{"cache_invalidate",0xbc9a0000}, Case{"cache_writeback",0xbc9c0000},
    Case{"wired_write",0x40883000}, Case{"wired_read",0x40093000},
    Case{"cache_tag",0xbc900000}, Case{"tag_read",0x4009e000}, Case{"cache_index_flush",0xbc940000},
    Case{"cache_instruction_flush",0xbc870000}, Case{"cache_branch_flush",0xbc8c0000}
};
inline constexpr std::uint32_t scalar_base=0x310000;
inline std::uint32_t scalar_pc(std::string_view name) {
    for (std::size_t i=0;i<scalar.size();++i) if (scalar[i].name==name) return scalar_base+static_cast<std::uint32_t>(i)*8;
    throw std::logic_error("Missing scalar fixture");
}
inline constexpr std::array vector{
    Case{"qmfc2",0x48281000}, Case{"qmtc2",0x48a81000},
    Case{"qmfc2_wait",0x48281001}, Case{"qmtc2_wait",0x48a81001},
    Case{"vmul",0x4be3112a}, Case{"vmul_bc",0x4be3111b},
    Case{"vmax",0x4be3112b}, Case{"vmini",0x4be3112f},
    Case{"vmax_bc",0x4be31111}, Case{"vmini_bc",0x4be31116},
    Case{"vabs",0x4be411fd}, Case{"vnop",0x4a0002ff},
    Case{"vitof0",0x4be4113c}, Case{"vitof4",0x4be4113d},
    Case{"vitof12",0x4be4113e}, Case{"vitof15",0x4be4113f},
    Case{"vftoi0",0x4be4117c}, Case{"vftoi4",0x4be4117d},
    Case{"vftoi12",0x4be4117e}, Case{"vftoi15",0x4be4117f}
};
inline constexpr std::uint32_t vector_base=0x320000;
// Each vector instruction has four masks and four destination aliases.
inline std::uint32_t vector_pc(std::size_t index,unsigned mask_variant=0,unsigned destination_variant=0) {
    return vector_base+static_cast<std::uint32_t>(index)*128+mask_variant*32+destination_variant*8;
}
inline constexpr std::array trap_functions{48U,49U,50U,51U,52U,54U};
inline constexpr std::array trap_immediates{8U,9U,10U,11U,12U,14U};
inline constexpr std::uint32_t trap_base=0x340000;
inline constexpr std::uint32_t prefetch_pc=0x340100;
inline constexpr std::uint32_t trap_delay_pc=0x340110;
inline constexpr std::uint32_t branch_base=0x330000;
}
