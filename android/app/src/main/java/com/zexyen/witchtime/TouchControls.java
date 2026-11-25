package com.zexyen.witchtime;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.hardware.input.InputManager;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;

import org.libsdl.app.SDLControllerManager;
import org.libsdl.app.TouchPadBridge;

/**
 * On-screen controls over the game (dusklight's touch controls were the reference: a floating left stick, a drag on the
 * right half for the camera, buttons where a Switch has them). They are an SDL gamepad of their own, so the runtime sees
 * a standard controller (nnpad_sdl.c; its positional layout makes the right button A, the bottom one B).
 * controllers.ini, [general] touch_controls = auto (shown while Android has no gamepad: a handheld's built-in controller,
 * or one paired later, hides them) | on | off.
 */
public class TouchControls extends View implements InputManager.InputDeviceListener {
    private static final int DEVICE = -0x5754;          // an id no Android input device has
    private static final float STICK_DP = 62f, KNOB_DP = 24f, BUTTON_DP = 28f, SMALL_DP = 22f;

    /** a button: its label, what it sends (a keycode, or -axis-1 for ZL / ZR), its centre in dp from a corner */
    private static final class Button {
        final String label; final int send; final boolean right, bottom; final float x, y, r;
        int pointer = -1;
        Button(String label, int send, boolean right, boolean bottom, float x, float y, float r) {
            this.label = label; this.send = send; this.right = right; this.bottom = bottom; this.x = x; this.y = y; this.r = r;
        }
    }
    private final Button[] buttons = {
        new Button("A", KeyEvent.KEYCODE_BUTTON_B, true, true, 58, 110, BUTTON_DP),      // right of the diamond = EAST
        new Button("B", KeyEvent.KEYCODE_BUTTON_A, true, true, 110, 58, BUTTON_DP),      // bottom = SOUTH
        new Button("X", KeyEvent.KEYCODE_BUTTON_Y, true, true, 110, 162, BUTTON_DP),     // top = NORTH
        new Button("Y", KeyEvent.KEYCODE_BUTTON_X, true, true, 162, 110, BUTTON_DP),     // left = WEST
        new Button("ZR", -6, true, false, 60, 60, BUTTON_DP),
        new Button("R", KeyEvent.KEYCODE_BUTTON_R1, true, false, 60, 130, BUTTON_DP),
        new Button("RS", KeyEvent.KEYCODE_BUTTON_THUMBR, true, true, 230, 50, SMALL_DP),
        new Button("ZL", -5, false, false, 60, 60, BUTTON_DP),
        new Button("L", KeyEvent.KEYCODE_BUTTON_L1, false, false, 60, 130, BUTTON_DP),
        new Button("◀", KeyEvent.KEYCODE_DPAD_LEFT, false, false, 150, 100, SMALL_DP),
        new Button("▲", KeyEvent.KEYCODE_DPAD_UP, false, false, 195, 55, SMALL_DP),
        new Button("▶", KeyEvent.KEYCODE_DPAD_RIGHT, false, false, 240, 100, SMALL_DP),
        new Button("▼", KeyEvent.KEYCODE_DPAD_DOWN, false, false, 195, 145, SMALL_DP),
        new Button("−", KeyEvent.KEYCODE_BUTTON_SELECT, false, false, -1, 30, SMALL_DP),   // x < 0: left of the middle
        new Button("+", KeyEvent.KEYCODE_BUTTON_START, true, false, -1, 30, SMALL_DP),          // right of the middle
    };
    /** a stick that appears where the finger lands: pointer, its start and current position in pixels */
    private static final class Stick { int pointer = -1; float x0, y0, x, y; }
    private final Stick move = new Stick(), look = new Stick();
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final float dp;
    private final File config;
    private boolean registered;

    public TouchControls(Context c, File config) {
        super(c);
        this.config = config;
        dp = c.getResources().getDisplayMetrics().density;
        line.setStyle(Paint.Style.STROKE); line.setStrokeWidth(2 * dp); line.setColor(0x99ffffff);
        text.setColor(0xddffffff); text.setTextAlign(Paint.Align.CENTER); text.setTextSize(15 * dp); text.setFakeBoldText(true);
        ((InputManager) c.getSystemService(Context.INPUT_SERVICE)).registerInputDeviceListener(this, null);
        update();
    }

    /** auto | on | off from controllers.ini [general] touch_controls (default auto; the file is the runtime's) */
    String mode() {
        String section = "", mode = "auto";
        try (BufferedReader r = new BufferedReader(new FileReader(config))) {
            for (String l; (l = r.readLine()) != null; ) {
                l = l.trim();
                if (l.startsWith("[")) section = l.toLowerCase();
                else if (section.equals("[general]") && l.toLowerCase().startsWith("touch_controls")) {
                    int eq = l.indexOf('='), sc = l.indexOf(';');
                    if (eq > 0) mode = l.substring(eq + 1, sc > eq ? sc : l.length()).trim().toLowerCase();
                }
            }
        } catch (IOException e) {
            // no file yet (the runtime writes it at its first start): auto
        }
        return mode;
    }

    /** SDL's own test for a game controller, so the controls hide for exactly the devices the game can use */
    static boolean gamepadPresent() {
        for (int id : InputDevice.getDeviceIds())
            if (SDLControllerManager.isDeviceSDLJoystick(id)) return true;
        return false;
    }

