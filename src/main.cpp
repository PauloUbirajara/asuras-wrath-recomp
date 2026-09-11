// asurawrath - ReXGlue Recompiled Project

#include "generated/default/asura_wrath_init.h"

#include "asurawrath_app.h"

#if defined(__ANDROID__)
SDL_JoystickID g_VirtualJoystickID = 0;
SDL_Gamepad *g_VirtualGamepad = nullptr;
#endif

REX_DEFINE_APP(asura_wrath_recomp, AsurawrathApp::Create)
