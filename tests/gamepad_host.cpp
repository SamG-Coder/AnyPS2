#include <nt/gamepad.hpp>
#include <SDL3/SDL.h>
#include <iostream>
#include <stdexcept>

int main() {
    try {
        const auto check=[](bool value) { if (!value) throw std::runtime_error(SDL_GetError()); };
        check(SDL_InitSubSystem(SDL_INIT_GAMEPAD));
        SDL_VirtualJoystickDesc descriptor{};
        SDL_INIT_INTERFACE(&descriptor);
        descriptor.type=SDL_JOYSTICK_TYPE_GAMEPAD;
        descriptor.naxes=SDL_GAMEPAD_AXIS_COUNT;
        descriptor.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
        descriptor.name="AnyPS2 synthetic controller";
        std::array<std::uint16_t,2> motors{};
        descriptor.userdata=&motors;
        descriptor.Rumble=[](void* data,Uint16 low,Uint16 high) -> bool {
            *static_cast<std::array<std::uint16_t,2>*>(data)={low,high}; return true;
        };
        const auto id=SDL_AttachVirtualJoystick(&descriptor);
        check(id!=0);
        auto* joystick=SDL_OpenJoystick(id);
        check(joystick!=nullptr);
        {
            nt::input::Gamepads pads;
            unsigned port=0;
            while (port<2 && pads.name(port)!=descriptor.name) ++port;
            check(port<2 && pads.sample(port).connected);
            check(pads.sample(port).rumble && pads.rumble(port,32000,16000));
            check(motors==std::array<std::uint16_t,2>{32000,16000});
            check(pads.rumble(port,0,0) && motors==std::array<std::uint16_t,2>{0,0});
            check(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_SOUTH,true));
            check(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_BACK,true));
            check(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_DPAD_UP,true));
            check(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFTX,-32768));
            check(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHTY,32767));
            check(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,32767));
            check(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,-32768));
            pads.update();
            const auto state=pads.sample(port);
            check(state.buttons==((1U<<14)|(1U<<4)|(1U<<8)|1U));
            check(state.axes[2]==0 && state.axes[1]==255);
            check(SDL_DetachVirtualJoystick(id));
            pads.update();
            check(!pads.sample(port).connected && pads.sample(port).buttons==0);
            check(!pads.rumble(port,65535,65535));
        }
        SDL_CloseJoystick(joystick);
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        std::cout<<"SDL button/trigger/stick mapping and disconnect passed\n";
    } catch (const std::exception& e) { std::cerr<<"Native mapping test failed: "<<e.what()<<'\n'; return 1; }
}
