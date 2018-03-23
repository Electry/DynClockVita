#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/display.h>
#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <taihen.h>

#include "display.h"

#define SECOND 1000000

#define FREQ_LIST_N 9
static int g_freq_list[FREQ_LIST_N][4] = {
    {444, 222, 222, 166},
    {366, 222, 222, 166},
    {333, 222, 222, 166},
    {333, 166, 166, 166},
    {266, 166, 166, 166},
    {222, 166, 166, 166},
    {166, 111, 111, 111},
    {111, 111, 111, 111},
    { 55,  55,  55,  55}
};
static int g_freq_current = 3;

static SceUID g_hook[5];
static tai_hook_ref_t g_hook_ref[5];

static long g_frametime_target = 1;
static int g_fps_target = 1;

static SceUInt64 g_tick_last = 1;
static SceUInt64 g_tick_last_drop = 1;

static long g_drop_frametime_diff = 1000; // 1ms
static long g_drop_cooldown = SECOND * 5;

void freqApply()
{
    scePowerSetArmClockFrequency(g_freq_list[g_freq_current][0]);
    scePowerSetBusClockFrequency(g_freq_list[g_freq_current][1]);
    scePowerSetGpuClockFrequency(g_freq_list[g_freq_current][2]);
    //scePowerSetGpuXbarClockFrequency(g_freq_list[g_freq_current][3]);
}

int sceDisplaySetFrameBuf_patched(const SceDisplayFrameBuf *pParam, int sync)
{
    updateFramebuf(pParam);
    SceUInt64 tick_now = sceKernelGetProcessTimeWide();

    long frametime = tick_now - g_tick_last;
    long frametime_trigger = g_frametime_target + g_drop_frametime_diff;
    int fps = ((SECOND*10) / frametime + 5) / 10; // rounding hack

    // Up
    if (frametime > frametime_trigger && tick_now - g_tick_last_drop > frametime) {
        if (g_freq_current > 0)
            g_freq_current--;
        g_tick_last_drop = tick_now;

        freqApply();
    }
    // Down
    else if (tick_now - g_tick_last_drop > g_drop_cooldown) {
        if (g_freq_current < FREQ_LIST_N - 1)
            g_freq_current++;
        g_tick_last_drop = tick_now;

        freqApply();
    }

    // Calculate target FPS and frametime
    g_fps_target = fps > 35 ? 60 : 30;
    g_frametime_target = SECOND / g_fps_target;

    // Print shit on screen
    drawStringF(5, 5, "%d/%d [%d|%d]",
                fps,
                g_fps_target,
                /*g_freq_list[g_freq_current][0]*/scePowerGetArmClockFrequency(),
                /*g_freq_list[g_freq_current][2]*/scePowerGetGpuClockFrequency());


    g_tick_last = tick_now;

    return TAI_CONTINUE(int, g_hook_ref[0], pParam, sync);
}

int scePowerSetArmClockFrequency_patched(int freq)
{
    return TAI_CONTINUE(int, g_hook_ref[1], g_freq_list[g_freq_current][0]);
}

int scePowerSetBusClockFrequency_patched(int freq)
{
    return TAI_CONTINUE(int, g_hook_ref[2], g_freq_list[g_freq_current][1]);
}

int scePowerSetGpuClockFrequency_patched(int freq)
{
    return TAI_CONTINUE(int, g_hook_ref[3], g_freq_list[g_freq_current][2]);
}
/*
int scePowerSetGpuXbarClockFrequency_patched(int freq)
{
    return TAI_CONTINUE(int, g_hook_ref[4], g_freq_list[g_freq_current][3]);
}
*/

void _start() __attribute__ ((weak, alias ("module_start")));
int module_start(SceSize argc, const void *args)
{
    g_tick_last = sceKernelGetProcessTimeWide();
    g_tick_last_drop = g_tick_last;

    g_hook[0] = taiHookFunctionImport(&g_hook_ref[0],
                                      TAI_MAIN_MODULE,
                                      TAI_ANY_LIBRARY,
                                      0x7A410B64,
                                      sceDisplaySetFrameBuf_patched);

    g_hook[1] = taiHookFunctionImport(&g_hook_ref[1],
                                      TAI_MAIN_MODULE,
                                      TAI_ANY_LIBRARY,
                                      0x74DB5AE5,
                                      scePowerSetArmClockFrequency_patched);

    g_hook[2] = taiHookFunctionImport(&g_hook_ref[2],
                                      TAI_MAIN_MODULE,
                                      TAI_ANY_LIBRARY,
                                      0xB8D7B3FB,
                                      scePowerSetBusClockFrequency_patched);

    g_hook[3] = taiHookFunctionImport(&g_hook_ref[3],
                                      TAI_MAIN_MODULE,
                                      TAI_ANY_LIBRARY,
                                      0x717DB06C,
                                      scePowerSetGpuClockFrequency_patched);

    /*
    g_hook[4] = taiHookFunctionImport(&g_hook_ref[4],
                                      TAI_MAIN_MODULE,
                                      TAI_ANY_LIBRARY,
                                      0xA7739DBE,
                                      scePowerSetGpuXbarClockFrequency_patched);
    */

    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
    if (g_hook[0] >= 0)
        taiHookRelease(g_hook[0], g_hook_ref[0]);
    if (g_hook[1] >= 0)
        taiHookRelease(g_hook[1], g_hook_ref[1]);
    if (g_hook[2] >= 0)
        taiHookRelease(g_hook[2], g_hook_ref[2]);
    if (g_hook[3] >= 0)
        taiHookRelease(g_hook[3], g_hook_ref[3]);
    /*
    if (g_hook[4] >= 0)
        taiHookRelease(g_hook[4], g_hook_ref[4]);
    */

    return SCE_KERNEL_STOP_SUCCESS;
}
