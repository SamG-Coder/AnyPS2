#pragma once
#include <nt/iop_native_services.hpp>
#include <nt/iop_native_code.hpp>
#include <nt/disc_files.hpp>
#include <nt/iop_native_file_descriptors.hpp>
#include <algorithm>
#include <bit>

namespace nt::iop {
class NativeDiscServices final : public NativeServices {
public:
    explicit NativeDiscServices(const std::filesystem::path& image,NativeFileDescriptors* descriptors=nullptr)
        : files_(image),descriptors_(descriptors ? *descriptors : owned_descriptors_) {}
    NativeServiceReply invoke(const NativeServiceRequest& incoming,std::span<std::uint8_t> ram) override {
        if (incoming.kind!=NativeServiceKind::ioman) return {};
        auto request=incoming;
        const bool descriptor_call=(request.selector>=5 && request.selector<=9) ||
            request.selector==14 || request.selector==15 || request.selector==22 || request.selector==30 || request.selector==34;
        if (descriptor_call) {
            const auto entry=descriptors_.find(request.arguments[0]);
            if (entry && entry->owner!=this) return {};
            if (entry) request.arguments[0]=entry->local;
            else if (request.arguments[0]>=3) {
                NativeServiceReply reply{NativeServiceStatus::returned,static_cast<std::uint32_t>(-9)};
                if (request.selector==30) reply.high_return=0xffffffffU;
                return reply;
            }
        }
        const auto& args=request.arguments;
        const auto returned=[](std::int32_t value) { return NativeServiceReply{NativeServiceStatus::returned,static_cast<std::uint32_t>(value)}; };
        if (request.selector==22) {
            if (args[0]<3) return {};
            return files_.contains(args[0]) ? returned(0x10000010) : returned(-9);
        }
        // Disc descriptors are opened read-only. Validate access before touching
        // the caller's source buffer, including zero-length writes. Standard
        // streams belong to an explicit console provider, not this disc mount.
        if (request.selector==7) return args[0]<3 ? NativeServiceReply{} : returned(-9);
        if (request.selector==9 || request.selector==34) {
            // Console streams are owned by their provider. This mount opens no
            // streaming handles; ioctl2 cannot pause/resume ordinary ISO files.
            if (args[0]<3) return {};
            if (!files_.contains(args[0])) return returned(-9);
            if (files_.directory(args[0])) return {};
            if (request.selector==34) return returned(-5);
            switch (args[1]) {
            case 0x6310: case 0x6311: case 0x6312:
            case 0x10000: case 0x10008: case 0x10020: case 0x10030:
                // Known hardware controls need explicit native semantics.
                return {};
            default: return returned(-5);
            }
        }
        const bool mutation=(request.selector>=10 && request.selector<=12) || request.selector==17 || request.selector==18;
        if (request.selector==4 || request.selector==13 || request.selector==16 || mutation) {
            const auto source=ram_offset(args[0],1,ram.size());
            if (!source) return {NativeServiceStatus::address_fault,0,args[0]};
            const auto available=std::min<std::size_t>({256,ram.size()-*source,0x200000-*source});
            const auto bytes=ram.subspan(*source,available);
            const auto end=std::find(bytes.begin(),bytes.end(),0);
            if (end==bytes.end()) return available==256 ? returned(-36) : NativeServiceReply{NativeServiceStatus::address_fault,0,args[0]};
            const std::string path(bytes.begin(),end);
            if (mutation) return returned(disc::Files::mutation_error(path));
            if (request.selector==4) return returned(publish(files_.open(path,args[1]),false));
            if (request.selector==13) return returned(publish(files_.open_directory(path),true));
            const auto output=ram_offset(args[1],4,ram.size());
            if (!output || ram.size()-*output<40 || 0x200000-*output<40)
                return {NativeServiceStatus::address_fault,0,args[1]};
            for (auto at=*output;at<*output+40;at+=4)
                if (nt_iop_native_code_word(at)) return {NativeServiceStatus::code_write,0,args[1]};
            const auto stat=files_.stat(path);
            if (stat.result) return returned(stat.result);
            std::array<std::uint8_t,40> record{};
            encode_stat(record,stat.entry);
            std::copy(record.begin(),record.end(),ram.begin()+*output);
            return returned(0);
        }
        if (request.selector==30) {
            if (request.stack_pointer>0xffffffffU-16) return {NativeServiceStatus::address_fault,0,request.stack_pointer};
            const auto stack=ram_offset(request.stack_pointer+16,4,ram.size());
            if (!stack || ram.size()-*stack<4 || 0x200000-*stack<4)
                return {NativeServiceStatus::address_fault,0,request.stack_pointer};
            // O32 aligns a 64-bit argument to an even register pair: a2/a3.
            const auto offset=std::bit_cast<std::int64_t>(std::uint64_t(args[2])|(std::uint64_t(args[3])<<32));
            const auto value=static_cast<std::uint64_t>(files_.seek64(args[0],offset,load_native_word(ram,*stack)));
            NativeServiceReply reply{NativeServiceStatus::returned,static_cast<std::uint32_t>(value)};
            reply.high_return=static_cast<std::uint32_t>(value>>32); return reply;
        }
        if (request.selector==14) return returned(close(incoming.arguments[0],args[0],true));
        if (request.selector==15) {
            if (!files_.directory(args[0])) return returned(-9);
            const auto output=ram_offset(args[1],4,ram.size());
            if (!output || ram.size()-*output<300 || 0x200000-*output<300)
                return {NativeServiceStatus::address_fault,0,args[1]};
            for (auto at=*output;at<*output+300;at+=4)
                if (nt_iop_native_code_word(at)) return {NativeServiceStatus::code_write,0,args[1]};
            const auto next=files_.read_directory(args[0]);
            if (next.result<=0) return returned(next.result);
            std::array<std::uint8_t,300> record{};
            encode_stat(record,next.entry);
            std::copy(next.entry.name.begin(),next.entry.name.end(),record.begin()+40);
            std::copy(record.begin(),record.end(),ram.begin()+*output);
            return returned(1);
        }
        if (request.selector==5) return returned(close(incoming.arguments[0],args[0],false));
        if (request.selector==8) return returned(files_.seek(args[0],std::bit_cast<std::int32_t>(args[1]),args[2]));
        if (request.selector!=6) return {};
        if (!files_.contains(args[0])) return returned(-9);
        if (files_.directory(args[0])) return returned(-21);
        const auto size=args[2];
        if (size>0x100000) return returned(-22);
        if (!size) return returned(0);
        const auto destination=ram_offset(args[1],1,ram.size());
        if (!destination || size>ram.size()-*destination || size>0x200000-*destination)
            return {NativeServiceStatus::address_fault,0,args[1]};
        // Validate the entire requested write before reading or advancing the file.
        for (auto word=*destination&~3U;word<*destination+size;word+=4)
            if (nt_iop_native_code_word(word)) return {NativeServiceStatus::code_write,0,args[1]};
        auto result=files_.read(args[0],size);
        if (result.result>0) std::copy(result.bytes.begin(),result.bytes.end(),ram.begin()+*destination);
        return returned(result.result);
    }
private:
    static void encode_stat(std::span<std::uint8_t> record,const disc::Entry& entry) {
        store_native_word(record,0,entry.directory ? 0x25 : 0x14);
        store_native_word(record,8,entry.size);
        const auto& date=entry.recorded;
        const auto year=1900U+date[0];
        const std::array<std::uint8_t,8> timestamp{0,date[5],date[4],date[3],date[2],date[1],
            static_cast<std::uint8_t>(year),static_cast<std::uint8_t>(year>>8)};
        for (unsigned offset : {12U,20U,28U}) std::copy(timestamp.begin(),timestamp.end(),record.begin()+offset);
    }
    std::int32_t publish(std::int32_t local,bool directory) {
        if (local<0) return local;
        const auto fd=descriptors_.allocate(this,static_cast<std::uint32_t>(local));
        if (fd) return static_cast<std::int32_t>(*fd);
        files_.close(static_cast<std::uint32_t>(local),directory); return -24;
    }
    std::int32_t close(std::uint32_t fd,std::uint32_t local,bool directory) {
        const auto result=files_.close(local,directory);
        if (!result) descriptors_.release(fd,this);
        return result;
    }
    disc::Files files_;
    NativeFileDescriptors owned_descriptors_;
    NativeFileDescriptors& descriptors_;
};
}
