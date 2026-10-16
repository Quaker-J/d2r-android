// 原游戏宿主的 NativeActivity 前端：真正运行 nnMain 并呈现游戏交换链。
// NativeActivity frontend for the real guest host: execute nnMain and present its swapchain.
#define _GNU_SOURCE
#include "../../host/dm.h"
#include "../../host/hle.h"
#include "../../host/front.h"
#include "../../host/dm_input.h"
#include "../../host/dm_i18n.h"
#include "../../host/dm_hud.h"
#include "debug_stats.h"
#include "performance_hint.h"
#include "launch_settings.h"
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/window.h>
#include <android/input.h>
#include <android/looper.h>
#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdatomic.h>
#include <limits.h>
#include <float.h>
#include <math.h>

typedef struct {
    ANativeActivity *activity; ANativeWindow *window; AInputQueue *input;
    pthread_mutex_t lock; pthread_cond_t ready; pthread_t worker;
    int stop; _Atomic int started;
    uint64_t pad_buttons,move_keys;int tap_a;int32_t lx,ly,rx,ry;
    uint64_t touch_buttons;int32_t touch_lx,touch_ly,touch_rx,touch_ry;
    pthread_t debug_thread;int debug_started;_Atomic int debug_stop;
    float requested_frame_rate;int frame_rate_attempted;
} game_app;
static game_app *application;
static _Atomic int display_hz;
int dm_android_display_hz(void) {return atomic_load(&display_hz);}
static pthread_once_t frame_rate_once=PTHREAD_ONCE_INIT;
static int32_t (*window_set_frame_rate)(ANativeWindow *,float,int8_t);
static void frame_rate_api_load(void) {
    // API30符号动态解析；API29仍可加载。库句柄保留至进程退出。
    // Resolve the API30 symbol dynamically for API29 loading; retain its library for process lifetime.
    void *library=dlopen("libandroid.so",RTLD_NOW|RTLD_LOCAL);
    if(library){void *symbol=dlsym(library,"ANativeWindow_setFrameRate");memcpy(&window_set_frame_rate,&symbol,sizeof window_set_frame_rate);}
}
typedef struct { ANativeActivity *activity;float rate; } frame_rate_request;
static void *frame_rate_java_request(void *argument) {
    frame_rate_request *request=argument;ANativeActivity *a=request->activity;JNIEnv *env=NULL;int attached=0;
    // 使用真实宿主pthread栈附着ART，不能从已换栈的客体线程直接附着。
    // Attach ART from a real host pthread stack, never from a guest thread's replaced stack.
    if((*a->vm)->GetEnv(a->vm,(void**)&env,JNI_VERSION_1_6)!=JNI_OK){
        if((*a->vm)->AttachCurrentThread(a->vm,&env,NULL)!=JNI_OK){LOG("[android-display-preference] ART attach failed");return NULL;}attached=1;
    }
    jclass klass=(*env)->GetObjectClass(env,a->clazz);
    jmethodID method=klass?(*env)->GetMethodID(env,klass,"requestDisplayFrameRate","(F)V"):NULL;
    if(method&&!(*env)->ExceptionCheck(env))(*env)->CallVoidMethod(env,a->clazz,method,request->rate);
    if((*env)->ExceptionCheck(env)){(*env)->ExceptionDescribe(env);(*env)->ExceptionClear(env);LOG("[android-display-preference] JNI request failed rate=%.6f",request->rate);}
    else if(!method)LOG("[android-display-preference] Java method unavailable");
    else LOG("[android-display-preference] UI request queued rate=%.6f Hz (not measured refresh)",request->rate);
    if(klass)(*env)->DeleteLocalRef(env,klass);if(attached)(*a->vm)->DetachCurrentThread(a->vm);return NULL;
}
void dm_android_window_frame_period(double period) {
    game_app *s=application;if(!s||!isfinite(period)||period<0)return;
    double rate=period>0?1.0/period:0;if(!isfinite(rate)||rate>FLT_MAX)return;
    float requested=(float)rate;if(period>0&&requested<=0)return;
    pthread_once(&frame_rate_once,frame_rate_api_load);
    // 窗口引用由前端持有；锁阻止销毁/替换。每个窗口只在目标变化时调用一次。
    // The frontend owns the window reference; the lock excludes replacement/destruction. Call only on target changes.
    int changed=0;pthread_mutex_lock(&s->lock);
    if(s->window&&(!s->frame_rate_attempted||s->requested_frame_rate!=requested)){
        changed=1;s->frame_rate_attempted=1;s->requested_frame_rate=requested;
        if(window_set_frame_rate){
            int32_t result=window_set_frame_rate(s->window,requested,ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT);
            LOG("[android-frame-rate] requested=%.6f Hz period=%.9f s compatibility=default seamless_only=1 result=%d (request, not measured refresh)",requested,period,result);
        }else LOG("[android-frame-rate] API unavailable requested=%.6f Hz; retain system display policy",requested);
    }
    pthread_mutex_unlock(&s->lock);
    // 不持窗口锁调用JNI；Java只向UI排队，不等待UI，避免暂停回调与游戏线程互等。
    // Call JNI outside the window lock; Java queues without waiting for UI to avoid lifecycle deadlocks.
    if(changed){frame_rate_request request={s->activity,requested};pthread_t thread;
        int error=pthread_create(&thread,NULL,frame_rate_java_request,&request);
        if(error)LOG("[android-display-preference] host thread creation failed error=%d",error);
        else pthread_join(thread,NULL);
    }
}
static struct { pthread_mutex_t lock; pthread_cond_t ready; uint64_t serial; int done,result; char text[4096]; } text_request = {
    .lock=PTHREAD_MUTEX_INITIALIZER,.ready=PTHREAD_COND_INITIALIZER
};
size_t swkbd_u16_to_u8(const uint16_t *,size_t,char *,size_t);
size_t swkbd_u8_to_u16(const char *,size_t,uint16_t *,size_t);
static jstring java_text(JNIEnv *env,const char *text) {
    if(!text)text="";size_t cap=strlen(text)+1;uint16_t *units=malloc(cap*sizeof *units);if(!units)die("Android text allocation");
    size_t length=swkbd_u8_to_u16(text,cap-1,units,cap);jstring result=(*env)->NewString(env,units,length);free(units);return result;
}
__attribute__((visibility("default"))) void Java_org_d2r_android_port_D2RActivity_onTextResult(JNIEnv *env,jclass klass,jlong serial,jstring text,jboolean accepted) {
    (void)klass;pthread_mutex_lock(&text_request.lock);
    if(serial==(jlong)text_request.serial&&!text_request.done){
        text_request.text[0]=0;
        if(accepted&&text){const jchar *units=(*env)->GetStringChars(env,text,NULL);if(units){swkbd_u16_to_u8(units,(*env)->GetStringLength(env,text),text_request.text,sizeof text_request.text);(*env)->ReleaseStringChars(env,text,units);}else accepted=JNI_FALSE;}
        text_request.result=accepted?1:0;text_request.done=1;pthread_cond_signal(&text_request.ready);
    }pthread_mutex_unlock(&text_request.lock);
}
static void *progress_watch(void *argument) {
    game_app *s=argument;ANativeActivity *a=s->activity;JNIEnv *env=NULL;if((*a->vm)->AttachCurrentThread(a->vm,&env,NULL)!=JNI_OK)return NULL;
    jclass klass=(*env)->GetObjectClass(env,a->clazz);jmethodID method=(*env)->GetMethodID(env,klass,"updateDebug","(Ljava/lang/String;)V");
    if(!method){(*env)->ExceptionDescribe(env);(*env)->ExceptionClear(env);(*a->vm)->DetachCurrentThread(a->vm);return NULL;}
    // 电池查询方法一次查找、每轮复用；任一查找失败就整组降级为显示 --，不让缺方法拖垮调试线程。
    // Battery query methods are resolved once and reused; if any lookup fails the whole group degrades to "--" rather than failing the debug thread.
    jmethodID battery_current=NULL,battery_voltage=NULL,battery_level=NULL,battery_status=NULL;
    battery_current=(*env)->GetMethodID(env,klass,"batteryCurrentMa","()I");
    battery_voltage=(*env)->GetMethodID(env,klass,"batteryVoltageMv","()I");
    battery_level=(*env)->GetMethodID(env,klass,"batteryLevelPercent","()I");
    battery_status=(*env)->GetMethodID(env,klass,"batteryStatusValue","()I");
    if((*env)->ExceptionCheck(env)){(*env)->ExceptionDescribe(env);(*env)->ExceptionClear(env);battery_current=battery_voltage=battery_level=battery_status=NULL;}
    // 浮层开关由启动界面写进 launch-settings.conf，经 DM_DEBUG_OVERLAY 传到这里；只在启动时读一次，运行中不切换。
    // The launcher writes the overlay switch into launch-settings.conf and it reaches us as DM_DEBUG_OVERLAY;
    // read once at startup, never toggled at runtime. Absent or any value other than "0" keeps the overlay on.
    const char *overlay_setting=getenv("DM_DEBUG_OVERLAY");
    int overlay=!(overlay_setting&&!strcmp(overlay_setting,"0"));
    jmethodID overlay_method=(*env)->GetMethodID(env,klass,"setDebugOverlay","(Z)V");
    if((*env)->ExceptionCheck(env)){(*env)->ExceptionDescribe(env);(*env)->ExceptionClear(env);overlay_method=NULL;}
    if(overlay_method)(*env)->CallVoidMethod(env,a->clazz,overlay_method,(jboolean)(overlay!=0));
    else LOG("[android] debug overlay switch unavailable; showing the overlay");
    uint64_t previous=0;unsigned unchanged=0;
    for(unsigned i=0;!atomic_load(&s->debug_stop);i++){usleep(500000);dm_android_debug_stats stats;dm_android_debug_get(&stats);dm_hud_info hud;dm_hud_get(&hud);
        pthread_mutex_lock(&text_request.lock);int keyboard=text_request.serial&&!text_request.done;pthread_mutex_unlock(&text_request.lock);
        // 浮层关掉时整块显示工作都跳过：不查电池、不读 /proc、不拼字符串、不碰 UI 线程。
        // 但下面的首帧看门狗必须照跑，它跟显示无关。
        // With the overlay off this whole display block is skipped: no battery query, no /proc read, no string
        // building, no UI-thread call. The first-frame watchdog below must still run; it is not a display concern.
        if(overlay){
            int dm_loadscreen_up(void);const char *stage=dm_ui(keyboard?"dmAndroidStageText":dm_loadscreen_up()?"dmAndroidStageLoading":hud.valid?"dmAndroidStageGame":stats.frame?"dmAndroidStageMenu":"dmAndroidStageBoot");
            unsigned long pages=0;FILE *memory=fopen("/proc/self/statm","r");if(memory){unsigned long total;fscanf(memory,"%lu %lu",&total,&pages);fclose(memory);}
            char gpu[32];if(stats.gpu_valid)snprintf(gpu,sizeof gpu,"%.1f",stats.gpu_ms);else snprintf(gpu,sizeof gpu,"--");
            // 电池读数经框架获取（应用不需要 root）；读不到就明确显示 --，不伪造数值。
            // Battery readings come from the framework (no root needed); anything unavailable shows "--" rather than a made-up number.
            char power[160];
            if(battery_current&&battery_voltage&&battery_level&&battery_status){
                int milliamps=(*env)->CallIntMethod(env,a->clazz,battery_current);
                int millivolts=(*env)->CallIntMethod(env,a->clazz,battery_voltage);
                int capacity=(*env)->CallIntMethod(env,a->clazz,battery_level);
                int status=(*env)->CallIntMethod(env,a->clazz,battery_status);
                if((*env)->ExceptionCheck(env)){(*env)->ExceptionDescribe(env);(*env)->ExceptionClear(env);milliamps=-1;}
                char value[64];
                if(milliamps<0)snprintf(value,sizeof value,"--");
                else if(millivolts>0){double volts=millivolts/1000.0;snprintf(value,sizeof value,"%d mA | %.2f V | %.1f W",milliamps,volts,milliamps*volts/1000.0);}
                else snprintf(value,sizeof value,"%d mA",milliamps);
                // 本机放电时 HAL 也返回正值，方向不能靠符号判断；充电与否用框架状态（2 充电 / 5 已满）决定。
                // This HAL reports a positive value while discharging too, so direction comes from the framework status (2 charging / 5 full), not the sign.
                int charging=status==2||status==5;
                snprintf(power,sizeof power,dm_ui(charging?"dmAndroidCharging":"dmAndroidPower"),value,capacity<0?0.0:(double)capacity);
            }else snprintf(power,sizeof power,dm_ui("dmAndroidPower"),"--",0.0);
            char text[1024];snprintf(text,sizeof text,dm_ui("dmAndroidDebugMetrics"),stats.frame_ms>0?1000/stats.frame_ms:0,stats.frame_ms,stats.cpu_ms,gpu,stats.wait_ms,power,stats.draws,stats.passes,stats.submits,stats.snapshot_mb,stats.buffer_mb,pages*sysconf(_SC_PAGESIZE)/1048576.,stage,hud.area,hud.x,hud.y,(unsigned long long)dm_input_samples());
            jstring value=java_text(env,text);(*env)->CallVoidMethod(env,a->clazz,method,value);(*env)->DeleteLocalRef(env,value);
            if((*env)->ExceptionCheck(env)){(*env)->ExceptionDescribe(env);(*env)->ExceptionClear(env);break;}
        }
        if(i<96&&i%4==0){uint64_t frame=dm_nvn_frame_count();LOG("[android] game progress frame=%llu input_samples=%llu",(unsigned long long)frame,(unsigned long long)dm_input_samples());
            if(frame&&frame==previous&&!keyboard){if(++unchanged==4)crash_dump_now("Android first-frame progress stalled");}else unchanged=0;previous=frame;}
    }
    (*env)->DeleteLocalRef(env,klass);(*a->vm)->DetachCurrentThread(a->vm);return NULL;
}

