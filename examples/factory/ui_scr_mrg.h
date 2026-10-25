#ifndef __SCR_MRG_H__
#define __SCR_MRG_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

typedef enum scr_mgr_state {
    SCR_MGR_STATE_IDLE = 0,  /* Not in use */
    SCR_MGR_STATE_CREATED,   /* Created */
    SCR_MGR_STATE_INACTIVE,  /* Not active */
    SCR_MGR_STATE_ACTIVE,    /* Active and at the foreground */
} scr_mgr_state_e;

typedef struct scr_lifecycle {
    void (*create)(lv_obj_t *parent);
    void (*entry)(void);
    void (*exit)(void);
    void (*destroy)(void);
    /* Called by the global 20 ms keypad-dispatch timer (§5.1).
     * The function drains the keypad ring-buffer itself (same while/
     * get/set loop as the old per-screen timer callback it replaces).
     * NULL means this screen has no keyboard handling via the global
     * timer; SCREEN0 is handled separately by menu_keypay_get_event. */
    void (*on_key)(void);
} scr_lifecycle_t;

typedef struct scr_card {
    int id;
    lv_obj_t        *obj;
    scr_mgr_state_e  st;
    scr_lifecycle_t *life;
    struct scr_card *next;
    struct scr_card *prev;
} scr_card_t;

/*********************************************************************************
 *                              GLOBAL PROTOTYPES
 * *******************************************************************************/
void scr_mgr_init(void);
bool scr_mgr_register(int id, scr_lifecycle_t *card_life);
bool scr_mgr_switch(int id, bool anim);
bool scr_mgr_push(int id, bool anim);
bool scr_mgr_pop(bool anim);

int scr_mgr_get_curr_scr_id(void);
scr_lifecycle_t *scr_mgr_get_curr_life(void);
/* Screen object of the current top card, or NULL if the stack is empty. */
lv_obj_t *scr_mgr_get_curr_obj(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*__SCR_MRG_H__*/
