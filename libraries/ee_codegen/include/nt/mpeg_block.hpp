#pragma once
#include <nt/mpeg_tables.hpp>
#include <nt/mpeg_idct.hpp>
#include <algorithm>
#include <array>
#include <span>

namespace nt::media {
// Incremental MPEG entropy decoding. Input is compressed media, never machine
// code. A symbol is committed only when all its bits are available.
class MpegBlockDecoder {
public:
    enum class Result { need_input, complete, malformed };
    struct Format {
        bool intra=false,field=false,reset_dc=false,mpeg1=false,alternate_scan=false,intra_vlc=false;
        unsigned precision=0,scale=0;
    };
    void begin(Format format,const std::array<std::uint8_t,64>& matrix) {
        format_=format; matrix_=matrix; samples_={}; coefficients_={}; block_=0; position_=0;
        if (format.reset_dc) predictors_.fill(128<<format.precision);
        pattern_=format.intra ? 63 : 0;
        stage_=format.intra ? Stage::block : Stage::pattern;
    }
    void reset() { predictors_={}; stage_=Stage::done; samples_={}; }
    unsigned pattern() const { return pattern_; }
    const std::array<std::int16_t,384>& samples() const { return samples_; }
    Result process(std::span<const std::uint8_t> bytes,unsigned& cursor) {
        const auto available=[&](unsigned bits) { return std::uint64_t(cursor)+bits<=bytes.size()*8; };
        const auto peek=[&](unsigned bits,unsigned offset=0) {
            unsigned value=0;
            for (unsigned i=0;i<bits;++i) {
                const auto bit=cursor+offset+i;
                value=(value<<1)|((bytes[bit/8]>>(7-bit%8))&1U);
            }
            return value;
        };
        for (;;) {
            if (stage_==Stage::done) return Result::complete;
            if (stage_==Stage::pattern) {
                if (!available(9)) return Result::need_input;
                const auto* code=find(std::span{mpeg_tables::block_pattern},peek(9),9);
                if (!code) return Result::malformed;
                cursor+=code->bits; pattern_=code->value; stage_=Stage::block;
            }
            if (stage_==Stage::block) {
                while (block_<6 && !(pattern_&(1U<<(5-block_)))) ++block_;
                if (block_==6) { stage_=Stage::done; continue; }
                coefficients_={}; position_=0; stage_=format_.intra ? Stage::dc : Stage::ac;
            }
            if (stage_==Stage::dc) {
                if (!available(32)) return Result::need_input;
                const auto table=block_<4 ? std::span<const mpeg_tables::Vlc>{mpeg_tables::dc_luma} : std::span<const mpeg_tables::Vlc>{mpeg_tables::dc_chroma};
                const auto* code=find(table,peek(10),10);
                if (!code) return Result::malformed;
                const auto size=code->value;
                auto delta=static_cast<int>(peek(size,code->bits));
                if (size && delta<(1<<(size-1))) delta-=((1<<size)-1);
                auto& predictor=predictors_[block_<4 ? 0 : block_-3];
                const auto next=predictor+delta;
                if (next<-32768 || next>32767) return Result::malformed;
                predictor=next; coefficients_[0]=static_cast<std::int16_t>(std::clamp(next*(8>>format_.precision),-2048,2047));
                cursor+=code->bits+size; position_=1; stage_=Stage::ac;
            }
            if (stage_==Stage::ac) {
                // The longest escape symbol, including its run and level, is
                // 28 bits in MPEG-1 and 24 bits in MPEG-2.
                if (!available(32)) return Result::need_input;
                const bool alternate=format_.intra && format_.intra_vlc;
                const unsigned end_bits=alternate ? 4 : 2,end_code=alternate ? 6 : 2;
                if (position_ && peek(end_bits)==end_code) {
                    cursor+=end_bits; finish_block(); ++block_; stage_=Stage::block; continue;
                }
                unsigned used=0,run=0;
                int level=0;
                bool escape=false;
                if (!position_ && !format_.intra && peek(1)==1) {
                    used=2; level=peek(1,1) ? -1 : 1;
                } else {
                    const auto table=alternate ? std::span{mpeg_tables::coefficients1} : std::span{mpeg_tables::coefficients0};
                    const auto* code=find(table,peek(16),16);
                    if (!code) return Result::malformed;
                    used=code->bits; escape=code->run<0;
                    if (escape) {
                        run=peek(6,used); used+=6;
                        if (format_.mpeg1) {
                            const auto raw=peek(8,used); used+=8;
                            if (!raw || raw==128) { level=static_cast<int>(peek(8,used))-(raw==128 ? 256 : 0); used+=8; }
                            else level=raw&128 ? static_cast<int>(raw)-256 : static_cast<int>(raw);
                        } else {
                            const auto raw=peek(12,used); used+=12;
                            level=raw&2048 ? static_cast<int>(raw)-4096 : static_cast<int>(raw);
                            if (level==-2048) return Result::malformed;
                        }
                        if (!level) return Result::malformed;
                    } else {
                        run=static_cast<unsigned>(code->run); level=peek(1,used) ? -code->level : code->level; ++used;
                    }
                }
                if (run>=64-position_) return Result::malformed;
                position_+=run;
                const int magnitude=level<0 ? -level : level;
                int value=(format_.intra ? magnitude*2 : magnitude*2+1)*static_cast<int>(matrix_[position_])*static_cast<int>(format_.scale)/32;
                if (format_.mpeg1 && !(value&1)) value=std::max(1,value-1);
                if (level<0) value=-value;
                // Signed MPEG-2 escape inverse quantization rounds down.
                if (escape && !format_.mpeg1 && level<0) {
                    const int numerator=(format_.intra ? magnitude*2 : magnitude*2+1)*static_cast<int>(matrix_[position_])*static_cast<int>(format_.scale);
                    value=-(numerator+31)/32;
                }
                const auto index=format_.alternate_scan ? mpeg_tables::scan1[position_] : mpeg_tables::scan0[position_];
                coefficients_[index]=static_cast<std::int16_t>(std::clamp(value,-2048,2047));
                ++position_; cursor+=used;
            }
        }
    }
private:
    template<class T,std::size_t N> static const T* find(std::span<const T,N> table,unsigned prefix,unsigned width) {
        for (const auto& entry:table) if ((prefix>>(width-entry.bits))==entry.code) return &entry;
        return nullptr;
    }
    void finish_block() {
        const auto pixels=mpeg_idct(coefficients_);
        for (unsigned y=0;y<8;++y) for (unsigned x=0;x<8;++x) {
            unsigned index;
            if (block_<4) {
                const auto row=format_.field ? y*2+block_/2 : y+(block_/2)*8;
                index=row*16+(block_%2)*8+x;
            } else index=256+(block_-4)*64+y*8+x;
            samples_[index]=format_.intra ? std::clamp<std::int16_t>(pixels[y*8+x],0,255) : pixels[y*8+x];
        }
    }
    enum class Stage { pattern,block,dc,ac,done };
    Stage stage_=Stage::done;
    Format format_;
    std::array<int,3> predictors_{};
    std::array<std::uint8_t,64> matrix_{};
    std::array<std::int16_t,64> coefficients_{};
    std::array<std::int16_t,384> samples_{};
    unsigned block_=0,position_=0,pattern_=0;
};
}
