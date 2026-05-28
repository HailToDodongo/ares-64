#include <SDL3/SDL.h>
#include <nall/hid.hpp>
#include "input.hpp"

// SDL3 scancode to ares key name mapping
static const struct { SDL_Scancode sc; const char* name; } sdlKeys[] = {
  {SDL_SCANCODE_ESCAPE,"Escape"},{SDL_SCANCODE_F1,"F1"},{SDL_SCANCODE_F2,"F2"},
  {SDL_SCANCODE_F3,"F3"},{SDL_SCANCODE_F4,"F4"},{SDL_SCANCODE_F5,"F5"},
  {SDL_SCANCODE_F6,"F6"},{SDL_SCANCODE_F7,"F7"},{SDL_SCANCODE_F8,"F8"},
  {SDL_SCANCODE_F9,"F9"},{SDL_SCANCODE_F10,"F10"},{SDL_SCANCODE_F11,"F11"},
  {SDL_SCANCODE_F12,"F12"},{SDL_SCANCODE_SCROLLLOCK,"ScrollLock"},{SDL_SCANCODE_PAUSE,"Pause"},
  {SDL_SCANCODE_GRAVE,"Tilde"},{SDL_SCANCODE_0,"Num0"},{SDL_SCANCODE_1,"Num1"},
  {SDL_SCANCODE_2,"Num2"},{SDL_SCANCODE_3,"Num3"},{SDL_SCANCODE_4,"Num4"},
  {SDL_SCANCODE_5,"Num5"},{SDL_SCANCODE_6,"Num6"},{SDL_SCANCODE_7,"Num7"},
  {SDL_SCANCODE_8,"Num8"},{SDL_SCANCODE_9,"Num9"},{SDL_SCANCODE_MINUS,"Dash"},
  {SDL_SCANCODE_EQUALS,"Equal"},{SDL_SCANCODE_BACKSPACE,"Backspace"},{SDL_SCANCODE_INSERT,"Insert"},
  {SDL_SCANCODE_DELETE,"Delete"},{SDL_SCANCODE_HOME,"Home"},{SDL_SCANCODE_END,"End"},
  {SDL_SCANCODE_PAGEUP,"PageUp"},{SDL_SCANCODE_PAGEDOWN,"PageDown"},
  {SDL_SCANCODE_A,"A"},{SDL_SCANCODE_B,"B"},{SDL_SCANCODE_C,"C"},{SDL_SCANCODE_D,"D"},
  {SDL_SCANCODE_E,"E"},{SDL_SCANCODE_F,"F"},{SDL_SCANCODE_G,"G"},{SDL_SCANCODE_H,"H"},
  {SDL_SCANCODE_I,"I"},{SDL_SCANCODE_J,"J"},{SDL_SCANCODE_K,"K"},{SDL_SCANCODE_L,"L"},
  {SDL_SCANCODE_M,"M"},{SDL_SCANCODE_N,"N"},{SDL_SCANCODE_O,"O"},{SDL_SCANCODE_P,"P"},
  {SDL_SCANCODE_Q,"Q"},{SDL_SCANCODE_R,"R"},{SDL_SCANCODE_S,"S"},{SDL_SCANCODE_T,"T"},
  {SDL_SCANCODE_U,"U"},{SDL_SCANCODE_V,"V"},{SDL_SCANCODE_W,"W"},{SDL_SCANCODE_X,"X"},
  {SDL_SCANCODE_Y,"Y"},{SDL_SCANCODE_Z,"Z"},{SDL_SCANCODE_LEFTBRACKET,"LeftBracket"},
  {SDL_SCANCODE_RIGHTBRACKET,"RightBracket"},{SDL_SCANCODE_BACKSLASH,"Backslash"},
  {SDL_SCANCODE_SEMICOLON,"Semicolon"},{SDL_SCANCODE_APOSTROPHE,"Apostrophe"},
  {SDL_SCANCODE_COMMA,"Comma"},{SDL_SCANCODE_PERIOD,"Period"},{SDL_SCANCODE_SLASH,"Slash"},
  {SDL_SCANCODE_KP_0,"Keypad0"},{SDL_SCANCODE_KP_1,"Keypad1"},{SDL_SCANCODE_KP_2,"Keypad2"},
  {SDL_SCANCODE_KP_3,"Keypad3"},{SDL_SCANCODE_KP_4,"Keypad4"},{SDL_SCANCODE_KP_5,"Keypad5"},
  {SDL_SCANCODE_KP_6,"Keypad6"},{SDL_SCANCODE_KP_7,"Keypad7"},{SDL_SCANCODE_KP_8,"Keypad8"},
  {SDL_SCANCODE_KP_9,"Keypad9"},{SDL_SCANCODE_KP_PLUS,"Add"},{SDL_SCANCODE_KP_MINUS,"Subtract"},
  {SDL_SCANCODE_KP_MULTIPLY,"Multiply"},{SDL_SCANCODE_KP_DIVIDE,"Divide"},
  {SDL_SCANCODE_KP_ENTER,"Enter"},{SDL_SCANCODE_UP,"Up"},{SDL_SCANCODE_DOWN,"Down"},
  {SDL_SCANCODE_LEFT,"Left"},{SDL_SCANCODE_RIGHT,"Right"},{SDL_SCANCODE_TAB,"Tab"},
  {SDL_SCANCODE_RETURN,"Return"},{SDL_SCANCODE_SPACE,"Spacebar"},
  {SDL_SCANCODE_LCTRL,"LeftControl"},{SDL_SCANCODE_RCTRL,"RightControl"},
  {SDL_SCANCODE_LALT,"LeftAlt"},{SDL_SCANCODE_RALT,"RightAlt"},
  {SDL_SCANCODE_LSHIFT,"LeftShift"},{SDL_SCANCODE_RSHIFT,"RightShift"},
  {SDL_SCANCODE_LGUI,"LeftSuper"},{SDL_SCANCODE_RGUI,"RightSuper"},{SDL_SCANCODE_MENU,"Menu"},
};


