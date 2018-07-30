#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/display.h>
#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <taihen.h>
#include <stdio.h>

#include "display.h"

#define SECOND              1000000

#define FREQ_STEP_GPU_BUS_N 4
#define FREQ_STEP_CPU_N     2
#define FREQ_TABLE_N        5

#define FRAMETIME_STABLE_FRAMES_N 5

#define COLOR_TEXT_SELECT 0x004444FF
#define COLOR_TEXT        0x00FFFFFF

typedef enum {
	CLOCK_CPU = 0,
	CLOCK_BUS = 1,
	CLOCK_GPU = 2,
	CLOCK_N   = 3
} DC_ClockIndex;

typedef enum {
	MODE_DYNAMIC = 0,
	MODE_DEFAULT = 1,
	MODE_MANUAL  = 2,
	MODE_N       = 3
} DC_Mode;

typedef enum {
	MENU_HIDDEN  = 0,
	MENU_MINIMAL = 1,
	MENU_FULL    = 2,
	MENU_N       = 3
} DC_Menu;

// Manual mode
static int g_freq_step_gpu_bus[FREQ_STEP_GPU_BUS_N] = {
    55,
    111,
    166,
    222
};
static int g_freq_step_cpu[FREQ_STEP_CPU_N] = {
    333,
    444
};

// Dynamic mode
static int g_freq_table[FREQ_TABLE_N][CLOCK_N] = {
//   CPU, BUS, GPU
    {333,  55,  55},
    {333, 111, 111},
    {333, 166, 166},
    {333, 222, 222},
    {444, 222, 222}
};
// Default mode
static int g_freq_default[CLOCK_N] = {
//  CPU, BUS, GPU
    333, 166, 166
};

static int g_freq_current_table         = 2;         // g_freq_table index (Dynamic mode)
static int g_freq_current_step[CLOCK_N] = {0, 0, 0}; // g_freq_step_xxx index (CPU, BUS, GPU) (Manual mode)

static int g_mode[MODE_N] = {MODE_DYNAMIC, MODE_DYNAMIC, MODE_DYNAMIC}; // (CPU, BUS, GPU)
static int g_menu         = MENU_HIDDEN;


static long g_frametime_target    = 33333;
static long g_frametime_stable    = 33333;
static int g_frametime_stable_n   = 0;

static int g_fps_stable           = 30;
static int g_fps_target_stable    = 30;

static SceUInt32 g_tick_last      = 1; // tick of last frame
static SceUInt32 g_tick_real_last = 1; // real tick of last frame (ignore costs of calling sceXXXXX)
static long g_frame_n_since_up    = 0; // num of frames since last freq change
static long g_frame_n_since_down  = 0;

static long g_drop_frametime_diff        = SECOND * 0.002f; // 2ms - minimal frametime loss for freq bump up
static long g_frame_n_cooldown_up        = 1;               // wait for n frames before bumping up again
static long g_frame_n_cooldown_down      = 120;             // wait for n frames before bumping down again

static long g_buttons_old = 0;
static int g_selected     = 0;

static SceUID g_hook[8];
static tai_hook_ref_t g_hook_ref[8];

int getFreq(int index)
{
    // Dynamic
    if (g_mode[index] == MODE_DYNAMIC)
        return g_freq_table[g_freq_current_table][index];
    // Default
    else if (g_mode[index] == MODE_DEFAULT)
        return g_freq_default[index];
    // Manual
    else {
        if (index == CLOCK_CPU)
            return g_freq_step_cpu[g_freq_current_step[index]];
        else
            return g_freq_step_gpu_bus[g_freq_current_step[index]];
    }
}

void applyFreq()
{
    scePowerSetArmClockFrequency(getFreq(CLOCK_CPU));
    scePowerSetBusClockFrequency(getFreq(CLOCK_BUS));
    scePowerSetGpuClockFrequency(getFreq(CLOCK_GPU));
}

