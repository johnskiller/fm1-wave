/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* The template: the smallest complete fm1-base firmware. Copy this directory to start a firmware.
 * This header is the app's half of the contract with the platform (device/unity.c, host/plat_host.c). */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "FM1-WAVE"
#define APP_SPLASH_RGB RGB(40, 180, 220)
#define APP_BLOCK 256u                   /* frames per audio block: 5.8 ms at 44.1 kHz */
#define APP_TOUCH                        /* velocity and pressure from the keys (lib/touch.h) */

/* The scope's debug corner (app.c dbg_draw): the render's load, the second core, and
 * the core clock. A build for playing does not want it; a build for measuring does.
 * Comment it out for a release -- it is not a runtime toggle, because the measurement
 * itself costs a render's worth of time at boot and the strings cost flash. */
#define DBG_ON
#define APP_BTDEV                        /* Bluetooth MIDI devices (lib/btdev.h) */
#define APP_BLE_AT_BOOT (!template_ble_off())   /* Bluetooth at power-on: as the player left it (REC held) */
int template_ble_off(void);

/* the store: the app's objects, the panel calibration (the platform's) and Bluetooth's address and pairing among
 * them, and their flash offsets */
enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_BLE, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0xDC000u, 0xDE000u}

/* The sample pool: 0x97000..0xDC000 is 276 KiB of the data area, above the app and
 * below the store. Not RAM — the bytes are read in place through the platform's
 * window (plat_res), which is the only way a 250 KiB pool fits an FM-1. The pool is
 * a region of the flash, so its size is a section of the map, not a buffer. */
#define APP_RES_BASE     0x97000u
/* APP_RES_SECTORS is sectors of 4 KiB, not kilobytes: 0x97000..0xDC000 is 276 KiB,
 * which is 69 sectors. (Written as 276 the static_assert in device/res.c catches it,
 * which is exactly what that assert is for.) */
#define APP_RES_SECTORS  69u

/* Backup and restore: the resource region's transfer is the restore's (device/res.c),
 * so this app needs the contract even though it keeps its own settings in the store.
 * The pool is not in it: a backup carries the app's objects, and the pool is a
 * resource the installer writes. */
#define APP_BACKUP_OBJS    {OBJ_SETTINGS}
#define APP_BACKUP_MAGIC   0x57415645u          /* "WAVE" */
#define APP_BACKUP_VERSION 1
#define APP_BACKUP_WHAT    "replaces this instrument's samples"

/* the contract: the platform calls these */
void app_init(void);                     /* boot: the engine, then the saved settings (or defaults) */
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);   /* audio interrupt: n stereo frames, 24-bit */
void app_panic(void);                    /* silence now (before an update rewrites the flash) */
void app_ui_init(void);                  /* the first screen, after the splash */
void app_frame(void);                    /* the UI: input and a redraw, about 60 times a second */
void app_input_only(void);               /* between frames: input only */
void app_say(const char *a, const char *b);   /* a short message on the screen and to a screen reader */

/* the backup contract (plat.h: backup = true). Declared here as well as defined in
 * app.c, because a definition without a visible prototype is an implicit
 * declaration to whatever includes this header first. */
int  app_backup_playing(void);
void app_backup_flush(void);
void app_backup_hold(int on);
void app_backup_restored(void);

/* The host simulator's "expect" checks (host/plat_host.c: expect WHAT VALUE). The
 * panel is now settled enough to assert against, and these exist so the assignment
 * in docs/panel-map.md is checked by the build rather than trusted: "what does
 * EN_K1 do in GRAN" must be answerable by running a script, not by reading app.c.
 *
 * The names are the panel's, so a failure names the knob the player turns. */
#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