auto Input::create() -> bool {
  return initialize();
}

auto Input::setContext(uintptr context) -> bool {
  _window = reinterpret_cast<SDL_Window*>(context);
  return initialize();
}

auto Input::acquire() -> bool {
  if(!_mouseAcquired) { _mouseAcquired = true; SDL_SetWindowRelativeMouseMode((SDL_Window*)_window, true); }
  return true;
}

auto Input::release() -> bool {
  if(_mouseAcquired) { _mouseAcquired = false; SDL_SetWindowRelativeMouseMode((SDL_Window*)_window, false); }
  return true;
}

auto Input::poll() -> std::vector<std::shared_ptr<::nall::HID::Device>> {
  std::vector<std::shared_ptr<::nall::HID::Device>> devices;

  // Keyboard
  if(_keyboard) {
    int numKeys = 0;
    const bool* state = SDL_GetKeyboardState(&numKeys);
    auto& group = _keyboard->buttons();
    if(!_keyboardCaptured) {
      for(u32 i = 0; i < _keyMap.size(); i++) {
        bool v = _keyMap[i].sc < (u32)numKeys ? state[_keyMap[i].sc] : false;
        if(group.input(i).value() != v) {
          if(_onChange) _onChange(_keyboard, ::nall::HID::Keyboard::GroupID::Button, i, group.input(i).value(), v);
          group.input(i).setValue(v);
        }
      }
    }
    devices.push_back(_keyboard);
  }

  // Mouse
  if(_mouse) {
    f32 mx, my;
    if(_mouseAcquired) {
      SDL_GetRelativeMouseState(&mx, &my);
    } else {
      SDL_GetMouseState(&mx, &my);
      mx -= _lastMouseX; my -= _lastMouseY;
      _lastMouseX += mx; _lastMouseY += my;
    }
    u32 buttons = SDL_GetMouseState(nullptr, nullptr);

    auto setMouse = [&](u32 g, u32 i, s16 v) {
      auto& grp = _mouse->group(g);
      if(grp.input(i).value() != v) {
        if(_onChange) _onChange(_mouse, g, i, grp.input(i).value(), v);
        grp.input(i).setValue(v);
      }
    };
    setMouse(::nall::HID::Mouse::GroupID::Axis, 0, (s16)mx);
    setMouse(::nall::HID::Mouse::GroupID::Axis, 1, (s16)my);
    setMouse(::nall::HID::Mouse::GroupID::Button, 0, (bool)(buttons & SDL_BUTTON_LMASK));
    setMouse(::nall::HID::Mouse::GroupID::Button, 1, (bool)(buttons & SDL_BUTTON_MMASK));
    setMouse(::nall::HID::Mouse::GroupID::Button, 2, (bool)(buttons & SDL_BUTTON_RMASK));
    setMouse(::nall::HID::Mouse::GroupID::Button, 3, (bool)(buttons & SDL_BUTTON_X1MASK));
    setMouse(::nall::HID::Mouse::GroupID::Button, 4, (bool)(buttons & SDL_BUTTON_X2MASK));
    devices.push_back(_mouse);
  }

  // Joypads
  for(auto& jp : _joypads) {
    auto setJP = [&](u32 g, u32 i, s16 v) {
      auto& grp = jp.hid->group(g);
      if(grp.input(i).value() != v) {
        if(_onChange) _onChange(jp.hid, g, i, grp.input(i).value(), v);
        grp.input(i).setValue(v);
      }
    };
    for(u32 n : range(jp.hid->axes().size())) setJP(::nall::HID::Joypad::GroupID::Axis, n, (s16)SDL_GetJoystickAxis((SDL_Joystick*)jp.handle, n));
    for(s32 n = 0; n < (s32)jp.hid->hats().size() - 1; n += 2) {
      u8 state = SDL_GetJoystickHat((SDL_Joystick*)jp.handle, n >> 1);
      setJP(::nall::HID::Joypad::GroupID::Hat, n+0, state&SDL_HAT_LEFT ? -32767 : state&SDL_HAT_RIGHT ? +32767 : 0);
      setJP(::nall::HID::Joypad::GroupID::Hat, n+1, state&SDL_HAT_UP   ? -32767 : state&SDL_HAT_DOWN  ? +32767 : 0);
    }
    for(u32 n : range(jp.hid->buttons().size())) setJP(::nall::HID::Joypad::GroupID::Button, n, (bool)SDL_GetJoystickButton((SDL_Joystick*)jp.handle, n));
    devices.push_back(jp.hid);
  }

  return devices;
}

