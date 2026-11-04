// MPEG VLC tables adapted from Play! at 1f27232772e484bfb79364b751b84f41f4c87481.
// Copyright (c) 2006-2026, Jean-Philip Desjardins
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//  * Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
// OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
// DAMAGE.
//
#pragma once
#include <cstdint>

namespace nt::ee::video_vlc {
struct Entry { std::uint16_t code; unsigned bits; std::uint32_t result; };
inline constexpr Entry address_increment[]{
    {0x0001, 1, 0x00010001},
    {0x0003, 3, 0x00030002},
    {0x0002, 3, 0x00030003},
    {0x0003, 4, 0x00040004},
    {0x0002, 4, 0x00040005},
    {0x0003, 5, 0x00050006},
    {0x0002, 5, 0x00050007},
    {0x0007, 7, 0x00070008},
    {0x0006, 7, 0x00070009},
    {0x000B, 8, 0x0008000A},
    {0x000A, 8, 0x0008000B},
    {0x0009, 8, 0x0008000C},
    {0x0008, 8, 0x0008000D},
    {0x0007, 8, 0x0008000E},
    {0x0006, 8, 0x0008000F},
    {0x0017, 10, 0x000A0010},
    {0x0016, 10, 0x000A0011},
    {0x0015, 10, 0x000A0012},
    {0x0014, 10, 0x000A0013},
    {0x0013, 10, 0x000A0014},
    {0x0012, 10, 0x000A0015},
    {0x0023, 11, 0x000B0016},
    {0x0022, 11, 0x000B0017},
    {0x0021, 11, 0x000B0018},
    {0x0020, 11, 0x000B0019},
    {0x001F, 11, 0x000B001A},
    {0x001E, 11, 0x000B001B},
    {0x001D, 11, 0x000B001C},
    {0x001C, 11, 0x000B001D},
    {0x001B, 11, 0x000B001E},
    {0x001A, 11, 0x000B001F},
    {0x0019, 11, 0x000B0020},
    {0x0018, 11, 0x000B0021},
    {0x000F, 11, 0x000B0022},
    {0x0008, 11, 0x000B0023},
};
inline constexpr Entry intra_type[]{
    {0x0001, 1, 0x00010001},
    {0x0001, 2, 0x00020011},
};
inline constexpr Entry predictive_type[]{
    {0x0001, 1, 0x0001000A},
    {0x0001, 2, 0x00020002},
    {0x0001, 3, 0x00030008},
    {0x0003, 5, 0x00050001},
    {0x0002, 5, 0x0005001A},
    {0x0001, 5, 0x00050012},
    {0x0001, 6, 0x00060011},
};
inline constexpr Entry bidirectional_type[]{
    {0x0002, 2, 0x0002000C},
    {0x0003, 2, 0x0002000E},
    {0x0002, 3, 0x00030004},
    {0x0003, 3, 0x00030006},
    {0x0002, 4, 0x00040008},
    {0x0003, 4, 0x0004000A},
    {0x0003, 5, 0x00050001},
    {0x0002, 5, 0x0005001E},
    {0x0003, 6, 0x0006001A},
    {0x0002, 6, 0x00060016},
    {0x0001, 6, 0x00060011},
};
inline constexpr Entry motion_code[]{
    {0x0001, 1, 0x00010000},
    {0x0002, 3, 0x00030001},
    {0x0003, 3, 0x0003FFFF},
    {0x0002, 4, 0x00040002},
    {0x0003, 4, 0x0004FFFE},
    {0x0002, 5, 0x00050003},
    {0x0003, 5, 0x0005FFFD},
    {0x0006, 7, 0x00070004},
    {0x0007, 7, 0x0007FFFC},
    {0x000A, 8, 0x00080005},
    {0x000B, 8, 0x0008FFFB},
    {0x0008, 8, 0x00080006},
    {0x0009, 8, 0x0008FFFA},
    {0x0006, 8, 0x00080007},
    {0x0007, 8, 0x0008FFF9},
    {0x0016, 10, 0x000A0008},
    {0x0017, 10, 0x000AFFF8},
    {0x0014, 10, 0x000A0009},
    {0x0015, 10, 0x000AFFF7},
    {0x0012, 10, 0x000A000A},
    {0x0013, 10, 0x000AFFF6},
    {0x0022, 11, 0x000B000B},
    {0x0023, 11, 0x000BFFF5},
    {0x0020, 11, 0x000B000C},
    {0x0021, 11, 0x000BFFF4},
    {0x001E, 11, 0x000B000D},
    {0x001F, 11, 0x000BFFF3},
    {0x001C, 11, 0x000B000E},
    {0x001D, 11, 0x000BFFF2},
    {0x001A, 11, 0x000B000F},
    {0x001B, 11, 0x000BFFF1},
    {0x0018, 11, 0x000B0010},
    {0x0019, 11, 0x000BFFF0},
};
inline constexpr Entry differential_motion[]{
    {0x0000, 1, 0x00010000},
    {0x0002, 2, 0x00020001},
    {0x0003, 2, 0x0002FFFF},
};
}
