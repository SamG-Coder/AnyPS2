#pragma once
#include <map>
#include <nt/vu_dispatch.hpp>
#include <memory>
#include <optional>

namespace nt::vu {
struct UploadView {
    unsigned destination;
    std::span<const std::uint8_t> bytes;
};
// LLVM-exported descriptor: native entries and their immutable upload identity.
struct Artifact {
    const Block* blocks;
    std::uint32_t block_count,destination;
    const std::uint8_t* image;
    std::uint32_t image_size;
};

// Binds conversion artifacts to matching uploads. Original bytes are used
// solely for exact identity checks; execution calls the compiled Program only.
class Binding {
public:
    Binding(Program program,unsigned destination,std::span<const std::uint8_t> image)
        : catalog_(std::make_shared<const Catalog>(Catalog{Image(std::move(program),destination,image)})),
          present_{std::vector<bool>(image.size()/8,false)},ready_(1,false),
          memory_((catalog_->front().program.unit() ? 2048U : 512U)*8U),loaded_(memory_.size()/8,false) {}
    Binding(unsigned unit,std::span<const Artifact> artifacts) {
        if (artifacts.empty() || artifacts.size()>4096) throw std::invalid_argument("Invalid VU artifact catalog size");
        Catalog catalog;
        for (const auto& artifact:artifacts) {
            if (!artifact.blocks || !artifact.image || artifact.block_count>2048 || artifact.image_size>16384)
                throw std::invalid_argument("Invalid VU artifact descriptor");
            catalog.emplace_back(Program(unit,{artifact.blocks,artifact.block_count}),artifact.destination,
                std::span<const std::uint8_t>(artifact.image,artifact.image_size));
            present_.emplace_back(artifact.image_size/8,false);
            ready_.push_back(false);
        }
        catalog_=std::make_shared<const Catalog>(std::move(catalog));
        memory_.resize((unit ? 2048U : 512U)*8U);
        loaded_.assign(memory_.size()/8,false);
    }

