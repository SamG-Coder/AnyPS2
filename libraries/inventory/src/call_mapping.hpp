#pragma once
#include <nt/inventory.hpp>
#include <string_view>

namespace nt::inventory::detail {
struct NativeMatch {
    std::string_view category,current,candidate,license,target,contract,optimization,reference;
};
inline const NativeMatch& mapping_for(const LibraryCall& call,std::string_view name) {
    static constexpr NativeMatch memory{
        "memory-and-strings","IOP native sysclib and LLVM memory intrinsics","C++ standard library / LLVM intrinsics","toolchain",
        "llvm.memcpy / llvm.memmove / llvm.memset; std::memchr and std::mismatch",
        "Preserve 32-bit sizes, aliases, overlap rules, unsigned-byte comparisons and faults before writes.",
        "Validate ranges once, use contiguous host operations, specialize constant sizes during AOT.",
        "https://llvm.org/docs/LangRef.html#standard-c-library-intrinsics"};
    static constexpr NativeMatch continuation{
        "nonlocal-control-flow","Native saved-register setjmp/longjmp continuations","LLVM-compiled continuations","project implementation",
        "Typed continuation with saved guest registers and compiled return targets",
        "Host jmp_buf is not compatible. Preserve saved registers, return value, stack lifetime and load-delay state.",
        "Discover return targets ahead of time; never decode a saved guest PC at runtime.",
        "https://llvm.org/docs/ExceptionHandling.html"};
    static constexpr NativeMatch formatting{
        "text-formatting","Existing native format parser and compiled print callbacks","Reuse native format helpers","project implementation",
        "format_native and typed guest argument capture",
        "Preserve 32-bit varargs layout, format dialect, output bounds and callback order; host printf varargs are not ABI compatible.",
        "Parse known formats ahead of time where proven; render into a reusable bounded buffer.",
        "https://github.com/ps2dev/ps2sdk/tree/2a18d15da1332665451a9bcf9a0d4df9029c9490/iop/system/sysclib"};
    static constexpr NativeMatch characters{
        "character-and-number-conversion","Native ASCII classification and 32-bit integer parsing","Reuse native fixed-width conversion helpers","project implementation",
        "Explicit ASCII classification and bounded integer parsing",
        "Preserve signed char conventions, supported character range, base prefixes, end pointers and 32-bit overflow. Host locale and long width can differ.",
        "Use bounded contiguous spans and constant-fold proven literal conversions during AOT.",
        "https://github.com/ps2dev/ps2sdk/tree/2a18d15da1332665451a9bcf9a0d4df9029c9490/iop/system/sysclib"};
    static constexpr NativeMatch threads{
        "scheduling-and-synchronization","NativeThreads, native IOP thread/semaphore/event services","Existing native scheduler plus C++20 synchronization","toolchain",
        "Compiled continuations, condition_variable and semaphore primitives",
        "Preserve priority, wake counts, cancellation, event modes, callback ordering and guest error values.",
        "Batch runnable continuations; block on real host events instead of guest polling. Do not map each guest thread to an OS thread blindly.",
        "https://learn.microsoft.com/en-us/cpp/standard-library/semaphore"};
    static constexpr NativeMatch allocation{
        "memory-allocation","NativeMemoryResources and native heap services","Existing native arenas / C++ memory resources","toolchain",
        "Bounded native arena allocation with stable converted addresses",
        "Preserve alignment, allocation direction, lifetime, free-space queries and 32-bit address identity.",
        "Reuse native arenas and combine allocation metadata; do not return arbitrary 64-bit host pointers to converted code.",
        "https://learn.microsoft.com/en-us/cpp/standard-library/memory-resource"};
    static constexpr NativeMatch files{
        "files-and-disc","Native file-I/O service and ISO9660 reader","Existing native file service / C++ filesystem","toolchain",
        "Disc range reads and native file handles",
        "Preserve path rules, sector sizes, descriptor ownership, seek results, EOF, async completion and read-only disc access.",
        "Coalesce contiguous reads and reuse buffers; preserve request completion order.",
        "https://learn.microsoft.com/en-us/windows/win32/fileio/file-buffering"};
    static constexpr NativeMatch saves{
        "save-storage","Partial file service; no general memory-card replacement","Existing file service with explicit save-container adapter","project implementation",
        "Native files with validated metadata and atomic updates",
        "Preserve directory metadata, capacity/errors and commit semantics; raw page/ECC interfaces are not normal file calls.",
        "Operate on files above the card transport boundary; coalesce writes only at defined commit points.",
        "https://learn.microsoft.com/en-us/windows/win32/fileio/file-management-functions"};
    static constexpr NativeMatch input{
        "controller-input","Existing native pad and SIO2 services","SDL3","Zlib",
        "SDL_GetGamepadButton / SDL_GetGamepadAxis / SDL_RumbleGamepad",
        "Adapt packet layout, slot/hotplug state, axis scale, pressure-button capability and async transfer completion. SDL does not implement SIO2 registers.",
        "Sample native input once per input update; publish packets to compiled consumers without serial-bus polling.",
        "https://github.com/libsdl-org/SDL/tree/bd9f6ca9ff0e9509f49980fc1b70837f54b6f244"};
    static constexpr NativeMatch usb{
        "physical-usb","Native Windows USB discovery and compiled driver callbacks","libusb","LGPL-2.1-or-later",
        "libusb_submit_transfer / libusb_cancel_transfer / descriptor APIs",
        "Preserve endpoint type, partial lengths, error codes, callback thread and cancellation lifetime. Host driver access is required. No fabricated device responses.",
        "Reuse transfer buffers and deliver completions to the existing native event queue.",
        "https://github.com/libusb/libusb/tree/987c1170d0018ba82557823a98ae443e8335e11b"};
    static constexpr NativeMatch audio{
        "audio","Existing XAudio2 ownership and native audio service","miniaudio (optional mixer/resampler)","MIT-0 OR Unlicense",
        "PCM device output, mixing and resampling",
        "SPU sample decoding, pitch, envelopes, loop points, reverb and transfer callbacks require explicit matching; an audio device does not supply them.",
        "Decode validated sample blocks once; batch PCM mixing. Reuse existing output ownership.",
        "https://github.com/mackron/miniaudio/tree/877ea4a7c2601c7b0f2a44e83dfac42e6470fea6"};
    static constexpr NativeMatch network{
        "networking","No general native socket ABI adapter","Standalone Asio","BSL-1.0",
        "asio::ip::tcp / udp, async operations and timers",
        "Verify each socket signature, sockaddr layout, byte order, partial I/O, errors, cancellation and ownership before binding. Unknown inet ordinals remain unknown.",
        "Use a shared completion loop and batched buffers; replace network polling with real completions.",
        "https://github.com/chriskohlhoff/asio/tree/bdf337ddb1a667c274372a602de5857ad3cb3ed0"};
    static constexpr NativeMatch device{
        "device-lifecycle","No complete native DEV9 replacement","No direct library match","not applicable",
        "Identify higher-level storage/network owner before selecting an adapter",
        "A hardware revision or register read is not a socket or file operation. Preserve initialization failure and callback ownership; do not fabricate readiness.",
        "Lower a proven device operation at its semantic boundary rather than reproducing a register bank.",
        "https://github.com/ps2dev/ps2sdk/tree/2a18d15da1332665451a9bcf9a0d4df9029c9490/iop/dev9"};
    static constexpr NativeMatch expansion{
        "device-lifecycle","Native DEV9 initialization reports unavailable adapter; exported I/O remains unsupported",
        "No direct library match","not applicable",
        "Optional expansion network/storage provider",
        "Module failure releases residency and publishes no exports. Host sockets alone do not implement raw-device transfers or callbacks.",
        "Replace higher-level network operations with a native socket owner before enabling an adapter.",
        "https://github.com/ps2dev/ps2sdk/tree/2a18d15da1332665451a9bcf9a0d4df9029c9490/iop/dev9"};
    static constexpr NativeMatch graphics{
        "graphics","Existing Vulkan renderer, compiled VU and packet conversion","Vulkan Memory Allocator (allocation only)","MIT",
        "Vulkan draw/transfer pipeline; vmaCreateBuffer / vmaCreateImage for allocation",
        "Preserve GS pixel formats, blending, ordering and scanout. VMA supplies allocation, not graphics-command semantics.",
        "Batch compatible draws without crossing synchronization or framebuffer dependencies; reuse GPU resources.",
        "https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/tree/0f1a757da579831283413295cc7f3b47eb521c77"};
    static constexpr NativeMatch services{
        "module-and-service-lifecycle","Existing compiled module loader, SIF and native service owners","Existing AnyPS2 native services","project implementation",
        "AOT module binding, typed native requests and compiled callbacks",
        "Preserve ABI versions, registration, reset generations, ownership, callback arguments and completion. A matching symbol is not a ready service.",
        "Bind known calls ahead of time and batch independent requests while preserving observable order.",
        "https://llvm.org/docs/LangRef.html"};
    static constexpr NativeMatch unknown{
        "unclassified","Unverified","No verified match","unknown",
        "Recover the public contract or retain a validated compiled module",
        "Do not infer semantics from a filename, game title, nearby call or an ordinal alone.",
        "No optimization before the call contract is known.",""};
    const auto lib=std::string_view(call.library);
    if (call.domain=="ee") {
        if (call.binding==CallBinding::unknown || name.empty()) return unknown;
        if (name.find("Thread")!=name.npos || name.find("Sema")!=name.npos || name.find("Alarm")!=name.npos || name.find("Timer")!=name.npos) return threads;
        if (name.find("Gs")!=name.npos || name.find("GS")!=name.npos) return graphics;
        return services;
    }
    if (lib=="sysclib") {
        if (call.selector==4 || call.selector==5) return continuation;
        if (call.selector==18 || call.selector==19 || call.selector==42) return formatting;
        if ((call.selector>=6 && call.selector<=9) || (call.selector>=36 && call.selector<=38)) return characters;
        return memory;
    }
    if (lib=="sysmem" || lib=="heaplib" || lib=="thfpool" || lib=="thvpool") return allocation;
    if (lib=="thbase" || lib=="thsemap" || lib=="thmsgbx" || lib=="thevent" || lib=="vblank" || lib=="timrman") return threads;
    if (lib=="ioman" || lib=="iomanx" || lib=="cdvdman" || lib=="cdvdfsv" || lib=="romdrv") return files;
    if (lib=="mcman" || lib=="mcserv") return saves;
    if (lib=="padman" || lib=="sio2man" || lib=="sio2d") return input;
    if (lib=="usbd") return usb;
    if (lib=="libsd") return audio;
    if (lib=="inet" || lib=="inetctl" || lib=="netcnf" || lib=="netdev" || lib=="ps2ip") return network;
    if (lib=="dev9") return expansion;
    if (lib=="atad" || lib=="dmacman") return device;
    if (lib=="modload" || lib=="loadcore" || lib=="sifcmd" || lib=="sifman" || lib=="intrman" || lib=="msifrpc") return services;
    return unknown;
}
}
