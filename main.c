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

#define FREQ_CPU_MAX_N   8
#define FREQ_BUS_MAX_N   4
#define FREQ_GPU_MAX_N   4

#define COLOR_TEXT_SELECT 0x004444FF
#define COLOR_TEXT        0x00FFFFFF

// Manual mode
static int g_freq_step[FREQ_STEP_N] = {
    /*0*/55,
    /*1*/111,
    /*2*/166,
    /*3*/222,
    /*4*/266,
    /*5*/333,
    /*6*/366,
    /*7*/444
};
// Dynamic mode
static int g_freq_table[FREQ_TABLE_N][3] = {
//   CPU, BUS, GPU
    { 55,  55,  55},
    {111, 111, 111},
    {166, 111, 111},
    {222, 166, 166},
    {266, 166, 166},
    {333, 166, 166},
    {333, 222, 222},
    {366, 222, 222},
    {444, 222, 222}
};
// Default mode
static int g_freq_default[3] = {
//  CPU, BUS, GPU
    266, 166, 166
};

static int g_freq_current_table   = 4;           // g_freq_table index (Dynamic mode)
static int g_freq_current_step[3] = {0,  0,  0}; // g_freq_step index (CPU, BUS, GPU) (Manual mode)

static int g_mode[3] = {0, 0, 0}; // 0 = dynamic, 1 = default, 2 = manual (CPU, BUS, GPU)
static int g_menu    = 0;         // 0 = none, 1 = minimal, 2 = full

static long g_frametime_target   = 33333;
static int g_fps_target          = 30;
static int g_fps_target_stable   = 30;
static long g_fps_target_n       = 0;

static SceUInt32 g_tick_last     = 1; // tick of last frame
static long g_frame_n_since_up   = 0; // num of frames since last freq change
static long g_frame_n_since_down = 0;

static long g_drop_frametime_diff        = SECOND * 0.002f; // 2ms - minimal frametime loss for freq bump up
static long g_frame_n_cooldown_up        = 1;               // wait for n frames before bumping up again
static long g_frame_n_cooldown_down      = 30;              // wait for n framew before bumping down again

static long g_buttons_old = 0;
static int g_selected     = 0;

static SceUID g_hook[8];
static tai_hook_ref_t g_hook_ref[8];

int getFreq(int index)
{
    // Dynamic
    if (g_mode[index] == 0)
        return g_freq_table[g_freq_current_table][index];
    // Default
    else if (g_mode[index] == 1)
        return g_freq_default[index];
    // Manual
    else
        return g_freq_step[g_freq_current_step[index]];
}

void applyFreq()
{
    scePowerSetArmClockFrequency(getFreq(0));
    scePowerSetBusClockFrequency(getFreq(1));
    scePowerSetGpuClockFrequency(getFreq(2));
}