    // Validate every chunk before changing coverage. Rejecting an upload batch
    // leaves the binding unchanged, just as the caller must leave its packet state.
    // Direct memory writes may also hold bytes no program owns, such as the padding
    // after a program copied with a rounded size. These only remove coverage.
    bool upload(unsigned unit,std::span<const UploadView> uploads,bool allow_unknown=false) {
        if (unit!=this->unit() || uploads.empty()) return false;
        for (const auto& upload:uploads) {
            if (upload.bytes.empty() || upload.bytes.size()%8 ||
                std::uint64_t(upload.destination)+upload.bytes.size()/8>(unit ? 2048U : 512U)) return false;
            const auto matches=[&](const Image& image) {
                if (upload.destination<image.destination) return false;
                const auto offset=std::uint64_t(upload.destination-image.destination)*8;
                return offset<=image.bytes.size() && upload.bytes.size()<=image.bytes.size()-offset &&
                    std::equal(upload.bytes.begin(),upload.bytes.end(),image.bytes.begin()+offset);
            };
            if (!allow_unknown && std::none_of(catalog_->begin(),catalog_->end(),matches) &&
                std::none_of(catalog_->begin(),catalog_->end(),[&](const Image& image) { return part_of(image,upload.bytes); }))
                return false;
        }
        for (const auto& upload:uploads) {
            std::copy(upload.bytes.begin(),upload.bytes.end(),memory_.begin()+upload.destination*8U);
            std::fill_n(loaded_.begin()+upload.destination,upload.bytes.size()/8,true);
        }
        for (const auto& upload:uploads) for (std::size_t index=0;index<catalog_->size();++index) {
            const auto& image=(*catalog_)[index];
            const auto begin=std::max(upload.destination,image.destination);
            const auto end=std::min(std::size_t(upload.destination)+upload.bytes.size()/8,
                std::size_t(image.destination)+image.bytes.size()/8);
            for (auto at=std::size_t(begin);at<end;++at) {
                const auto source=upload.bytes.subspan((at-upload.destination)*8,8);
                present_[index][at-image.destination]=std::equal(source.begin(),source.end(),
                    image.bytes.begin()+(at-image.destination)*8);
            }
        }
        // Coverage changes only here and in reset(). Validate once per upload,
        // not on every native graphics continuation of an unchanged module.
        for (std::size_t i=0;i<present_.size();++i)
            ready_[i]=std::all_of(present_[i].begin(),present_[i].end(),[](bool value) { return value; });
        placements_.clear();
        if (active_ && !(relocated_base_ ? placed(*active_,*relocated_base_) : ready(*active_))) { active_.reset(); relocated_base_.reset(); }
        return true;
    }
    bool ready() const {
        for (std::size_t i=0;i<present_.size();++i) if (ready(i)) return true;
        return false;
    }
    unsigned unit() const { return catalog_->front().program.unit(); }
    void reset() {
        for (auto& coverage:present_) std::fill(coverage.begin(),coverage.end(),false);
        std::fill(ready_.begin(),ready_.end(),false);
        std::fill(loaded_.begin(),loaded_.end(),false);
        placements_.clear();
        active_.reset(); relocated_base_.reset();
    }
    // A continuation resumes from next_pair, which is already in the running
    // program's compiled addresses when that program is relocated.
    RunResult launch(State& state,unsigned entry,std::uint64_t budget,ExecutionProfile* profile=nullptr,bool continuation=false) {
        if (continuation && active_ && relocated_base_ && (*catalog_)[*active_].program.contains(entry)) {
            if (state.graphics_pending) return {RunStatus::graphics_pending,0};
            if (!budget) return {RunStatus::budget_exhausted,0};
            state.graphics_completes=0;
            return (*catalog_)[*active_].program.run(state,budget,profile);
        }
        auto selected=select(entry);
        std::optional<unsigned> base;
        auto compiled_entry=entry;
        if (!selected) if (const auto placement=find_placement(entry)) {
            selected=placement->index; base=placement->base;
            compiled_entry=(*catalog_)[placement->index].destination+entry-placement->base;
        }
        if (!selected) return {ready() ? RunStatus::unmapped_block : RunStatus::unbound_program,0};
        if (state.graphics_pending) return {RunStatus::graphics_pending,0};
        // Zero budget must not alter even the caller's continuation.
        if (!budget) return {RunStatus::budget_exhausted,0};
        active_=selected; relocated_base_=base;
        state.graphics_completes=0;
        state.next_pair=compiled_entry;
        return follow(state,(*catalog_)[*active_].program.run(state,budget,profile),budget,profile);
    }
    // Attaches loop substitutes to one catalog artifact. Call before copying the
    // binding to execution owners; existing copies keep their programs.
    void substitute(std::size_t artifact,std::shared_ptr<const LoopSubstitutes> substitutes) {
        if (artifact>=catalog_->size()) throw std::invalid_argument("Unknown VU artifact for loop substitutes");
        auto catalog=*catalog_;
        catalog[artifact].program.substitute(std::move(substitutes));
        catalog_=std::make_shared<const Catalog>(std::move(catalog));
    }
    std::size_t artifacts() const { return catalog_->size(); }
    RunResult resume(State& state,std::uint64_t budget,ExecutionProfile* profile=nullptr) {
        return active_ && (relocated_base_ || ready(*active_)) ?
            follow(state,(*catalog_)[*active_].program.run(state,budget,profile),budget,profile) : RunResult{RunStatus::unbound_program,0};
    }
private:
    // A program can jump to another one uploaded beside it, as with routines placed
    // at a fixed address and shared between programs. The target must be fully
    // loaded compiled code; otherwise the unmapped stop stands.
    RunResult follow(State& state,RunResult result,std::uint64_t budget,ExecutionProfile* profile) {
        while (result.status==RunStatus::unmapped_block && !relocated_base_ && result.blocks<budget) {
            const auto next=select(state.next_pair);
            if (!next || next==active_) break;
            active_=next;
            const auto blocks=result.blocks;
            result=(*catalog_)[*active_].program.run(state,budget-blocks,profile);
            result.blocks+=blocks;
        }
        return result;
    }
    struct Image {
        Program program;
        unsigned destination;
        std::vector<std::uint8_t> bytes;
        Image(Program value,unsigned begin,std::span<const std::uint8_t> image)
            : program(std::move(value)),destination(begin),bytes(image.begin(),image.end()) {
            if (image.empty() || image.size()%8 || std::uint64_t(begin)+image.size()/8>(program.unit() ? 2048U : 512U) ||
                !program.within(begin,std::uint64_t(begin)+image.size()/8))
                throw std::invalid_argument("Invalid compiled VU image extent");
        }
    };
    using Catalog=std::vector<Image>;
    bool ready(std::size_t index) const {
        return ready_[index];
    }
    // Programs may be placed anywhere in microcode memory. Compiled branches are
    // relative, so one runs unchanged at its compiled addresses; a BAL link and
    // the JR using it agree. An absolute address the EE computes for its load
    // location and passes through data is not translated, and such a jump would
    // leave the program as an unmapped stop or reach the wrong compiled pair.
    struct Placement { std::size_t index; unsigned base; };
    bool part_of(const Image& image,std::span<const std::uint8_t> bytes) const {
        for (std::size_t offset=0;offset+bytes.size()<=image.bytes.size();offset+=8)
            if (std::equal(bytes.begin(),bytes.end(),image.bytes.begin()+offset)) return true;
        return false;
    }
    bool placed(std::size_t index,unsigned base) const {
        const auto& image=(*catalog_)[index];
        const auto pairs=image.bytes.size()/8;
        if (std::size_t(base)+pairs>loaded_.size() ||
            !std::all_of(loaded_.begin()+base,loaded_.begin()+base+pairs,[](bool value) { return value; })) return false;
        return std::equal(image.bytes.begin(),image.bytes.end(),memory_.begin()+base*8U);
    }
    std::optional<Placement> find_placement(unsigned entry) {
        if (const auto known=placements_.find(entry);known!=placements_.end()) return known->second;
        std::optional<Placement> result;
        for (std::size_t i=0;i<catalog_->size() && !result;++i) {
            const auto& image=(*catalog_)[i];
            for (unsigned offset=0;offset<image.bytes.size()/8 && offset<=entry;++offset) {
                if (!image.program.contains(image.destination+offset) || !placed(i,entry-offset)) continue;
                result=Placement{i,entry-offset};
                break;
            }
        }
        if (result) placements_.emplace(entry,*result);
        return result;
    }
    std::optional<std::size_t> select(unsigned entry) const {
        std::optional<std::size_t> selected;
        for (std::size_t i=0;i<catalog_->size();++i) {
            const auto& image=(*catalog_)[i];
            if (!ready(i) || !image.program.contains(entry)) continue;
            if (selected) {
                const auto& previous=(*catalog_)[*selected];
                const auto end=std::uint64_t(image.destination)+image.bytes.size()/8;
                const auto previous_end=std::uint64_t(previous.destination)+previous.bytes.size()/8;
                // A fully covered superset supplies all the smaller artifact's
                // exact bytes plus additional compiled continuations.
                if (image.destination<=previous.destination && end>=previous_end) selected=i;
                else if (!(previous.destination<=image.destination && previous_end>=end)) return {};
            } else selected=i;
        }
        return selected;
    }
    std::shared_ptr<const Catalog> catalog_;
    std::vector<std::vector<bool>> present_;
    std::vector<bool> ready_;
    std::optional<std::size_t> active_;
    std::optional<unsigned> relocated_base_;
    std::vector<std::uint8_t> memory_;
    std::vector<bool> loaded_;
    std::map<unsigned,Placement> placements_;
};
}
