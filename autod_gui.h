// autod_gui.h — optional GUI module for autod
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal read-only config the GUI actually needs */
typedef struct {
    int  port;
    char role[64];
    char device[64];
} autod_gui_config_t;

/* Snapshot format the GUI renders (strings sized for UI) */
typedef struct {
    int count;
    struct {
        char ip[16];
        char role[64];
        char device[64];
        char version[32];
    } nodes[64];                 /* GUI draws up to 64 rows */
    int      scanning;           /* 0/1 */
    unsigned targets;
    unsigned done;
    int      progress_pct;       /* 0..100 */
} autod_gui_snapshot_t;

/* Core provides a callback that fills the snapshot from its state */
typedef void (*autod_gui_snapshot_cb)(autod_gui_snapshot_t* snap, void* user);

/* Start GUI in its own thread (no-op if not built with SDL) */
int  autod_gui_start(const autod_gui_config_t* cfg,
                     autod_gui_snapshot_cb snapshot_cb,
                     void* snapshot_user);

/* Ask the GUI to quit (safe to call even if GUI not running) */
void autod_gui_request_quit(void);

/* Optional: block until GUI thread exits (safe to call even if not running) */
void autod_gui_join(void);

#ifdef __cplusplus
}
#endif
