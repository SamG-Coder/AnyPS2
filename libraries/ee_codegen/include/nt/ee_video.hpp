#pragma once
#include <nt/ee_native.hpp>
#include <nt/ee_video_vlc.hpp>
#include <nt/mpeg_block.hpp>
#include <nt/mpeg_color.hpp>
#include <array>
#include <utility>
#include <vector>
#include <span>
#include <algorithm>
#include <cstring>

namespace nt::ee {
// Native media stream initialization, header parsing and block decoding.
// Addresses are the converted caller's command ABI, not a device clock model.
class NativeVideo {
    enum class Command { none, peek, matrix, palette, vlc, picture, color };
public:
    static bool maps(std::uint32_t address) {
        return address>=0x10002000 && address<0x10002040;
    }
    IoRead read(std::uint32_t address) const {
        switch (address) {
        case 0x10002000: return {Stop::budget,result_};
        case 0x10002030: return {Stop::budget,peek_};
        case 0x10002004: case 0x10002034:
            return {Stop::budget,(pending_==Command::peek || pending_==Command::vlc) ? 0x80000000U : 0U};
        case 0x10002010: return {Stop::budget,format_|(coded_pattern_<<8)|(start_code_ ? 0x8000U : 0U)|(std::uint32_t(std::min<std::size_t>(8,output().size()/16))<<4)|(decode_error_ ? 0x4000U : 0U)|(pending_!=Command::none ? 0x80000000U : 0U)|std::uint32_t(input_.size()/16)};
        case 0x10002020: return {Stop::budget,bit_offset_|(std::uint32_t(input_.size()/16)<<8)};
        default: return {Stop::unsupported_system};
        }
    }
    Stop write(std::uint32_t address,std::uint32_t value) {
        if (address==0x10002010) {
            // Precision 3 is reserved. Do not silently choose another format.
            if (((value>>16)&3)==3) return Stop::unsupported_system;
            format_=value&0x07f30000U;
            if (value&(1U<<30)) {
                clear_input(0);
                thresholds_={}; peek_=0; result_=0; decode_error_=false; start_code_=false; coded_pattern_=0;
                decoder_.reset(); output_offset_=0; output_ready_=false; picture_decoded_=false;
                completed_=true;
                ++resets_;
            }
            return Stop::budget;
        }
        if (address!=0x10002000) return Stop::unsupported_system;
        if (pending_!=Command::none && (value>>28)!=0) return Stop::unsupported_system;
        switch (value>>28) {
        case 0: // Clear input and select the next stream's initial bit offset.
            decode_error_=false; start_code_=false;
            clear_input(value&127U);
            break;
        case 2: { // Decode one macroblock into native planar 16-bit samples.
            if (output_ready_ || ((format_&0x00a00000U)==0x00a00000U)) return Stop::unsupported_system;
            media::MpegBlockDecoder::Format format;
            format.intra=(value&(1U<<27))!=0; format.field=(value&(1U<<25))!=0;
            format.reset_dc=(value&(1U<<26))!=0; format.precision=(format_>>16)&3;
            format.mpeg1=(format_&(1U<<23))!=0; format.alternate_scan=(format_&(1U<<20))!=0;
            format.intra_vlc=(format_&(1U<<21))!=0;
            const auto scale=(value>>16)&31U;
            format.scale=(format_&(1U<<22)) ? media::mpeg_tables::nonlinear_scale[scale] : scale*2;
            decoder_.begin(format,matrices_[format.intra ? 0 : 1]);
            bit_offset_+=value&63U; pending_=Command::picture; picture_decoded_=false;
            start_code_=false; decode_error_=false; coded_pattern_=0;
            process(); return Stop::budget;
        }
        case 3: { // Decode one MPEG header symbol, then expose the next 32 bits.
            const auto table=(value>>26)&3U,picture=(format_>>24)&7U;
            if (table==1 && (picture<1 || picture>3)) return Stop::unsupported_system;
            switch (table) {
            case 0: vlc_=video_vlc::address_increment; break;
            case 1:
                if (picture==1) vlc_=video_vlc::intra_type;
                else if (picture==2) vlc_=video_vlc::predictive_type;
                else vlc_=video_vlc::bidirectional_type;
                break;
            case 2: vlc_=video_vlc::motion_code; break;
            case 3: vlc_=video_vlc::differential_motion; break;
            }
            bit_offset_+=value&63U; pending_=Command::vlc; decode_error_=false; start_code_=false;
            process();
            return Stop::budget;
        }
        case 4: // Advance, then peek a big-endian 32-bit stream value.
            decode_error_=false; start_code_=false;
            bit_offset_+=value&63U; pending_=Command::peek;
            process();
            return Stop::budget;
        case 5: // Load a complete intra/non-intra quantization matrix.
            decode_error_=false; start_code_=false;
            bit_offset_+=value&63U;
            pending_=Command::matrix; matrix_index_=(value>>27)&1;
            process();
            return Stop::budget;
        case 6: // Sixteen packed 16-bit vector-quantization colors.
            decode_error_=false; start_code_=false;
            pending_=Command::palette;
            process();
            return Stop::budget;
        case 7:
            if (output_ready_ || (value&(1U<<27))) return Stop::unsupported_system;
            color_remaining_=value&2047U; color_offset_=0;
            pending_=Command::color; start_code_=false; decode_error_=false;
            process(); return Stop::budget;
        case 9:
            decode_error_=false; start_code_=false;
            thresholds_={value&511U,(value>>16)&511U};
            break;
        default: return Stop::unsupported_system;
        }
        completed_=true;
        return Stop::budget;
    }
    Stop submit(std::span<const std::uint8_t> bytes) {
        if (submission_pending() || bytes.size()%16 || bytes.size()>0x100000) return Stop::unsupported_system;
        submitted_.assign(bytes.begin(),bytes.end()); submitted_offset_=0; accepted_bytes_=0;
        process();
        return Stop::budget;
    }
    std::span<const std::uint8_t> output() const {
        if (!output_ready_) return {};
        return std::span<const std::uint8_t>(output_storage_).first(output_size_).subspan(output_offset_);
    }
    void consume_output(std::size_t bytes) {
        output_offset_+=bytes;
        if (output_offset_==output_size_) { output_ready_=false; output_offset_=0; }
        process();
    }
    std::uint64_t converted_blocks() const { return converted_blocks_; }
    std::uint64_t decoded_blocks() const { return decoded_blocks_; }
    std::size_t accepted_bytes() const { return accepted_bytes_; }
    void cancel_input() { submitted_.clear(); submitted_offset_=0; }
    bool submission_pending() const { return submitted_offset_<submitted_.size(); }
    Stop append(std::span<const std::uint8_t> bytes) {
        if (submission_pending() || bytes.size()!=16 || input_.size()>128-bytes.size()) return Stop::unsupported_system;
        input_.insert(input_.end(),bytes.begin(),bytes.end());
        process();
        return Stop::budget;
    }
    const std::array<std::uint8_t,64>& matrix(unsigned index) const { return matrices_.at(index); }
    const std::array<std::uint8_t,32>& palette() const { return palette_; }
    bool take_completion() { return std::exchange(completed_,false); }
    std::array<std::uint32_t,2> thresholds() const { return thresholds_; }
    std::uint64_t resets() const { return resets_; }
private:
    void clear_input(std::uint32_t offset) {
        input_.clear(); bit_offset_=offset; pending_=Command::none;
        // Reset/clear affects accepted input, not the owner's unsubmitted tail.
        // The caller decides whether its outstanding submission is cancelled.
    }
    void compact_input() {
        const auto count=std::min<std::size_t>((bit_offset_/128)*16,input_.size());
        input_.erase(input_.begin(),input_.begin()+count);
        bit_offset_-=static_cast<unsigned>(count*8);
    }
    void feed() {
        const auto count=std::min<std::size_t>(128-input_.size(),submitted_.size()-submitted_offset_);
        input_.insert(input_.end(),submitted_.begin()+submitted_offset_,submitted_.begin()+submitted_offset_+count);
        submitted_offset_+=count; accepted_bytes_+=count;
        if (submitted_offset_==submitted_.size()) { submitted_.clear(); submitted_offset_=0; }
    }
    std::uint8_t byte_at(unsigned bit) const {
        const auto byte=bit/8,shift=bit%8;
        auto value=static_cast<std::uint8_t>(input_[byte]<<shift);
        if (shift) value|=input_[byte+1]>>(8-shift);
        return value;
    }
    std::uint32_t word_at(unsigned bit) const {
        std::uint32_t value=0;
        for (unsigned i=0;i<4;++i) value=(value<<8)|byte_at(bit+i*8);
        return value;
    }
    void process() {
        compact_input(); feed();
        if (pending_==Command::none) return;
        if (pending_==Command::color) {
            if (output_ready_) return;
            if (!color_remaining_) { pending_=Command::none; completed_=true; return; }
            while (color_offset_<color_input_.size()) {
                if (input_.size()*8<bit_offset_+8) return;
                color_input_[color_offset_++]=byte_at(bit_offset_); bit_offset_+=8;
                compact_input(); feed();
            }
            output_storage_=media::mpeg_rgba(color_input_,thresholds_);
            output_size_=1024; output_offset_=0; output_ready_=true;
            color_offset_=0; --color_remaining_; ++converted_blocks_; return;
        }
        if (pending_==Command::picture) {
            if (!picture_decoded_) {
                for (;;) {
                    const auto previous=bit_offset_;
                    const auto status=decoder_.process(input_,bit_offset_);
                    const bool progressed=previous!=bit_offset_;
                    compact_input(); feed();
                    if (status==media::MpegBlockDecoder::Result::malformed) {
                        decode_error_=true; pending_=Command::none; completed_=true; return;
                    }
                    if (status==media::MpegBlockDecoder::Result::complete) {
                        coded_pattern_=decoder_.pattern(); picture_decoded_=true;
                        output_ready_=true; output_offset_=0; output_size_=sizeof(decoder_.samples());
                        std::memcpy(output_storage_.data(),decoder_.samples().data(),output_size_);
                        ++decoded_blocks_; break;
                    }
                    // A full native input window with consumed bytes can refill;
                    // otherwise wait for another owned submission/programmed write.
                    if (!progressed || input_.size()*8<bit_offset_+32) return;
                }
            }
            if (output_ready_) return;
            if (input_.size()*8<bit_offset_+32) return;
            if ((word_at(bit_offset_)>>24)==0) {
                bit_offset_=(bit_offset_+7)&~7U;
                for (;;) {
                    compact_input(); feed();
                    if (input_.size()*8<bit_offset_+32) return;
                    const auto prefix=word_at(bit_offset_)>>8;
                    if (prefix) { start_code_=prefix==1; decode_error_=prefix!=1; break; }
                    bit_offset_+=8;
                }
            }
            peek_=word_at(bit_offset_); pending_=Command::none; completed_=true;
            return;
        }
        const auto required=pending_==Command::matrix ? 512U : pending_==Command::palette ? 256U : 32U;
        if (input_.size()*8<bit_offset_+required) return;
        if (pending_==Command::vlc) {
            const auto prefix=word_at(bit_offset_);
            const video_vlc::Entry* match=nullptr;
            for (const auto& entry:vlc_) if ((prefix>>(32-entry.bits))==entry.code) { match=&entry; break; }
            const auto consumed=match ? match->bits : 0U;
            // Keep this operation pending until its look-ahead is available.
            // Retrying must not advance the stream twice.
            if (input_.size()*8<bit_offset_+consumed+32) return;
            result_=match ? match->result : 0U;
            decode_error_=!match;
            bit_offset_+=consumed;
            peek_=word_at(bit_offset_);
        } else if (pending_==Command::peek) {
            result_=peek_=word_at(bit_offset_);
        } else {
            const auto destination=pending_==Command::matrix ? std::span<std::uint8_t>(matrices_[matrix_index_]) : std::span<std::uint8_t>(palette_);
            for (unsigned i=0;i<destination.size();++i) destination[i]=byte_at(bit_offset_+i*8);
            bit_offset_+=required;
        }
        compact_input(); feed();
        pending_=Command::none; completed_=true;
    }
    std::array<std::uint8_t,1024> output_storage_{};
    std::size_t output_size_=0;
    std::array<std::uint8_t,384> color_input_{};
    unsigned color_remaining_=0,color_offset_=0;
    std::uint64_t converted_blocks_=0;
    media::MpegBlockDecoder decoder_;
    bool picture_decoded_=false,output_ready_=false,start_code_=false;
    std::size_t output_offset_=0;
    std::uint32_t coded_pattern_=0;
    std::uint64_t decoded_blocks_=0;
    std::vector<std::uint8_t> submitted_;
    std::size_t submitted_offset_=0,accepted_bytes_=0;
    std::uint32_t peek_=0,result_=0;
    bool decode_error_=false;
    std::span<const video_vlc::Entry> vlc_;
    std::vector<std::uint8_t> input_;
    std::array<std::array<std::uint8_t,64>,2> matrices_{};
    std::array<std::uint8_t,32> palette_{};
    unsigned matrix_index_=0;
    Command pending_=Command::none;
    std::uint32_t format_=0,bit_offset_=0;
    std::array<std::uint32_t,2> thresholds_{};
    std::uint64_t resets_=0;
    bool completed_=false;
};
}
