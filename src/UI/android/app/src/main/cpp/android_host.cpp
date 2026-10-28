#include <jni.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdlib>
#include <cstdio>
#include <string_view>
#include <SDL.h>
#include <SDL_system.h>
namespace {
struct Utf { JNIEnv* e; jstring s; const char* p; Utf(JNIEnv* env,jstring v):e(env),s(v),p(v?e->GetStringUTFChars(v,nullptr):nullptr){} ~Utf(){if(p)e->ReleaseStringUTFChars(s,p);} };
}
extern "C" JNIEXPORT void JNICALL Java_com_bgiplayer_bgi_1ui_1demo_GameActivity_nativeConfigure(JNIEnv* e,jclass,jstring log,jstring icon,jstring mode,jint language,jboolean traceGpuFrames) {
 Utf l(e,log),i(e,icon),m(e,mode);
 if(l.p){int fd=open(l.p,O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd>=0){fflush(nullptr);dup2(fd,1);dup2(fd,2);close(fd);}}
 if(traceGpuFrames)setenv("BGI_TRACE_GPU_FRAMES","1",1);else unsetenv("BGI_TRACE_GPU_FRAMES");
 setvbuf(stdout,nullptr,_IOLBF,0);setvbuf(stderr,nullptr,_IOLBF,0);
 if(i.p)setenv("BGI_GAME_ICON_OUTPUT",i.p,1);
 setenv("BGI_GPU",m.p && std::string_view(m.p)=="cpuExact"?"0":"1",1);
 setenv("BGIPLAYER_FONT_DIR","/system/fonts",1);
 char lang[16];snprintf(lang,sizeof(lang),"%u",unsigned(language));setenv("BGI_ANDROID_LANGID",lang,1);
 SDL_SetHint(SDL_HINT_ORIENTATIONS,"LandscapeLeft LandscapeRight");
 SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS,"0");
}
extern "C" JNIEXPORT void JNICALL Java_com_bgiplayer_bgi_1ui_1demo_GameActivity_nativeStop(JNIEnv*,jclass) { SDL_Event event{};event.type=SDL_QUIT;SDL_PushEvent(&event); }

extern "C" int SDL_main(int,char**);
extern "C" __attribute__((visibility("default"))) int BGI_AndroidMain(int argc,char** argv) {
 const int code=SDL_main(argc,argv);
 auto* env=static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());auto activity=static_cast<jobject>(SDL_AndroidGetActivity());
 if(env&&activity){auto cls=env->GetObjectClass(activity);auto method=env->GetMethodID(cls,"setEngineExitCode","(I)V");if(method)env->CallVoidMethod(activity,method,code);env->DeleteLocalRef(cls);env->DeleteLocalRef(activity);}
 return code;
}