void checkButtons(SceCtrlData *ctrl)
{
    unsigned long pressed = ctrl->buttons & ~g_buttons_old;

    // Toggle menu
    if (ctrl->buttons & SCE_CTRL_SELECT) {
        if (g_menu < MENU_FULL && (pressed & SCE_CTRL_UP)) {
            g_menu++;
        }
        else if (g_menu > MENU_HIDDEN && (pressed & SCE_CTRL_DOWN)) {
            g_menu--;
        }
    }

    // Full menu open
    if (g_menu == MENU_FULL) {
        // Move up/down in menu
        if (g_selected > CLOCK_CPU && (pressed & SCE_CTRL_UP))
            g_selected--;
        else if (g_selected < CLOCK_GPU && (pressed & SCE_CTRL_DOWN))
            g_selected++;

        if (pressed & SCE_CTRL_RIGHT) {
            // Dynamic, Default
            if (g_mode[g_selected] < MODE_MANUAL) {
                g_mode[g_selected]++;

                // Reset clocks
                if (g_mode[g_selected] == MODE_MANUAL)
                    g_freq_current_step[g_selected] = 0;
            // Manual
            } else if (g_mode[g_selected] == MODE_MANUAL) {
                // Freq up
                if ((g_selected == CLOCK_CPU && g_freq_current_step[g_selected] < FREQ_STEP_CPU_N - 1) ||
                   ((g_selected == CLOCK_BUS || g_selected == CLOCK_GPU) && g_freq_current_step[g_selected] < FREQ_STEP_GPU_BUS_N - 1))
                    g_freq_current_step[g_selected]++;
            }

            applyFreq();
        }

        if (pressed & SCE_CTRL_LEFT) {
            // Default (1) -> Dynamic (0)
            if (g_mode[g_selected] == MODE_DEFAULT)
                g_mode[g_selected]--;
            // Manual (2)
            else if (g_mode[g_selected] == MODE_MANUAL) {
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

    // Calculate target FPS and frametime
    long frametime = tick_now - g_tick_last;
    long real_frametime = tick_now - g_tick_real_last;

    if (g_frametime_stable_n > FRAMETIME_STABLE_FRAMES_N) {
        long frametime_avg = g_frametime_stable / g_frametime_stable_n;
        g_fps_stable = (SECOND + frametime_avg - 1) / frametime_avg;
        g_fps_target_stable = g_fps_stable > 35 ? 60 : 30;
        g_frametime_target = SECOND / g_fps_target_stable;

        g_frametime_stable_n = 0;
        g_frametime_stable = 0;
    } else {
        g_frametime_stable += frametime;
        g_frametime_stable_n++;
    }

    long frametime_trigger = g_frametime_target + g_drop_frametime_diff;

    // Dynamic
    if (g_mode[CLOCK_CPU] == MODE_DYNAMIC ||
            g_mode[CLOCK_BUS] == MODE_DYNAMIC ||
            g_mode[CLOCK_GPU] == MODE_DYNAMIC) {

        // Bump up
        if (real_frametime >= frametime_trigger &&
                g_frame_n_since_up > g_frame_n_cooldown_up) {

            if (g_freq_current_table < FREQ_TABLE_N - 1)
                g_freq_current_table++;

            g_frame_n_since_up = 0;
            applyFreq();
        }
        // Bump down
        else if (real_frametime < frametime_trigger &&
                g_frame_n_since_up > g_frame_n_cooldown_down &&
                g_frame_n_since_down > g_frame_n_cooldown_down) {

            if (g_freq_current_table > 0)
                g_freq_current_table--;

            g_frame_n_since_down = 0;
            applyFreq();
        }
    }

    // Print shit on screen
    if (g_menu == 1) {
        drawStringF(0, 0, "%d/%d [%d|%d]",
                    g_fps_stable,
                    g_fps_target_stable,
                    scePowerGetArmClockFrequency(),
                    scePowerGetGpuClockFrequency());
    } else if (g_menu == 2) {
        char buf[5];

        drawStringF(0, 0, "%d/%d [%d|%d|%d]",
                    g_fps_stable,
                    g_fps_target_stable,
                    scePowerGetArmClockFrequency(),
                    scePowerGetBusClockFrequency(),
                    scePowerGetGpuClockFrequency());
        drawStringF(0, 20, "               ");

        sprintf(buf, "%d", getFreq(CLOCK_CPU));
        setTextColor(COLOR_TEXT);
        drawStringF(0, 40, "CPU:  ");
        if (g_selected == CLOCK_CPU)
            setTextColor(COLOR_TEXT_SELECT);
        drawStringF(70, 40, "[%s]", (g_mode[CLOCK_CPU] == MODE_DYNAMIC ? "Dynamic" : (g_mode[CLOCK_CPU] == MODE_DEFAULT ? "Default" : buf)));

        sprintf(buf, "%d", getFreq(CLOCK_BUS));
        setTextColor(COLOR_TEXT);
        drawStringF(0, 60, "BUS:  ");
        if (g_selected == CLOCK_BUS)
            setTextColor(COLOR_TEXT_SELECT);
        drawStringF(70, 60, "[%s]", (g_mode[CLOCK_BUS] == MODE_DYNAMIC ? "Dynamic" : (g_mode[CLOCK_BUS] == MODE_DEFAULT ? "Default" : buf)));

        sprintf(buf, "%d", getFreq(CLOCK_GPU));
        setTextColor(COLOR_TEXT);
        drawStringF(0, 80, "GPU:  ");
        if (g_selected == CLOCK_GPU)
            setTextColor(COLOR_TEXT_SELECT);
        drawStringF(70, 80, "[%s]", (g_mode[CLOCK_GPU] == MODE_DYNAMIC ? "Dynamic" : (g_mode[CLOCK_GPU] == MODE_DEFAULT ? "Default" : buf)));

        setTextColor(COLOR_TEXT);
    }

    g_tick_last = tick_now;
    g_tick_real_last = sceKernelGetProcessTimeLow();
    g_frame_n_since_up++;
    g_frame_n_since_down++;

    return TAI_CONTINUE(int, g_hook_ref[0], pParam, sync);
}

int scePowerSetArmClockFrequency_patched(int freq)
{
    return TAI_CONTINUE(int, g_hook_ref[1], getFreq(CLOCK_CPU));
}
int scePowerSetBusClockFrequency_patched(int freq)
{
    return TAI_CONTINUE(int, g_hook_ref[2], getFreq(CLOCK_BUS));
}
int scePowerSetGpuClockFrequency_patched(int freq)
{
    return TAI_CONTINUE(int, g_hook_ref[3], getFreq(CLOCK_GPU));
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

    g_mode[CLOCK_CPU] = MODE_DEFAULT;
    g_mode[CLOCK_BUS] = MODE_DEFAULT;
    g_mode[CLOCK_GPU] = MODE_DEFAULT;
    applyFreq();

    return SCE_KERNEL_STOP_SUCCESS;
}
