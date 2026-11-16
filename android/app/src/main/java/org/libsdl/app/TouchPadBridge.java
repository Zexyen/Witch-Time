package org.libsdl.app;

/** [witchtime] SDLControllerManager's natives are package-private: this lets the app's touch controls
 *  (com.zexyen.witchtime.TouchControls) register a controller of their own with SDL, as SDL does for Android's. */
public final class TouchPadBridge {
    private TouchPadBridge() {}

    /** a standard gamepad: the 15 buttons SOUTH..DPAD_RIGHT, sticks on axes 0-3, triggers on axes 4 and 5 */
    public static void add(int id, String name, String desc) {
        SDLControllerManager.nativeAddJoystick(id, name, desc, 0, 0, 0x7fff, 6, 0x3f, 0, false, false, false, false);
    }

    public static void remove(int id) { SDLControllerManager.nativeRemoveJoystick(id); }

    /** keycode: an Android KeyEvent.KEYCODE_BUTTON_* or KEYCODE_DPAD_* */
    public static void button(int id, int keycode, boolean down) {
        if (down) SDLControllerManager.onNativePadDown(id, keycode, 0);
        else SDLControllerManager.onNativePadUp(id, keycode, 0);
    }

    /** value: -1..1 for the sticks (down and right positive), 0..1 for the triggers */
    public static void axis(int id, int axis, float value) { SDLControllerManager.onNativeJoy(id, axis, value); }
}