void checkButtons(SceCtrlData *ctrl)
{
    unsigned long pressed = ctrl->buttons & ~g_buttons_old;

    // Toggle menu
    if (ctrl->buttons & SCE_CTRL_SELECT) {
        if (g_menu < 2 && (pressed & SCE_CTRL_UP)) {
            g_menu++;
        }
        else if (g_menu > 0 && (pressed & SCE_CTRL_DOWN)) {
            g_menu--;
        }
    }

    // Full menu open
    if (g_menu == 2) {
        // Move up/down in menu
        if (g_selected > 0 && (pressed & SCE_CTRL_UP))
            g_selected--;
        else if (g_selected < 2 && (pressed & SCE_CTRL_DOWN))
            g_selected++;

        if (pressed & SCE_CTRL_RIGHT) {
            // Dynamic, Default
            if (g_mode[g_selected] < 2) {
                g_mode[g_selected]++;

                // Reset clocks
                if (g_mode[g_selected] == 2)
                    g_freq_current_step[g_selected] = 0;
            // Manual
            } else if (g_mode[g_selected] == 2) {
                // Freq up
                if ((g_freq_current_step[g_selected] < FREQ_CPU_MAX_N - 1 && g_selected == 0) ||
                    (g_freq_current_step[g_selected] < FREQ_BUS_MAX_N - 1 && g_selected == 1) ||
                    (g_freq_current_step[g_selected] < FREQ_GPU_MAX_N - 1 && g_selected == 2))
                    g_freq_current_step[g_selected]++;
            }

            applyFreq();
        }

        if (pressed & SCE_CTRL_LEFT) {
            // Default (1) -> Dynamic (0)
            if (g_mode[g_selected] == 1 && g_selected != 3)
                g_mode[g_selected]--;
            // Manual (2)
            else if (g_mode[g_selected] == 2) {
                // -> Default (1)
                if (g_freq_current_step[g_selected] == 0)
                    g_mode[g_selected]--;
                // Freq down
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
    SceUInt32 tick_now = sceKernelGetProcessTimeLow();

    long frametime = tick_now - g_tick_last;
    long frametime_trigger = g_frametime_target + g_drop_frametime_diff;
    int fps = SECOND / frametime;

    // Dynamic
    if (g_mode[0] == 0 || g_mode[1] == 0 || g_mode[2] == 0) {
        // Bump up
        if (frametime > frametime_trigger && g_frame_n_since_up > g_frame_n_cooldown_up) {
            if (g_freq_current_table < FREQ_TABLE_N - 1)
                g_freq_current_table++;
            g_frame_n_since_up = 0;

            applyFreq();
        }
        // Bump down
        else if (g_frame_n_since_up > g_frame_n_cooldown_down && g_frame_n_since_down > g_frame_n_cooldown_down) {
            if (g_freq_current_table > 0)
                g_freq_current_table--;
            g_frame_n_since_down = 0;

            applyFreq();
        }
    }

    // Calculate target FPS and frametime
    g_fps_target = fps > 35 ? 60 : 30;
    if (g_fps_target == g_fps_target_stable) {
        g_fps_target_n = 0;
    } else {
        g_fps_target_n++;

        if (g_fps_target_n > 10) {
            g_fps_target_stable = g_fps_target;
            g_frametime_target = SECOND / g_fps_target_stable;
        }
    }
    // Print shit on screen
    if (g_menu == 1) {
        drawStringF(0, 0, "%d/%d [%d|%d]",
                    fps,
                    g_fps_target_stable,
                    scePowerGetArmClockFrequency(),
                    scePowerGetGpuClockFrequency());
    } else if (g_menu == 2) {
        char buf[5];

        drawStringF(0, 0, "%d/%d [%d|%d|%d]",
                    fps,
                    g_fps_target_stable,
                    scePowerGetArmClockFrequency(),
                    scePowerGetBusClockFrequency(),
                    scePowerGetGpuClockFrequency());
        drawStringF(0, 20, "               ");

        sprintf(buf, "%d", getFreq(0));
        setTextColor(COLOR_TEXT);
        drawStringF(0, 40, "CPU:  ");
        if (g_selected == 0)
            setTextColor(COLOR_TEXT_SELECT);
        drawStringF(70, 40, "[%s]", (g_mode[0] == 0 ? "Dynamic" : (g_mode[0] == 1 ? "Default" : buf)));

        sprintf(buf, "%d", getFreq(1));
        setTextColor(COLOR_TEXT);
        drawStringF(0, 60, "BUS:  ");
        if (g_selected == 1)
            setTextColor(COLOR_TEXT_SELECT);
        drawStringF(70, 60, "[%s]", (g_mode[1] == 0 ? "Dynamic" : (g_mode[1] == 1 ? "Default" : buf)));

        sprintf(buf, "%d", getFreq(2));
        setTextColor(COLOR_TEXT);
        drawStringF(0, 80, "GPU:  ");
        if (g_selected == 2)
            setTextColor(COLOR_TEXT_SELECT);
        drawStringF(70, 80, "[%s]", (g_mode[2] == 0 ? "Dynamic" : (g_mode[2] == 1 ? "Default" : buf)));

        setTextColor(COLOR_TEXT);
    }

    g_tick_last = sceKernelGetProcessTimeLow();
    g_frame_n_since_up++;
    g_frame_n_since_down++;

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

int sceCtrlPeekBufferPositive_patched(int port, SceCtrlData *ctrl, int count)
{
    int ret = TAI_CONTINUE(int, g_hook_ref[4], port, ctrl, count);
    checkButtons(ctrl);
    return ret;
}
int sceCtrlPeekBufferPositive2_patched(int port, SceCtrlData *ctrl, int count)
{
    int ret = TAI_CONTINUE(int, g_hook_ref[5], port, ctrl, count);
    checkButtons(ctrl);
    return ret;
}
int sceCtrlReadBufferPositive_patched(int port, SceCtrlData *ctrl, int count)
{
    int ret = TAI_CONTINUE(int, g_hook_ref[6], port, ctrl, count);
    checkButtons(ctrl);
    return ret;
}
int sceCtrlReadBufferPositive2_patched(int port, SceCtrlData *ctrl, int count)
{
    int ret = TAI_CONTINUE(int, g_hook_ref[7], port, ctrl, count);
    checkButtons(ctrl);
    return ret;
}

void _start() __attribute__ ((weak, alias ("module_start")));
int module_start(SceSize argc, const void *args)
{
    g_tick_last = sceKernelGetProcessTimeLow();

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
                                      0xA9C3CED6,
                                      sceCtrlPeekBufferPositive_patched);

    g_hook[5] = taiHookFunctionImport(&g_hook_ref[5],
                                      TAI_MAIN_MODULE,
                                      TAI_ANY_LIBRARY,
                                      0x15F81E8C,
                                      sceCtrlPeekBufferPositive2_patched);

    g_hook[6] = taiHookFunctionImport(&g_hook_ref[6],
                                      TAI_MAIN_MODULE,
                                      TAI_ANY_LIBRARY,
                                      0x67E7AB83,
                                      sceCtrlReadBufferPositive_patched);

    g_hook[7] = taiHookFunctionImport(&g_hook_ref[7],
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

    return SCE_KERNEL_STOP_SUCCESS;
}
