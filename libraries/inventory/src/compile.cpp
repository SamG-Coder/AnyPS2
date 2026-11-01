#include <nt/inventory.hpp>
#include <nt/process.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

namespace nt::inventory {
void compile_inventory(Inventory& inventory,const Options& options) {
    struct Task { std::size_t unit; std::filesystem::path ir; std::string error; };
    std::vector<Task> tasks;
    if (options.jobs>64) throw std::invalid_argument("Compile jobs must be in the range 1..64 or zero for auto");
    inventory.jobs=options.jobs ? options.jobs : std::clamp(std::thread::hardware_concurrency(),1U,4U);
    if (!options.compile) return;
    for (std::size_t unit=0;unit<inventory.units.size();++unit) if (inventory.units[unit].stage>=Stage::llvm_generated) {
        inventory.units[unit].stage=Stage::llvm_generated;
        inventory.units[unit].compiled_objects=0;
        for (const auto& module:inventory.units[unit].modules) tasks.push_back({unit,module,{}});
    }
    std::erase_if(inventory.gaps,[](const auto& gap) { return gap.subsystem=="llvm-compile"; });
    if (tasks.empty()) return;
    const auto start=std::chrono::steady_clock::now();
    std::atomic<std::size_t> next{0};
    const auto worker=[&] {
        for (auto index=next.fetch_add(1);index<tasks.size();index=next.fetch_add(1)) {
            auto& task=tasks[index];
            auto object=task.ir; object.replace_extension(".obj");
            try {
                const auto status=run_process({options.compiler.string(),"-target","x86_64-pc-windows-msvc",
                    "-Werror","-Wno-override-module","-O2","-c",task.ir.string(),"-o",object.string()});
                if (status) task.error="Compiler exited with status "+std::to_string(status);
                else if (!std::filesystem::is_regular_file(object) || !std::filesystem::file_size(object))
                    task.error="Compiler returned success without an object file";
            } catch (const std::exception& error) { task.error=error.what(); }
        }
    };
    {
        std::vector<std::jthread> workers;
        for (std::size_t index=0;index<std::min<std::size_t>(inventory.jobs,tasks.size());++index)
            workers.emplace_back(worker);
    }
    for (const auto& task:tasks) {
        auto& unit=inventory.units[task.unit];
        if (task.error.empty()) ++unit.compiled_objects;
        else {
            if (!unit.detail.empty()) unit.detail+="; ";
            unit.detail+=task.ir.filename().string()+": "+task.error;
            inventory.gaps.push_back({"llvm-compile",task.error,task.ir.generic_string(),unit.id,1,0});
        }
    }
    for (auto& unit:inventory.units)
        if (!unit.modules.empty() && unit.compiled_objects==unit.modules.size()) unit.stage=Stage::machine_compiled;
    inventory.compilation_seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
}
}
