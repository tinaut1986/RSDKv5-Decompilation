#include "RSDK/Core/RetroEngine.hpp"
#include "main.hpp"

#if RETRO_PLATFORM == RETRO_3DS 
// The heaps, sized from what the process may commit (as ../legend-of-doom-3ds memory.cpp): the application heap gets
// all of it but 5 MB of linear heap (the screens, the GPU presentation, the bottom-screen UI), up to 92 MB: its address
// window is 96 MB (OS_HEAP_AREA), and in the New 3DS's 124 MB mode libctru's own split asked ~105 MB and panicked
// before main (v0.2.2; the dev builds' larger code had kept it just under). What the cap leaves goes to the linear
// heap, up to 32 MB. The Old 3DS / 2DS (80 MB mode) gets the same split as before.
extern "C" {
extern u32 __ctru_heap, __ctru_linear_heap;
extern char *fake_heap_start, *fake_heap_end;
u32 __ctru_heap_size        = 0;
u32 __ctru_linear_heap_size = 0x500000;

void __system_allocateHeaps(void)
{
    const u32 pageMask = 0xFFF, linearMin = 0x500000, linearMax = 32 << 20, step = 4 << 20;
    const u32 heapMax = (OS_HEAP_AREA_END - OS_HEAP_AREA_BEGIN) - (4 << 20);
    Handle limit = 0;
    if (R_FAILED(svcGetResourceLimit(&limit, CUR_PROCESS_HANDLE)))
        svcBreak(USERBREAK_PANIC);
    s64 maxCommit = 0, curCommit = 0;
    ResourceLimitType type = RESLIMIT_COMMIT;
    svcGetResourceLimitLimitValues(&maxCommit, limit, &type, 1);
    svcGetResourceLimitCurrentValues(&curCommit, limit, &type, 1);
    svcCloseHandle(limit);
    const u32 remaining = (u32)(maxCommit - curCommit) & ~pageMask;
    if (remaining <= linearMin + (24 << 20))
        svcBreak(USERBREAK_PANIC);

    u32 heap = remaining - linearMin;
    if (heap > heapMax)
        heap = heapMax;
    heap &= ~pageMask;
    for (;;) { // a smaller heap rather than a panic if the window will not take it
        __ctru_heap = 0;
        if (R_SUCCEEDED(svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0, heap, MEMOP_ALLOC, (MemPerm)(MEMPERM_READ | MEMPERM_WRITE))))
            break;
        if (heap < (24 << 20) + step)
            svcBreak(USERBREAK_PANIC);
        heap -= step;
    }
    __ctru_heap_size = heap;

    u32 linear = remaining - heap;
    if (linear > linearMax)
        linear = linearMax;
    linear &= ~pageMask;
    for (;;) { // contiguous linear memory can be scarcer than the commit count says
        __ctru_linear_heap = 0;
        if (R_SUCCEEDED(svcControlMemory(&__ctru_linear_heap, 0, 0, linear, MEMOP_ALLOC_LINEAR, (MemPerm)(MEMPERM_READ | MEMPERM_WRITE))))
            break;
        if (linear <= linearMin)
            svcBreak(USERBREAK_PANIC);
        linear = linear > linearMin + step ? linear - step : linearMin;
    }
    __ctru_linear_heap_size = linear;

    mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
    fake_heap_start = (char *)__ctru_heap;
    fake_heap_end   = fake_heap_start + __ctru_heap_size;
}
}

bool RSDK::printToConsole;
#endif

#if RETRO_STANDALONE
#define LinkGameLogic RSDK::LinkGameLogic
#else
#define EngineInfo RSDK::EngineInfo
#include <GameMain.h>
#define LinkGameLogic LinkGameLogicDLL
#endif

#if RETRO_PLATFORM == RETRO_WIN && !RETRO_RENDERDEVICE_SDL2

