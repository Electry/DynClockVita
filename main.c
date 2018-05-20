#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/display.h>
#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <taihen.h>
#include <stdio.h>

#include "display.h"

#define SECOND       1000000

#define FREQ_STEP_N  8
#define FREQ_TABLE_N 9

#define FREQ_CPU_N   8
#define FREQ_BUS_N   4
#define FREQ_GPU_N   4
#define FREQ_XBAR_N  3

#define COLOR_TEXT_SELECT 0x004444FF
#define COLOR_TEXT 0x00FFFFFF

static int g_freq_step[FREQ_STEP_N] = {
    /*0*/ 55, /*1*/111, /*2*/166, /*3*/222,
    /*4*/266, /*5*/333, /*6*/366, /*7*/444
};
static int g_freq_table[FREQ_TABLE_N][3] = {
    {444, 222, 222},
    {366, 222, 222},
    {333, 222, 222},
    {333, 166, 166},
    {266, 166, 166},
    {222, 166, 166},
    {166, 111, 111},
    {111, 111, 111},
    { 55,  55,  55}
};
static int g_freq_current_table   = 4;
static int g_freq_current_step[4] = { 0,  0,  0,  0};
static int g_freq_game[4]         = {266, 166, 166, 111};

static int g_mode[4] = {0, 0, 0, 1}; // 0 = dynamic, 1 = game, 2 = manual
static int g_menu    = 0; // 0 = none, 1 = minimal, 2 = full

static SceUID g_hook[9];
static tai_hook_ref_t g_hook_ref[9];

static long g_frametime_target = 1;
static int g_fps_target = 1;

static SceUInt64 g_tick_last = 1;
static SceUInt64 g_tick_last_drop = 1;

static long g_drop_frametime_diff = SECOND * 0.002f; // 2ms
static long g_drop_cooldown = SECOND * 3;

static long g_buttons_old = 0;
static int g_selected = 0;

int getFreq(int index)
{
    // Dynamic
    if (index != 3 && g_mode[index] == 0)
        return g_freq_table[g_freq_current_table][index];
    // Game
    else if ((index == 3 && g_mode[index] == 0) || g_mode[index] == 1)
        return g_freq_game[index];
    // Manual
    else
        return g_freq_step[g_freq_current_step[index]];
}

void applyFreq()
{
    scePowerSetArmClockFrequency(getFreq(0));
    scePowerSetBusClockFrequency(getFreq(1));
    scePowerSetGpuClockFrequency(getFreq(2));

    // Not dynamic
    if (g_mode[3] != 0)
        scePowerSetGpuXbarClockFrequency(getFreq(3));
}

void checkButtons(SceCtrlData *ctrl)
{
    unsigned long pressed = ctrl->buttons & ~g_buttons_old;

    if (ctrl->buttons & SCE_CTRL_SELECT) {
        if (g_menu < 2 && (pressed & SCE_CTRL_UP)) {
            g_menu++;
        }
        else if (g_menu > 0 && (pressed & SCE_CTRL_DOWN)) {
            g_menu--;
        }
    }

    if (g_menu == 2) {
        if (g_selected > 0 && (pressed & SCE_CTRL_UP))
            g_selected--;
        else if (g_selected < 3 && (pressed & SCE_CTRL_DOWN))
            g_selected++;

        if (pressed & SCE_CTRL_RIGHT) {
            // Auto, Game
            if (g_mode[g_selected] < 2) {
                g_mode[g_selected]++;

                // Reset clocks
                if (g_mode[g_selected] == 2)
                    g_freq_current_step[g_selected] = 0;
            // Manual
            } else if (g_mode[g_selected] == 2) {
                if ((g_freq_current_step[g_selected] < FREQ_CPU_N - 1 && g_selected == 0) ||
                    (g_freq_current_step[g_selected] < FREQ_BUS_N - 1 && g_selected == 1) ||
                    (g_freq_current_step[g_selected] < FREQ_GPU_N - 1 && g_selected == 2) ||
                    (g_freq_current_step[g_selected] < FREQ_XBAR_N - 1 && g_selected == 3))
                    g_freq_current_step[g_selected]++;
            }

            applyFreq();
        }

        if (pressed & SCE_CTRL_LEFT) {
            // Game -> Auto
            if (g_mode[g_selected] == 1 && g_selected != 3)
                g_mode[g_selected]--;
            // Manual
            else if (g_mode[g_selected] == 2) {
                // -> Game
                if (g_freq_current_step[g_selected] == 0)
                    g_mode[g_selected]--;
                // Down
                else if (g_freq_current_step[g_selected] > 0)
                    g_freq_current_step[g_selected]--;
            }

            applyFreq();
        }
    }

    g_buttons_old = ctrl->buttons;
}

