#include <nt/host_metrics.hpp>
#include <iostream>

int main(int argc,char**) {
    if (argc!=1) { std::cerr<<"Usage: anyps2-host-metrics\n"; return 2; }
    try {
        const auto sources=nt::host::profile_sources();
        std::wcout<<L"Windows profiling sources: "<<sources.size()<<L"\n";
        for (const auto& source:sources)
            std::wcout<<source.id<<L" "<<source.description<<L" interval="
                      <<source.minimum_interval<<L".."<<source.maximum_interval<<L"\n";
        std::wcout<<L"Read-only enumeration. No counter collection enabled.\n"
                    L"Source availability does not establish synchronous per-thread reads or PS2 metric equivalence.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<"anyps2-host-metrics: "<<error.what()<<'\n'; return 1; }
}
