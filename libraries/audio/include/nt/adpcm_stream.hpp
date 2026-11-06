#pragma once
#include <nt/adpcm.hpp>
#include <deque>
#include <iterator>

namespace nt::audio {
// Incremental compressed-media cursor. PCM ownership and elapsed playback are
// provided by the native audio engine; no instruction or device clock runs here.
class AdpcmStream {
public:
    struct Block {
        std::uint32_t address; std::uint64_t begin; bool end,loop_start;
        AdpcmHistory history;
        std::uint32_t repeat;
    };
    AdpcmStream(std::uint32_t start,std::uint32_t repeat) : cursor_(start),repeat_(repeat) {}
    SampleError decode(std::span<const std::uint8_t> source,unsigned blocks,std::vector<std::int16_t>& pcm) {
        if (source.empty() || source.size()%16 || cursor_%16 || repeat_%16 ||
            cursor_>=source.size() || repeat_>=source.size()) return SampleError::malformed;
        if (blocks>128 || pending_.size()+blocks>256) return SampleError::limit;
        for (unsigned i=0;i<blocks && !finished_;++i) {
            const auto frame=source.subspan(cursor_,16);
            const auto before=history_;
            const auto result=decode_adpcm_block(frame,history_,pcm);
            if (result!=SampleError::none) return result;
            const auto flags=frame[1];
            pending_.push_back({cursor_,frames_,(flags&1)!=0,(flags&4)!=0,before,repeat_}); frames_+=28;
            if (flags&4) repeat_=cursor_;
            cursor_+=16;
            if (cursor_==source.size()) cursor_=0;
            if (flags&1) {
                cursor_=repeat_;
                finished_=(flags&2)==0;
            }
        }
        return SampleError::none;
    }
    const auto& pending() const { return pending_; }
    // Retain the block being played, so a writer cannot alter queued media.
    void retire(std::uint64_t played) {
        while (!pending_.empty() && pending_.front().begin+28<=played) pending_.pop_front();
    }
    bool overlaps(std::uint32_t begin,std::uint32_t size) const {
        for (const auto& block:pending_)
            if (begin<std::uint64_t(block.address)+16 && std::uint64_t(begin)+size>block.address) return true;
        return false;
    }
    SampleError revise(std::span<const std::uint8_t> source,std::uint64_t played,
                       std::uint32_t address,std::uint32_t size,std::uint64_t& begin,
                       std::vector<std::int16_t>& pcm) {
        pcm.clear(); begin=frames_;
        const auto first=std::find_if(pending_.begin(),pending_.end(),[&](const auto& block) {
            // Keep the block already reached by the device, including at its
            // first sample. New source bytes apply to subsequent block reads.
            return block.begin>played && address<std::uint64_t(block.address)+16 &&
                std::uint64_t(address)+size>block.address;
        });
        if (first==pending_.end()) return SampleError::none;
        auto next=*this;
        const auto count=static_cast<unsigned>(pending_.end()-first);
        next.cursor_=first->address; next.history_=first->history; next.repeat_=first->repeat;
        next.frames_=first->begin; next.finished_=false;
        next.pending_.erase(next.pending_.begin()+(first-pending_.begin()),next.pending_.end());
        const auto result=next.decode(source,count,pcm);
        if (result!=SampleError::none) return result;
        // A new end marker inside the queued range ends the sound there. Queued
        // buffers keep their length, so the rest of the extent plays as silence;
        // the end is reported when the marker's block is consumed.
        if (next.frames_>frames_) return SampleError::unsupported;
        pcm.resize(static_cast<std::size_t>(frames_-first->begin),0);
        begin=first->begin; *this=std::move(next); return SampleError::none;
    }
    // The device reads its loop address when a block ends, so a new address
    // applies to every jump it has not taken, including jumps decoded ahead of
    // it. Queued blocks after the one being played are decoded again.
    SampleError retarget(std::span<const std::uint8_t> source,std::uint64_t played,std::uint32_t address,
                         std::uint64_t& begin,std::vector<std::int16_t>& pcm) {
        pcm.clear(); begin=frames_;
        if (address%16 || address>=source.size()) return SampleError::malformed;
        const auto first=std::find_if(pending_.begin(),pending_.end(),[&](const auto& block) { return block.begin>played; });
        // The block before the first replaced one is at or before the device,
        // and its end flag has not been followed until that block finishes.
        const bool jump_pending=first!=pending_.begin() && std::prev(first)->end;
        if (first==pending_.end()) {
            repeat_=address;
            if (jump_pending && !finished_) cursor_=address;
            return SampleError::none;
        }
        auto next=*this;
        const auto count=static_cast<unsigned>(pending_.end()-first);
        next.cursor_=jump_pending ? address : first->address;
        next.history_=first->history; next.repeat_=address;
        next.frames_=first->begin; next.finished_=false;
        next.pending_.erase(next.pending_.begin()+(first-pending_.begin()),next.pending_.end());
        const auto result=next.decode(source,count,pcm);
        if (result!=SampleError::none) return result;
        if (next.frames_!=frames_) return SampleError::unsupported;
        begin=first->begin; *this=std::move(next); return SampleError::none;
    }
    std::uint32_t source_position(std::uint64_t played) const {
        for (const auto& block:pending_) if (played>=block.begin && played<block.begin+28)
            return block.address+2+static_cast<std::uint32_t>((played-block.begin)/4)*2;
        return cursor_+2;
    }
    std::uint64_t frames() const { return frames_; }
    bool finished() const { return finished_; }
private:
    std::uint32_t cursor_,repeat_;
    AdpcmHistory history_;
    std::uint64_t frames_=0;
    bool finished_=false;
    std::deque<Block> pending_;
};
}