int sceDisplaySetFrameBuf_patched(const SceDisplayFrameBuf *pParam, int sync)
{
    updateFramebuf(pParam);
    SceUInt64 tick_now = sceKernelGetProcessTimeWide();

    long frametime = tick_now - g_tick_last;
    long frametime_trigger = g_frametime_target + g_drop_frametime_diff;
    int fps = ((SECOND*10) / frametime + 5) / 10; // rounding hack

    // Dynamic
    if (g_mode[0] == 0 || g_mode[1] == 0 || g_mode[2] == 0) {
        // Up
        if (frametime > frametime_trigger && tick_now - g_tick_last_drop > frametime) {
            if (g_freq_current_table > 0)
                g_freq_current_table--;
            g_tick_last_drop = tick_now;

            applyFreq();
        }
        // Down
        else if (tick_now - g_tick_last_drop > g_drop_cooldown) {
            if (g_freq_current_table < FREQ_TABLE_N - 1)
                g_freq_current_table++;
            g_tick_last_drop = tick_now;

            applyFreq();
        }
    }

    // Calculate target FPS and frametime
    g_fps_target = fps > 35 ? 60 : 30;
    g_frametime_target = SECOND / g_fps_target;

    // Print shit on screen
    if (g_menu == 1) {
        drawStringF(0, 0, "%d/%d [%d|%d]",
                    fps,
                    g_fps_target,
                    scePowerGetArmClockFrequency(),
                    scePowerGetGpuClockFrequency());
    } else if (g_menu == 2) {
        char buf[5];

        drawStringF(0, 0, "%d/%d [%d|%d|%d|%d]",
                    fps,
                    g_fps_target,
                    scePowerGetArmClockFrequency(),
                    scePowerGetBusClockFrequency(),
                    scePowerGetGpuClockFrequency(),
                    scePowerGetGpuXbarClockFrequency());
        drawStringF(0, 20, "            ");

        sprintf(buf, " %d", getFreq(0));
        setTextColor(COLOR_TEXT);
        drawStringF(0, 40, "CPU:  ");
        if (g_selected == 0)
            setTextColor(COLOR_TEXT_SELECT);
        drawStringF(70, 40, "[%s]", (g_mode[0] == 0 ? "Auto" : (g_mode[0] == 1 ? "Game" : buf)));

        sprintf(buf, " %d", getFreq(1));
        setTextColor(COLOR_TEXT);
        drawStringF(0, 60, "BUS:  ");
        if (g_selected == 1)
            setTextColor(COLOR_TEXT_SELECT);
        drawStringF(70, 60, "[%s]", (g_mode[1] == 0 ? "Auto" : (g_mode[1] == 1 ? "Game" : buf)));

        sprintf(buf, " %d", getFreq(2));
        setTextColor(COLOR_TEXT);
        drawStringF(0, 80, "GPU:  ");
        if (g_selected == 2)
            setTextColor(COLOR_TEXT_SELECT);
        drawStringF(70, 80, "[%s]", (g_mode[2] == 0 ? "Auto" : (g_mode[2] == 1 ? "Game" : buf)));

        sprintf(buf, " %d", getFreq(3));
        setTextColor(COLOR_TEXT);
        drawStringF(0, 100, "XBAR: ");
        if (g_selected == 3)
            setTextColor(COLOR_TEXT_SELECT);
        drawStringF(70, 100, "[%s]", (g_mode[3] == 1 ? "Game" : buf));

        setTextColor(COLOR_TEXT);
    }

    g_tick_last = tick_now;

    return TAI_CONTINUE(int, g_hook_ref[0], pParam, sync);
}

