package com.bgiplayer.bgi_ui_demo;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Build;
import android.os.Bundle;
import java.util.ArrayList;
import java.util.Locale;
import org.libsdl.app.SDLActivity;

/** SDL owns the game surface, audio, touch and IME in a fresh engine process. */
public final class GameActivity extends SDLActivity {
    public static final String STOP = "com.bgiplayer.bgi_ui_demo.STOP_ENGINE";
    private final BroadcastReceiver stop = new BroadcastReceiver() {
        @Override public void onReceive(Context context, Intent intent) { nativeStop(); }
    };
    private static native void nativeConfigure(String log, String icon, String mode, int language, boolean traceGpuFrames);
    private static native void nativeStop();
    @Override protected String[] getLibraries() { return new String[]{"SDL2", "bgiplayer"}; }
    @Override protected String getMainSharedObject() { return getApplicationInfo().nativeLibraryDir + "/libbgiplayer.so"; }
    @Override protected String getMainFunction() { return "BGI_AndroidMain"; }
    @Override protected String[] getArguments() {
        Intent i=getIntent();String mode=i.getStringExtra("renderer");
        nativeConfigure(i.getStringExtra("logPath"),i.getStringExtra("iconPath"),mode,languageId(),i.getBooleanExtra("traceGpuFrames",false));
        ArrayList<String> args=new ArrayList<>();
        args.add("--game");args.add(i.getStringExtra("path"));args.add("--backend");args.add("sdl2");
        args.add("--render-mode");args.add("gpuFast".equals(mode)?"fast":"exact");
        String font=i.getStringExtra("defaultFont");if(font!=null && !font.isEmpty()){args.add("--default-font");args.add(font);}
        return args.toArray(new String[0]);
    }
    private int languageId() {
        Locale l=getResources().getConfiguration().getLocales().get(0);
        switch(l.getLanguage()) {
            case "ja":return 0x411;case "zh":return "TW".equals(l.getCountry())||"HK".equals(l.getCountry())||"Hant".equals(l.getScript())?0x404:0x804;
            case "ko":return 0x412;case "de":return 0x407;case "fr":return 0x40c;case "es":return 0xc0a;case "ru":return 0x419;
            case "it":return 0x410;case "pt":return "BR".equals(l.getCountry())?0x416:0x816;case "th":return 0x41e;case "vi":return 0x42a;
            default:return 0x409;
        }
    }
    // Called by the native entry wrapper before SDL finishes this Activity.
    public void setEngineExitCode(int code) { setResult(code==0?RESULT_OK:RESULT_FIRST_USER,new Intent().putExtra("exitCode",code)); }
    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if(focused) {
            if(Build.VERSION.SDK_INT>=30) {
                android.view.WindowInsetsController controller=getWindow().getInsetsController();
                if(controller!=null){controller.hide(android.view.WindowInsets.Type.systemBars());controller.setSystemBarsBehavior(android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);}
            } else getWindow().getDecorView().setSystemUiVisibility(android.view.View.SYSTEM_UI_FLAG_FULLSCREEN|android.view.View.SYSTEM_UI_FLAG_HIDE_NAVIGATION|android.view.View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        }
    }
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        if(Build.VERSION.SDK_INT>=33)registerReceiver(stop,new IntentFilter(STOP),Context.RECEIVER_NOT_EXPORTED);
        else registerReceiver(stop,new IntentFilter(STOP));
    }
    @Override protected void onDestroy(){unregisterReceiver(stop);super.onDestroy();}
}