static void copy_asset(ANativeActivity *a,const char *directory,const char *name) {
    AAsset *asset=AAssetManager_open(a->assetManager,name,AASSET_MODE_STREAMING);if(!asset)die("Missing game metadata asset %s",name);
    char path[1024];snprintf(path,sizeof path,"%s/%s",directory,name);FILE *file=fopen(path,"wb");if(!file)die("Game metadata open %s",path);
    char buffer[4096];int n;while((n=AAsset_read(asset,buffer,sizeof buffer))>0)if(fwrite(buffer,1,n,file)!=(size_t)n)die("Game metadata write");fclose(file);AAsset_close(asset);
}
static void *boot(void *argument) {
    game_app *s=argument;pthread_mutex_lock(&s->lock);while(!s->window&&!s->stop)pthread_cond_wait(&s->ready,&s->lock);int stop=s->stop;pthread_mutex_unlock(&s->lock);if(stop)return NULL;
    char path[1024],metadata[1024],log[1024];snprintf(metadata,sizeof metadata,"%s/game",s->activity->internalDataPath);mkdir(metadata,0700);
    snprintf(log,sizeof log,"%s/game.log",s->activity->internalDataPath);int fd=open(log,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600);if(fd>=0){dup2(fd,STDERR_FILENO);dup2(fd,STDOUT_FILENO);close(fd);}
    // ART 查询只在游戏开跑前的真实宿主 pthread 上进行；失败保留0，不伪造屏幕信息。
    // Query ART on the real host pthread before starting the guest; retain zero on failure rather than invent display information.
    {
        ANativeActivity *a=s->activity;JNIEnv *env=NULL;int attached=0,hz=0;float rate=0;
        if((*a->vm)->GetEnv(a->vm,(void**)&env,JNI_VERSION_1_6)!=JNI_OK){if((*a->vm)->AttachCurrentThread(a->vm,&env,NULL)==JNI_OK)attached=1;else env=NULL;}
        if(env){jclass klass=(*env)->GetObjectClass(env,a->clazz);jmethodID method=klass?(*env)->GetMethodID(env,klass,"getMaximumDisplayRefreshRate","()F"):NULL;
            if(method&&!(*env)->ExceptionCheck(env))rate=(*env)->CallFloatMethod(env,a->clazz,method);
            if((*env)->ExceptionCheck(env)){(*env)->ExceptionDescribe(env);(*env)->ExceptionClear(env);rate=0;}
            double rounded=(double)rate+.5;if(rate>0&&rounded<=INT_MAX)hz=(int)rounded;
            if(klass)(*env)->DeleteLocalRef(env,klass);if(attached)(*a->vm)->DetachCurrentThread(a->vm);
        }
        atomic_store(&display_hz,hz);LOG("[android] display maximum refresh=%.3f Hz reported=%d Hz (read-only capability query)",rate,hz);
    }
    const char *modules[]={"main","sdk","subsdk0"};for(unsigned i=0;i<3;i++)for(unsigned j=0;j<2;j++){char name[80];snprintf(name,sizeof name,"%s.%s.tsv",modules[i],j?"gates":"fixups");copy_asset(s->activity,metadata,name);}
    Dl_info info;if(!dladdr((void*)boot,&info)||!info.dli_fname)die("Native library directory lookup");char library[1024];snprintf(library,sizeof library,"%s",info.dli_fname);char *slash=strrchr(library,'/');if(!slash)die("Native library path");*slash=0;setenv("DM_ANDROID_LIBDIR",library,1);
    snprintf(path,sizeof path,"%s/romfs",s->activity->externalDataPath);setenv("DM_ROMFS",path,1);
    snprintf(path,sizeof path,"%s/shaders",s->activity->externalDataPath);setenv("DM_VK_SHADERS",path,1);
    snprintf(path,sizeof path,"%s/save",s->activity->internalDataPath);mkdir(path,0700);setenv("DM_SAVE",path,1);
    // 私有可重建的驱动缓存独立于游戏存档；不可写时正常使用内存缓存。
    // Keep rebuildable driver data separate from saves; an unwritable directory falls back to the in-memory cache.
    int cache_path=snprintf(path,sizeof path,"%s/pipeline-cache",s->activity->internalDataPath);
    if(cache_path>0&&(size_t)cache_path<sizeof path&&(!mkdir(path,0700)||errno==EEXIST))setenv("DM_VK_PIPELINE_CACHE_DIR",path,1);
    else{unsetenv("DM_VK_PIPELINE_CACHE_DIR");LOG("[vulkan-pipeline-cache] directory_unavailable=1");}
    snprintf(path,sizeof path,"%s/temp",s->activity->internalDataPath);mkdir(path,0700);setenv("DM_TEMP",path,1);
    // 在游戏首次缓存运行档及输入日志之前应用完整验证过的本应用设置。
    // Apply fully validated application settings before the guest first caches its operation mode and input log.
    dm_android_launch_settings launch;char launch_error[512];
    if(!dm_android_launch_settings_apply(path,&launch,launch_error,sizeof launch_error))die("[android] launch settings: %s",launch_error);
    LOG("[android] launch configuration present=%d operation_mode=%d input_receipts=%d",launch.present,launch.operation_mode,launch.input_receipts);
    setenv("DM_TACT_PREFETCH","0",1);setenv("DM_BOOTPROF","1",1);setenv("DM_DBG","1",1);
    setenv("DM_UIMSG","1",1);setenv("DM_MOVIE","1",1);
    __android_log_print(ANDROID_LOG_INFO,"D2RAndroid","Starting real game host; log: %s",log);atomic_store(&s->started,1);
    char *args[]={"dmhost-android",metadata,"main","sdk","subsdk0",NULL};int dm_main(int,char**);dm_main(5,args);ANativeActivity_finish(s->activity);return NULL;
}
void front_run(void (*guest)(void)) {void mtl_init(void);mtl_init();game_app *s=application;if(!pthread_create(&s->debug_thread,NULL,progress_watch,s))s->debug_started=1;guest();}
void *front_metal_layer(void) {if(!application)return NULL;pthread_mutex_lock(&application->lock);ANativeWindow *w=application->window;pthread_mutex_unlock(&application->lock);return w;}
uint64_t front_game_res(int docked) {return docked?((uint64_t)900<<32)|1600:((uint64_t)720<<32)|1280;}
static int text_input_request(const char *title,const char *sub,const char *guide,const char *initial,int max,char *out,int cap) {
    if(!application||!out||cap<=0)return -1;ANativeActivity *a=application->activity;JNIEnv *env=NULL;int attached=0;
    if((*a->vm)->GetEnv(a->vm,(void**)&env,JNI_VERSION_1_6)!=JNI_OK){if((*a->vm)->AttachCurrentThread(a->vm,&env,NULL)!=JNI_OK)return -1;attached=1;}
    extern int g_swkbd_name_panel,g_swkbd_numeric_max;int name=g_swkbd_name_panel,numeric=g_swkbd_numeric_max;
    char message[2048];snprintf(message,sizeof message,"%s%s%s%s%s",sub?sub:"",sub&&*sub&&guide&&*guide?"\n":"",guide?guide:"",name?"\n":"",name?dm_ui("dmNameRule"):"");
    if(numeric>=0)snprintf(message,sizeof message,"0 – %d",numeric);
    const char *heading=title&&*title?title:dm_ui(name?"dmNameTitle":numeric>=0?"dmQtyTitle":"dmInputTitle");
    pthread_mutex_lock(&text_request.lock);uint64_t serial=++text_request.serial;text_request.done=0;text_request.result=-1;pthread_mutex_unlock(&text_request.lock);
    // Java 的 UI 调度不持结果锁；回调可以在进入等待前完成。
    // Do not hold the result lock while scheduling Java UI; the callback may complete before the wait starts.
    jclass klass=(*env)->GetObjectClass(env,a->clazz);jmethodID method=(*env)->GetMethodID(env,klass,"requestText","(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;IIZ)V");
    jstring strings[4]={java_text(env,heading),java_text(env,message),java_text(env,name?dm_ui("dmNameBlankRandom"):guide),java_text(env,initial)};
    if(method&&!(*env)->ExceptionCheck(env))(*env)->CallVoidMethod(env,a->clazz,method,(jlong)serial,strings[0],strings[1],strings[2],strings[3],max,numeric,name?JNI_TRUE:JNI_FALSE);
    int failed=!method||(*env)->ExceptionCheck(env);if(failed){(*env)->ExceptionDescribe(env);(*env)->ExceptionClear(env);}
    for(unsigned i=0;i<4;i++)if(strings[i])(*env)->DeleteLocalRef(env,strings[i]);(*env)->DeleteLocalRef(env,klass);if(attached)(*a->vm)->DetachCurrentThread(a->vm);
    if(failed)return -1;
    pthread_mutex_lock(&text_request.lock);while(!text_request.done)pthread_cond_wait(&text_request.ready,&text_request.lock);
    int result=text_request.result;size_t n=strlen(text_request.text);if(n>=(size_t)cap){n=cap-1;while(n&&((unsigned char)text_request.text[n]&0xc0)==0x80)n--;}
    memcpy(out,text_request.text,n);out[n]=0;pthread_mutex_unlock(&text_request.lock);return result;
}
typedef struct { const char *title,*sub,*guide,*initial; int max,cap,result; char *out; } text_call;
static pthread_mutex_t text_call_lock=PTHREAD_MUTEX_INITIALIZER;
static void *text_input_worker(void *argument) {
    text_call *call=argument;call->result=text_input_request(call->title,call->sub,call->guide,call->initial,call->max,call->out,call->cap);return NULL;
}
int front_text_input(const char *title,const char *sub,const char *guide,const char *initial,int max,char *out,int cap) {
    // 客体线程换到客体栈后不能附着 ART；JNI 全部在有真实 pthread 栈元数据的宿主线程调用。
    // A guest thread running on its guest stack cannot attach to ART; perform all JNI calls on a host thread with genuine pthread stack metadata.
    pthread_mutex_lock(&text_call_lock);text_call call={title,sub,guide,initial,max,cap,-1,out};pthread_t thread;
    int error=pthread_create(&thread,NULL,text_input_worker,&call);if(!error)pthread_join(thread,NULL);
    pthread_mutex_unlock(&text_call_lock);return call.result;
}
int dm_thermal_now(void) {
    void *library=dlopen("libandroid.so",RTLD_NOW|RTLD_LOCAL);if(!library)return -1;
    void *(*acquire)(void)=dlsym(library,"AThermal_acquireManager");void (*release)(void*)=dlsym(library,"AThermal_releaseManager");int (*status)(void*)=dlsym(library,"AThermal_getCurrentThermalStatus");int result=-1;
    if(acquire&&release&&status){void *manager=acquire();if(manager){int v=status(manager);result=v<0?-1:v==0?0:v<=2?1:v==3?2:3;release(manager);}}dlclose(library);return result;
}
static void pad_publish(game_app *s) {
    int32_t x=s->lx,y=s->ly;
    if(s->move_keys){x=((!!(s->move_keys&DM_BTN_RIGHT))-(!!(s->move_keys&DM_BTN_LEFT)))*DM_STICK_MAX;y=((!!(s->move_keys&DM_BTN_UP))-(!!(s->move_keys&DM_BTN_DOWN)))*DM_STICK_MAX;}
    // 分开合并实体与屏幕手柄；抬起一只手不会清掉另一输入来源仍按住的键。
    // Merge physical and on-screen pads separately; releasing one source never clears a key held by another.
    if(!x&&!y){x=s->touch_lx;y=s->touch_ly;}
    int32_t rx=s->rx,ry=s->ry;if(!rx&&!ry){rx=s->touch_rx;ry=s->touch_ry;}
    dm_pad_set(s->pad_buttons|s->touch_buttons|(s->tap_a?DM_BTN_A:0),x,y,rx,ry);
}
static void pad_clear(game_app *s) {s->pad_buttons=s->move_keys=s->touch_buttons=0;s->tap_a=0;s->lx=s->ly=s->rx=s->ry=s->touch_lx=s->touch_ly=s->touch_rx=s->touch_ry=0;pad_publish(s);}
static int32_t touch_axis(jint value) {return value>DM_STICK_MAX?DM_STICK_MAX:value< -DM_STICK_MAX?-DM_STICK_MAX:value;}
__attribute__((visibility("default"))) void Java_org_d2r_android_port_D2RActivity_onTouchPad(JNIEnv *env,jclass klass,jlong buttons,jint lx,jint ly,jint rx,jint ry) {
    (void)env;(void)klass;game_app *s=application;if(!s)return;
    // Java 窗口触控和 AInputQueue 都在 Activity 的主 Looper；统一走 HLE 的短按锁存。
    // Java window touches and AInputQueue run on the Activity's main Looper; both use the HLE press-edge latch.
    s->touch_buttons=(uint64_t)buttons&0xffff;s->touch_lx=touch_axis(lx);s->touch_ly=touch_axis(ly);s->touch_rx=touch_axis(rx);s->touch_ry=touch_axis(ry);pad_publish(s);
}
static int32_t stick_value(float value) {if(value>1)value=1;if(value<-1)value=-1;return value>-.15f&&value<.15f?0:(int32_t)(value*DM_STICK_MAX);}
static uint64_t pad_key(int key) {
    switch(key){case AKEYCODE_DPAD_UP:return DM_BTN_UP;case AKEYCODE_DPAD_DOWN:return DM_BTN_DOWN;case AKEYCODE_DPAD_LEFT:return DM_BTN_LEFT;case AKEYCODE_DPAD_RIGHT:return DM_BTN_RIGHT;
        case AKEYCODE_ENTER:case AKEYCODE_SPACE:case AKEYCODE_BUTTON_A:return DM_BTN_A;case AKEYCODE_ESCAPE:case AKEYCODE_BUTTON_B:return DM_BTN_B;
        case AKEYCODE_BUTTON_X:return DM_BTN_X;case AKEYCODE_BUTTON_Y:return DM_BTN_Y;case AKEYCODE_BUTTON_L1:return DM_BTN_L;case AKEYCODE_BUTTON_R1:return DM_BTN_R;
        case AKEYCODE_BUTTON_L2:return DM_BTN_ZL;case AKEYCODE_BUTTON_R2:return DM_BTN_ZR;case AKEYCODE_BUTTON_START:return DM_BTN_PLUS;case AKEYCODE_BUTTON_SELECT:return DM_BTN_MINUS;
        case AKEYCODE_BUTTON_THUMBL:return DM_BTN_LS;case AKEYCODE_BUTTON_THUMBR:return DM_BTN_RS;default:return 0;}
}
static int input_ready(int fd,int events,void *data) {
    (void)fd;(void)events;game_app *s=data;AInputEvent *event;
    while(AInputQueue_getEvent(s->input,&event)>=0){if(AInputQueue_preDispatchEvent(s->input,event))continue;int handled=0;
        if(AInputEvent_getType(event)==AINPUT_EVENT_TYPE_MOTION&&(AInputEvent_getSource(event)&AINPUT_SOURCE_JOYSTICK)==AINPUT_SOURCE_JOYSTICK){
            s->lx=stick_value(AMotionEvent_getAxisValue(event,AMOTION_EVENT_AXIS_X,0));s->ly=stick_value(-AMotionEvent_getAxisValue(event,AMOTION_EVENT_AXIS_Y,0));
            float x=AMotionEvent_getAxisValue(event,AMOTION_EVENT_AXIS_Z,0),y=AMotionEvent_getAxisValue(event,AMOTION_EVENT_AXIS_RZ,0);if(!x)x=AMotionEvent_getAxisValue(event,AMOTION_EVENT_AXIS_RX,0);if(!y)y=AMotionEvent_getAxisValue(event,AMOTION_EVENT_AXIS_RY,0);s->rx=stick_value(x);s->ry=stick_value(-y);pad_publish(s);handled=1;
        }else if(AInputEvent_getType(event)==AINPUT_EVENT_TYPE_MOTION){int raw_action=AMotionEvent_getAction(event),action=raw_action&AMOTION_EVENT_ACTION_MASK;unsigned lifted=(raw_action&AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)>>AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;dm_touch touches[16];int count=0;
            pthread_mutex_lock(&s->lock);ANativeWindow *window=s->window;int w=window?ANativeWindow_getWidth(window):1,h=window?ANativeWindow_getHeight(window):1;pthread_mutex_unlock(&s->lock);
            float gw=w,gh=w*9.f/16.f;if(gh>h){gh=h;gw=h*16.f/9.f;}float left=(w-gw)/2,top=(h-gh)/2;
            if(action!=AMOTION_EVENT_ACTION_UP&&action!=AMOTION_EVENT_ACTION_CANCEL)for(unsigned i=0;i<AMotionEvent_getPointerCount(event)&&count<16;i++){if(action==AMOTION_EVENT_ACTION_POINTER_UP&&i==lifted)continue;float x=(AMotionEvent_getX(event,i)-left)/gw,y=(AMotionEvent_getY(event,i)-top)/gh;if(x>=0&&x<1&&y>=0&&y<1)touches[count++]=(dm_touch){AMotionEvent_getPointerId(event,i)&15,x,y};}
            dm_touch_set(touches,count);
            // 首画面阶段：点击可确认标题/提示；按下沿由 HLE 锁存，不丢短按。
            // First-frame controls: taps confirm title/prompts; HLE latches press edges so short taps are retained.
            if(action==AMOTION_EVENT_ACTION_DOWN)s->tap_a=1;else if(action==AMOTION_EVENT_ACTION_UP||action==AMOTION_EVENT_ACTION_CANCEL)s->tap_a=0;pad_publish(s);handled=1;
        }else if(AInputEvent_getType(event)==AINPUT_EVENT_TYPE_KEY){int key=AKeyEvent_getKeyCode(event),down=AKeyEvent_getAction(event)==AKEY_EVENT_ACTION_DOWN;uint64_t button=pad_key(key),move=key==AKEYCODE_W?DM_BTN_UP:key==AKEYCODE_A?DM_BTN_LEFT:key==AKEYCODE_S?DM_BTN_DOWN:key==AKEYCODE_D?DM_BTN_RIGHT:0;
            if(button){if(down)s->pad_buttons|=button;else s->pad_buttons&=~button;handled=1;}if(move){if(down)s->move_keys|=move;else s->move_keys&=~move;handled=1;}if(handled)pad_publish(s);}
        AInputQueue_finishEvent(s->input,event,handled);
    }return 1;
}
static void window_created(ANativeActivity *a,ANativeWindow *w) {game_app *s=a->instance;pthread_mutex_lock(&s->lock);ANativeWindow_acquire(w);s->window=w;s->frame_rate_attempted=0;pthread_cond_signal(&s->ready);pthread_mutex_unlock(&s->lock);}
static void window_destroyed(ANativeActivity *a,ANativeWindow *w) {game_app *s=a->instance;if(atomic_load(&s->started))dm_host_shutdown("Android game window destroyed");pthread_mutex_lock(&s->lock);if(s->window==w){s->window=NULL;ANativeWindow_release(w);}pthread_mutex_unlock(&s->lock);}
static void input_created(ANativeActivity *a,AInputQueue *q) {game_app *s=a->instance;s->input=q;AInputQueue_attachLooper(q,ALooper_forThread(),0,input_ready,s);}
static void input_destroyed(ANativeActivity *a,AInputQueue *q) {game_app *s=a->instance;AInputQueue_detachLooper(q);s->input=NULL;dm_touch_set(NULL,0);pad_clear(s);}
static void resumed(ANativeActivity *a) {game_app *s=a->instance;dm_android_performance_resume();if(atomic_load(&s->started)){dm_gfx_suspend(0);dm_audio_suspend(0);dm_focus_set(1);}}
static void paused(ANativeActivity *a) {game_app *s=a->instance;dm_touch_set(NULL,0);pad_clear(s);if(atomic_load(&s->started)){dm_focus_set(3);dm_audio_suspend(1);dm_gfx_suspend(1);}dm_android_performance_close();}
static void destroyed(ANativeActivity *a) {game_app *s=a->instance;atomic_store(&s->debug_stop,1);if(atomic_load(&s->started))dm_host_shutdown("Android activity destroyed");pthread_mutex_lock(&s->lock);s->stop=1;pthread_cond_signal(&s->ready);pthread_mutex_unlock(&s->lock);pthread_join(s->worker,NULL);if(s->debug_started)pthread_join(s->debug_thread,NULL);if(s->window)ANativeWindow_release(s->window);free(s);application=NULL;}
__attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity *a,void *saved,size_t bytes) {
    // NativeActivity 自行 dlopen，不登记 Java 类加载器的 native 库；显式注册文本与触控回调。
    // NativeActivity dlopens its library without Java class-loader native-library registration; explicitly register text and touch callbacks.
    jclass klass=(*a->env)->GetObjectClass(a->env,a->clazz);JNINativeMethod methods[]={
        {"onTextResult","(JLjava/lang/String;Z)V",(void*)Java_org_d2r_android_port_D2RActivity_onTextResult},
        {"onTouchPad","(JIIII)V",(void*)Java_org_d2r_android_port_D2RActivity_onTouchPad}
    };
    if((*a->env)->RegisterNatives(a->env,klass,methods,sizeof methods/sizeof methods[0])!=JNI_OK){(*a->env)->ExceptionDescribe(a->env);(*a->env)->ExceptionClear(a->env);ANativeActivity_finish(a);return;}(*a->env)->DeleteLocalRef(a->env,klass);
    (void)saved;(void)bytes;game_app *s=calloc(1,sizeof *s);if(!s){ANativeActivity_finish(a);return;}s->activity=a;a->instance=s;application=s;pthread_mutex_init(&s->lock,NULL);pthread_cond_init(&s->ready,NULL);
    a->callbacks->onNativeWindowCreated=window_created;a->callbacks->onNativeWindowDestroyed=window_destroyed;a->callbacks->onInputQueueCreated=input_created;a->callbacks->onInputQueueDestroyed=input_destroyed;a->callbacks->onPause=paused;a->callbacks->onResume=resumed;a->callbacks->onDestroy=destroyed;
    ANativeActivity_setWindowFlags(a,AWINDOW_FLAG_FULLSCREEN|AWINDOW_FLAG_KEEP_SCREEN_ON,0);pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,64u<<20);int error=pthread_create(&s->worker,&attr,boot,s);pthread_attr_destroy(&attr);if(error)ANativeActivity_finish(a);
}