    /** shown or hidden by the setting and the devices; hidden, the touch controller leaves SDL before a real pad joins it
     *  (SDL looks for Android's devices every 3 s), so the real pad becomes player 1 */
    void update() {
        String m = mode();
        boolean show = m.equals("on") || (!m.equals("off") && !gamepadPresent());
        if (!show) release();
        setVisibility(show ? VISIBLE : GONE);
    }

    private void release() {
        move.pointer = look.pointer = -1;
        for (Button b : buttons) b.pointer = -1;
        if (registered) { TouchPadBridge.remove(DEVICE); registered = false; }
        invalidate();
    }

    private float bx(Button b) {
        if (b.x < 0) return getWidth() / 2f + (b.right ? 60 : -60) * dp;
        return b.right ? getWidth() - b.x * dp : b.x * dp;
    }
    private float by(Button b) { return b.bottom ? getHeight() - b.y * dp : b.y * dp; }
    private Button hit(float x, float y) {
        for (Button b : buttons) {
            float dx = x - bx(b), dy = y - by(b), r = (b.r + 8) * dp;         // a little larger than drawn: fingers are wide
            if (dx * dx + dy * dy <= r * r) return b;
        }
        return null;
    }

    private void send(Button b, boolean down) {
        if (b.send < 0) TouchPadBridge.axis(DEVICE, -b.send - 1, down ? 1f : 0f);
        else TouchPadBridge.button(DEVICE, b.send, down);
    }
    /** the stick's deflection, -1..1 per axis, its length at most 1 */
    static float[] deflection(float dx, float dy, float radius) {
        float len = (float) Math.sqrt(dx * dx + dy * dy), k = len > radius ? 1f / len : 1f / radius;
        return new float[] {dx * k, dy * k};
    }
    private void sendStick(Stick s, int axis) {
        float[] v = s.pointer < 0 ? new float[] {0, 0} : deflection(s.x - s.x0, s.y - s.y0, STICK_DP * dp);
        TouchPadBridge.axis(DEVICE, axis, v[0]);
        TouchPadBridge.axis(DEVICE, axis + 1, v[1]);
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (!registered) { TouchPadBridge.add(DEVICE, "Witch Time touch controls", "witchtime-touch"); registered = true; }
        int a = e.getActionMasked(), i = e.getActionIndex(), id = e.getPointerId(i);
        float x = e.getX(i), y = e.getY(i);
        switch (a) {
        case MotionEvent.ACTION_DOWN:
        case MotionEvent.ACTION_POINTER_DOWN: {
            Button b = hit(x, y);
            if (b != null) { b.pointer = id; send(b, true); }
            else {
                Stick s = x < getWidth() / 2f ? move : look;
                if (s.pointer < 0) { s.pointer = id; s.x0 = s.x = x; s.y0 = s.y = y; }
            }
            break;
        }
        case MotionEvent.ACTION_MOVE:
            for (int k = 0; k < e.getPointerCount(); k++) {
                int p = e.getPointerId(k);
                if (move.pointer == p) { move.x = e.getX(k); move.y = e.getY(k); }
                if (look.pointer == p) { look.x = e.getX(k); look.y = e.getY(k); }
            }
            break;
        case MotionEvent.ACTION_UP:
        case MotionEvent.ACTION_POINTER_UP:
        case MotionEvent.ACTION_CANCEL:
            for (Button b : buttons)
                if (b.pointer == id || a == MotionEvent.ACTION_CANCEL) { if (b.pointer >= 0) send(b, false); b.pointer = -1; }
            if (move.pointer == id || a == MotionEvent.ACTION_CANCEL) move.pointer = -1;
            if (look.pointer == id || a == MotionEvent.ACTION_CANCEL) look.pointer = -1;
            break;
        default:
            return true;
        }
        sendStick(move, 0);
        sendStick(look, 2);
        invalidate();
        return true;
    }

    @Override
    protected void onDraw(Canvas c) {
        for (Button b : buttons) {
            float x = bx(b), y = by(b), r = b.r * dp;
            fill.setColor(b.pointer >= 0 ? 0x88ffffff : 0x33000000);
            c.drawCircle(x, y, r, fill);
            c.drawCircle(x, y, r, line);
            c.drawText(b.label, x, y - (text.descent() + text.ascent()) / 2, text);
        }
        drawStick(c, move, (STICK_DP + 40) * dp, getHeight() - (STICK_DP + 40) * dp);
    }
    /** the move stick; where it rests while no finger holds it (the look stick is not drawn: it is the camera drag) */
    private void drawStick(Canvas c, Stick s, float restX, float restY) {
        float x0 = s.pointer >= 0 ? s.x0 : restX, y0 = s.pointer >= 0 ? s.y0 : restY, r = STICK_DP * dp;
        float[] v = s.pointer >= 0 ? deflection(s.x - s.x0, s.y - s.y0, r) : new float[] {0, 0};
        fill.setColor(0x33000000);
        c.drawCircle(x0, y0, r, fill);
        c.drawCircle(x0, y0, r, line);
        fill.setColor(s.pointer >= 0 ? 0x88ffffff : 0x55ffffff);
        c.drawCircle(x0 + v[0] * r, y0 + v[1] * r, KNOB_DP * dp, fill);
    }

    @Override public void onInputDeviceAdded(int id) { update(); }
    @Override public void onInputDeviceRemoved(int id) { update(); }
    @Override public void onInputDeviceChanged(int id) { update(); }
}
