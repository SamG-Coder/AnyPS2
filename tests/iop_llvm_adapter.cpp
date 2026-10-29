#include <nt/iop_llvm.hpp>

nt::iop::NativeResult nt_iop_generated_run(nt::iop::Cpu& cpu,std::span<std::uint8_t> ram,std::uint64_t budget) {
    return nt::iop::run_llvm(cpu,ram,budget);
}
