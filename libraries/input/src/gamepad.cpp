#include <nt/gamepad.hpp>
#include <SDL3/SDL.h>
#include <chrono>
#include <stdexcept>

namespace nt::input {
struct Gamepads::Impl {
    std::array<SDL_Gamepad*,2> pads{};
    std::array<PadSnapshot,2> snapshots{};
    std::chrono::steady_clock::time_point next_scan{};
    struct Motors { std::uint16_t low=0,high=0; };
    std::array<Motors,2> motors{};
    std::chrono::steady_clock::time_point next_rumble{};
    ~Impl() {
        for (auto* pad:pads) if (pad) { SDL_RumbleGamepad(pad,0,0,0); SDL_CloseGamepad(pad); }
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    }
};
Gamepads::Gamepads() {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        throw std::runtime_error(std::string("Native gamepad initialization failed: ")+SDL_GetError());
    impl_=std::make_unique<Impl>();
    SDL_SetGamepadEventsEnabled(false);
    SDL_SetJoystickEventsEnabled(false);
    update();
}
Gamepads::~Gamepads()=default;
void Gamepads::update() {
    SDL_PumpEvents();
    SDL_UpdateGamepads();
    for (unsigned port=0;port<2;++port) if (auto*& pad=impl_->pads[port]; pad && !SDL_GamepadConnected(pad)) {
        SDL_CloseGamepad(pad); pad=nullptr; impl_->motors[port]={};
    }
    const auto now=std::chrono::steady_clock::now();
    if (now>=impl_->next_scan) {
        impl_->next_scan=now+std::chrono::seconds(1);
        int count=0;
        auto* ids=SDL_GetGamepads(&count);
        for (int i=0;ids && i<count;++i) {
            bool owned=false;
            for (auto* pad:impl_->pads) if (pad && SDL_GetGamepadID(pad)==ids[i]) owned=true;
            if (!owned) for (auto*& pad:impl_->pads) if (!pad) { pad=SDL_OpenGamepad(ids[i]); break; }
        }
        SDL_free(ids);
    }
    constexpr std::array<SDL_GamepadButton,16> buttons{
        SDL_GAMEPAD_BUTTON_BACK,SDL_GAMEPAD_BUTTON_LEFT_STICK,SDL_GAMEPAD_BUTTON_RIGHT_STICK,SDL_GAMEPAD_BUTTON_START,
        SDL_GAMEPAD_BUTTON_DPAD_UP,SDL_GAMEPAD_BUTTON_DPAD_RIGHT,SDL_GAMEPAD_BUTTON_DPAD_DOWN,SDL_GAMEPAD_BUTTON_DPAD_LEFT,
        SDL_GAMEPAD_BUTTON_INVALID,SDL_GAMEPAD_BUTTON_INVALID,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
        SDL_GAMEPAD_BUTTON_NORTH,SDL_GAMEPAD_BUTTON_EAST,SDL_GAMEPAD_BUTTON_SOUTH,SDL_GAMEPAD_BUTTON_WEST};
    constexpr std::array<SDL_GamepadAxis,4> axes{SDL_GAMEPAD_AXIS_RIGHTX,SDL_GAMEPAD_AXIS_RIGHTY,
        SDL_GAMEPAD_AXIS_LEFTX,SDL_GAMEPAD_AXIS_LEFTY};
    for (unsigned port=0;port<impl_->pads.size();++port) {
        PadSnapshot state;
        if (auto* pad=impl_->pads[port]) {
            state.connected=SDL_GamepadConnected(pad);
            if (state.connected) {
                state.rumble=SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad),SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN,false);
                for (unsigned i=0;i<buttons.size();++i)
                    if (buttons[i]!=SDL_GAMEPAD_BUTTON_INVALID && SDL_GetGamepadButton(pad,buttons[i])) state.buttons|=std::uint16_t(1U<<i);
                if (SDL_GetGamepadAxis(pad,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)>16384) state.buttons|=1U<<8;
                if (SDL_GetGamepadAxis(pad,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)>16384) state.buttons|=1U<<9;
                for (unsigned i=0;i<axes.size();++i) state.axes[i]=pad_axis(SDL_GetGamepadAxis(pad,axes[i]));
            }
        }
        impl_->snapshots[port]=state;
        if (!state.connected) impl_->motors[port]={};
    }
    // A bounded lease prevents a hung/terminated client from leaving a motor
    // running indefinitely. Refresh only commands previously accepted by SDL.
    if (now>=impl_->next_rumble) {
        impl_->next_rumble=now+std::chrono::milliseconds(100);
        for (unsigned port=0;port<2;++port) {
            const auto motors=impl_->motors[port];
            if (impl_->snapshots[port].connected && (motors.low || motors.high) &&
                !SDL_RumbleGamepad(impl_->pads[port],motors.low,motors.high,300)) {
                impl_->motors[port]={};
                throw std::runtime_error(std::string("Native controller rumble refresh failed: ")+SDL_GetError());
            }
        }
    }
}
PadSnapshot Gamepads::sample(unsigned port) const { return port<2 ? impl_->snapshots[port] : PadSnapshot{}; }
std::string Gamepads::name(unsigned port) const {
    if (port>=2 || !impl_->pads[port]) return {};
    const auto* name=SDL_GetGamepadName(impl_->pads[port]);
    return name ? name : "Unnamed controller";
}
bool Gamepads::rumble(unsigned port,std::uint16_t low,std::uint16_t high) {
    if (port>=2 || !impl_->snapshots[port].connected || !impl_->snapshots[port].rumble) return false;
    if (!SDL_RumbleGamepad(impl_->pads[port],low,high,(low || high) ? 300 : 0)) return false;
    impl_->motors[port]={low,high};
    return true;
}
}
