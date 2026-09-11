#include <SDL3/SDL.h>
#include <android/log.h>
#include <jni.h>

#define LOG_TAG "AsuraInput"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

extern SDL_JoystickID g_VirtualJoystickID;

extern "C" JNIEXPORT void JNICALL
Java_com_recomp_asurawrath_AsuraActivity_sendNativeButton(JNIEnv *env,
                                                          jclass clazz,
                                                          jint button,
                                                          jboolean pressed) {
  LOGI("Button event received -> button: %d, pressed: %d, deviceID: %d", button,
       pressed, (int)g_VirtualJoystickID);

  if (g_VirtualJoystickID == 0)
    return;

  SDL_Event event;
  SDL_zero(event);
  event.type =
      pressed ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
  event.gbutton.which = g_VirtualJoystickID;
  event.gbutton.button = static_cast<Uint8>(button);
  event.gbutton.down = pressed ? true : false;
  event.gbutton.timestamp = SDL_GetTicksNS();

  SDL_PushEvent(&event);
}

extern "C" JNIEXPORT void JNICALL
Java_com_recomp_asurawrath_AsuraActivity_sendNativeAxis(JNIEnv *env,
                                                        jclass clazz, jint axis,
                                                        jfloat value) {
  if (g_VirtualJoystickID == 0)
    return;

  if (value < -1.0f)
    value = -1.0f;
  if (value > 1.0f)
    value = 1.0f;

  Sint16 axis_value = static_cast<Sint16>(value * 32767.0f);

  LOGI("Axis event received -> axis: %d, value: %d, deviceID: %d", axis,
       axis_value, (int)g_VirtualJoystickID);

  SDL_Event event;
  SDL_zero(event);
  event.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
  event.gaxis.which = g_VirtualJoystickID;
  event.gaxis.axis = static_cast<Uint8>(axis);
  event.gaxis.value = axis_value;
  event.gaxis.timestamp = SDL_GetTicksNS();

  SDL_PushEvent(&event);
}
