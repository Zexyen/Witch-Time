package com.zexyen.witchtime;

import android.os.Bundle;
import android.view.ViewGroup;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

import org.libsdl.app.SDLActivity;

/**
 * SDL's activity with the Witch Time runtime (libwitchtime.so, whose SDL_main is runtime/wt_main.c). The app's
 * external files folder (Android/data/com.zexyen.witchtime/files) is a Witch Time folder as on a PC: targets/ (the
 * player's converted dump), config/, and build/game.so, the game library the player's PC built for Android
 * (wtbuild link --target android). Code cannot run from the shared storage, so game.so is copied into the app's
 * own files first and loaded before the runtime, which needs it.
 */
public class WitchTimeActivity extends SDLActivity {
    private TouchControls touch;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        ViewGroup layout = (ViewGroup) getContentView();
        if (layout == null) return;      // the libraries did not load: SDL shows its error
        touch = new TouchControls(this, new File(getExternalFilesDir(null), "config/controllers.ini"));
        layout.addView(touch, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (touch != null) touch.update();     // controllers.ini may have changed while the app was away
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "witchtime" };
    }

    @Override
    public void loadLibraries() {
        File folder = getExternalFilesDir(null);
        File from = new File(folder, "build/game.so");
        File to = new File(getFilesDir(), "game.so");
        if (!from.isFile())
            throw new UnsatisfiedLinkError("There is no " + from + ". Build it on your PC (wtbuild compile --target android, "
                + "then wtbuild link --target android) and copy build/android/game.so there, with the targets folder beside build.");
        if (!to.isFile() || to.length() != from.length() || to.lastModified() < from.lastModified())
            copy(from, to);
        System.load(to.getPath());
        super.loadLibraries();
    }

    private static void copy(File from, File to) {
        File tmp = new File(to.getPath() + ".new");
        try (InputStream in = new FileInputStream(from); OutputStream out = new FileOutputStream(tmp)) {
            byte[] b = new byte[1 << 20];
            for (int n; (n = in.read(b)) > 0; )
                out.write(b, 0, n);
        } catch (IOException e) {
            tmp.delete();
            throw new UnsatisfiedLinkError("Cannot copy " + from + " to " + to + ": " + e.getMessage());
        }
        if (!tmp.renameTo(to))
            throw new UnsatisfiedLinkError("Cannot replace " + to);
    }
}