int scePowerSetArmClockFrequency_patched(int freq)
{
    return TAI_CONTINUE(int, g_hook_ref[1], getFreq(0));
}
int scePowerSetBusClockFrequency_patched(int freq)
{
    return TAI_CONTINUE(int, g_hook_ref[2], getFreq(1));
}
int scePowerSetGpuClockFrequency_patched(int freq)
{
    return TAI_CONTINUE(int, g_hook_ref[3], getFreq(2));
}
int scePowerSetGpuXbarClockFrequency_patched(int freq)
{
    return TAI_CONTINUE(int, g_hook_ref[4], getFreq(3));
}

int sceCtrlPeekBufferPositive_patched(int port, SceCtrlData *ctrl, int count)
{
    int ret = TAI_CONTINUE(int, g_hook_ref[5], port, ctrl, count);
    checkButtons(ctrl);
    return ret;
}
int sceCtrlPeekBufferPositive2_patched(int port, SceCtrlData *ctrl, int count)
{
    int ret = TAI_CONTINUE(int, g_hook_ref[6], port, ctrl, count);
    checkButtons(ctrl);
    return ret;
}
int sceCtrlReadBufferPositive_patched(int port, SceCtrlData *ctrl, int count)
{
    int ret = TAI_CONTINUE(int, g_hook_ref[7], port, ctrl, count);
    checkButtons(ctrl);
    return ret;
}
int sceCtrlReadBufferPositive2_patched(int port, SceCtrlData *ctrl, int count)
{
    int ret = TAI_CONTINUE(int, g_hook_ref[8], port, ctrl, count);
    checkButtons(ctrl);
    return ret;
}

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

    g_hook[4] = taiHookFunctionImport(&g_hook_ref[4],
                                      TAI_MAIN_MODULE,
                                      TAI_ANY_LIBRARY,
                                      0xA7739DBE,
                                      scePowerSetGpuXbarClockFrequency_patched);

    g_hook[5] = taiHookFunctionImport(&g_hook_ref[5],
                                      TAI_MAIN_MODULE,
                                      TAI_ANY_LIBRARY,
                                      0xA9C3CED6,
                                      sceCtrlPeekBufferPositive_patched);

    g_hook[6] = taiHookFunctionImport(&g_hook_ref[6],
                                      TAI_MAIN_MODULE,
                                      TAI_ANY_LIBRARY,
                                      0x15F81E8C,
                                      sceCtrlPeekBufferPositive2_patched);

    g_hook[7] = taiHookFunctionImport(&g_hook_ref[7],
                                      TAI_MAIN_MODULE,
                                      TAI_ANY_LIBRARY,
                                      0x67E7AB83,
                                      sceCtrlReadBufferPositive_patched);

    g_hook[8] = taiHookFunctionImport(&g_hook_ref[8],
                                      TAI_MAIN_MODULE,
                                      TAI_ANY_LIBRARY,
                                      0xC4226A3E,
                                      sceCtrlReadBufferPositive2_patched);

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
    if (g_hook[4] >= 0)
        taiHookRelease(g_hook[4], g_hook_ref[4]);
    if (g_hook[5] >= 0)
        taiHookRelease(g_hook[5], g_hook_ref[5]);
    if (g_hook[6] >= 0)
        taiHookRelease(g_hook[6], g_hook_ref[6]);
    if (g_hook[7] >= 0)
        taiHookRelease(g_hook[7], g_hook_ref[7]);
    if (g_hook[8] >= 0)
        taiHookRelease(g_hook[8], g_hook_ref[8]);

    return SCE_KERNEL_STOP_SUCCESS;
}
