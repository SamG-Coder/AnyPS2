#pragma once
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
          present_{std::vector<bool>(image.size()/8,false)},ready_(1,false) {}
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
    }

    // Validate every chunk before changing coverage. Rejecting an upload batch
    // leaves the binding unchanged, just as the caller must leave its packet state.
    bool upload(unsigned unit,std::span<const UploadView> uploads) {
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
            if (std::none_of(catalog_->begin(),catalog_->end(),matches)) return false;
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
        if (active_ && !ready(*active_)) active_.reset();
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
        active_.reset();
    }
    RunResult launch(State& state,unsigned entry,std::uint64_t budget,ExecutionProfile* profile=nullptr) {
        const auto selected=select(entry);
        if (!selected) return {ready() ? RunStatus::unmapped_block : RunStatus::unbound_program,0};
        if (state.graphics_pending) return {RunStatus::graphics_pending,0};
        // Zero budget must not alter even the caller's continuation.
        if (!budget) return {RunStatus::budget_exhausted,0};
        active_=selected;
        state.graphics_completes=0;
        state.next_pair=entry;
        return (*catalog_)[*active_].program.run(state,budget,profile);
    }
    RunResult resume(State& state,std::uint64_t budget,ExecutionProfile* profile=nullptr) const {
        return active_ && ready(*active_) ? (*catalog_)[*active_].program.run(state,budget,profile) : RunResult{RunStatus::unbound_program,0};
    }
private:
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
};
}