auto Input::rumble(u64 id, u16 strong, u16 weak) -> bool {
  for(auto& jp : _joypads) { auto* h = (SDL_Joystick*)jp.handle; if(jp.hid->id() == id) { SDL_RumbleJoystick(h, strong, weak, 0); return true; } }
  return false;
}

auto Input::initialize() -> bool {
  terminate();
  SDL_InitSubSystem(SDL_INIT_JOYSTICK);

  _keyboard = std::make_shared<::nall::HID::Keyboard>();
  _keyboard->setVendorID(::nall::HID::Keyboard::GenericVendorID);
  _keyboard->setProductID(::nall::HID::Keyboard::GenericProductID);
  _keyboard->setPathID(0);
  _keyMap.clear();
  for(u32 i = 0; i < sizeof(sdlKeys)/sizeof(sdlKeys[0]); i++) {
    _keyboard->buttons().append(sdlKeys[i].name);
    _keyMap.push_back({sdlKeys[i].sc, i});
  }

  _mouse = std::make_shared<::nall::HID::Mouse>();
  _mouse->setVendorID(::nall::HID::Mouse::GenericVendorID);
  _mouse->setProductID(::nall::HID::Mouse::GenericProductID);
  _mouse->setPathID(0);
  _mouse->axes().append("X"); _mouse->axes().append("Y");
  _mouse->buttons().append("Left"); _mouse->buttons().append("Middle");
  _mouse->buttons().append("Right"); _mouse->buttons().append("Up"); _mouse->buttons().append("Down");
  _lastMouseX = _lastMouseY = 0;
  _mouseAcquired = false;

  enumerateJoypads();
  _ready = true;
  return true;
}

auto Input::terminate() -> void {
  _ready = false;
  _keyMap.clear(); _keyboard.reset(); _mouse.reset();
  for(auto& jp : _joypads) SDL_CloseJoystick((SDL_Joystick*)jp.handle);
  _joypads.clear();
}

auto Input::enumerateJoypads() -> void {
  for(auto& jp : _joypads) SDL_CloseJoystick((SDL_Joystick*)jp.handle);
  _joypads.clear();
  int count; SDL_JoystickID* joysticks = SDL_GetJoysticks(&count);
  if(!joysticks) return;
  for(int i = 0; i < count; i++) {
    Joypad jp;
    jp.handle = SDL_OpenJoystick(joysticks[i]);
    if(!jp.handle) continue;
    s32 axes = SDL_GetNumJoystickAxes((SDL_Joystick*)jp.handle);
    s32 hats = SDL_GetNumJoystickHats((SDL_Joystick*)jp.handle) * 2;
    s32 buttons = SDL_GetNumJoystickButtons((SDL_Joystick*)jp.handle);
    u16 vid = SDL_GetJoystickVendorForID(joysticks[i]);
    u16 pid = SDL_GetJoystickProductForID(joysticks[i]);
    if(!vid) vid = ::nall::HID::Joypad::GenericVendorID;
    if(!pid) pid = ::nall::HID::Joypad::GenericProductID;
    SDL_GUID guid = SDL_GetJoystickGUIDForID(joysticks[i]);
    char gs[64]{}; SDL_GUIDToString(guid, gs, sizeof(gs));
    string name = SDL_GetJoystickName((SDL_Joystick*)jp.handle);
    if(!name) name = "Joypad";
    jp.hid->setName({name," SDL_ID:",joysticks[i]});
    jp.hid->setVendorID(vid); jp.hid->setProductID(pid); jp.hid->setPathID(0);
    if(*gs && string{gs} != "00000000000000000000000000000000") jp.hid->setIdentifier(gs);
    for(u32 n : range(axes)) jp.hid->axes().append(n);
    for(u32 n : range(hats)) jp.hid->hats().append(n);
    for(u32 n : range(buttons)) jp.hid->buttons().append(n);
    jp.hid->setRumble(true);
    _joypads.push_back(jp);
  }
  SDL_free(joysticks);
}