#if RETRO_RENDERDEVICE_DIRECTX9 || RETRO_RENDERDEVICE_DIRECTX11
INT WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR lpCmdLine, INT nShowCmd)
{
    RSDK::RenderDevice::hInstance     = hInstance;
    RSDK::RenderDevice::hPrevInstance = hPrevInstance;
    RSDK::RenderDevice::nShowCmd      = nShowCmd;

    return RSDK_main(1, &lpCmdLine, LinkGameLogic);
}
#else
INT WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR lpCmdLine, INT nShowCmd)
{
    return RSDK_main(1, &lpCmdLine, LinkGameLogic);
}
#endif

#elif RETRO_PLATFORM == RETRO_ANDROID
extern "C" {
void android_main(struct android_app *app);
}

void android_main(struct android_app *ap)
{
    app                                 = ap;
    app->onAppCmd                       = AndroidCommandCallback;
    app->activity->callbacks->onKeyDown = AndroidKeyDownCallback;
    app->activity->callbacks->onKeyUp   = AndroidKeyUpCallback;

    JNISetup *jni = GetJNISetup();
    // we make sure we do it here so init can chill safely before any callbacks occur
    Paddleboat_init(jni->env, jni->thiz);

    SwappyGL_init(jni->env, jni->thiz);
    SwappyGL_setAutoSwapInterval(false);
    SwappyGL_setSwapIntervalNS(SWAPPY_SWAP_60FPS);
    SwappyGL_setMaxAutoSwapIntervalNS(SWAPPY_SWAP_60FPS);

    getFD    = jni->env->GetMethodID(jni->clazz, "getFD", "([BB)I");
    writeLog = jni->env->GetMethodID(jni->clazz, "writeLog", "([BI)V");

    setLoading = jni->env->GetMethodID(jni->clazz, "setLoadingIcon", "([B)V");
    showLoading = jni->env->GetMethodID(jni->clazz, "showLoadingIcon", "()V");
    hideLoading = jni->env->GetMethodID(jni->clazz, "hideLoadingIcon", "()V");

    setPixSize = jni->env->GetMethodID(jni->clazz, "setPixSize", "(II)V");

#if RETRO_USE_MOD_LOADER
    fsExists      = jni->env->GetMethodID(jni->clazz, "fsExists", "([B)Z");
    fsIsDir       = jni->env->GetMethodID(jni->clazz, "fsIsDir", "([B)Z");
    fsDirIter     = jni->env->GetMethodID(jni->clazz, "fsDirIter", "([B)[Ljava/lang/String;");
    fsRecurseIter = jni->env->GetMethodID(jni->clazz, "fsRecurseIter", "([B)Ljava/lang/String;");
#endif

    GameActivity_setWindowFlags(app->activity,
                                AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_TURN_SCREEN_ON | AWINDOW_FLAG_LAYOUT_NO_LIMITS | AWINDOW_FLAG_FULLSCREEN
                                    | AWINDOW_FLAG_SHOW_WHEN_LOCKED,
                                0);

    RSDK_main(0, NULL, (void *)LinkGameLogic);

    Paddleboat_destroy(jni->env);
    SwappyGL_destroy();
}
#else
int32 main(int32 argc, char *argv[]) { return RSDK_main(argc, argv, (void *)LinkGameLogic); }
#endif

int32 RSDK_main(int32 argc, char **argv, void *linkLogicPtr)
{
#ifdef __SWITCH__
    // initNxLink();
#endif

#if RETRO_PLATFORM == RETRO_3DS
  osSetSpeedupEnable(true);

  hidScanInput();
  if (hidKeysDown() & KEY_SELECT)
    RSDK::printToConsole = true;
  else 
    RSDK::printToConsole = false;
#endif

    RSDK::linkGameLogic = (RSDK::LogicLinkHandle)linkLogicPtr;

    RSDK::InitCoreAPI();

    int32 exitCode = RSDK::RunRetroEngine(argc, argv);

    RSDK::ReleaseCoreAPI();

    return exitCode;
}
