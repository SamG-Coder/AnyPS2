#pragma once
#include <nt/audio.hpp>
#include <xaudio2fx.h>
#include <xapo.h>
#include <wrl/client.h>

namespace nt::audio::windows {
// Intentional native room presets, not a reproduction of console DSP memory.
inline bool reverb_parameters(ReverbSpace space,XAUDIO2FX_REVERB_PARAMETERS& result) {
    XAUDIO2FX_REVERB_I3DL2_PARAMETERS room{};
    switch (space) {
    case ReverbSpace::off: case ReverbSpace::room: room=XAUDIO2FX_I3DL2_PRESET_ROOM; break;
    case ReverbSpace::studio_small: room=XAUDIO2FX_I3DL2_PRESET_PADDEDCELL; break;
    case ReverbSpace::studio_medium: room=XAUDIO2FX_I3DL2_PRESET_LIVINGROOM; break;
    case ReverbSpace::studio_large: room=XAUDIO2FX_I3DL2_PRESET_AUDITORIUM; break;
    case ReverbSpace::hall: room=XAUDIO2FX_I3DL2_PRESET_CONCERTHALL; break;
    case ReverbSpace::space: room=XAUDIO2FX_I3DL2_PRESET_ARENA; break;
    case ReverbSpace::pipe: room=XAUDIO2FX_I3DL2_PRESET_STONECORRIDOR; break;
    default: return false;
    }
    ReverbConvertI3DL2ToNative(&room,&result);
    return true;
}
}
