
#include "ui_scr_mrg.h"

/* 记录所有的屏幕卡片 */ 
static scr_card_t *scr_mgr_head;
static scr_card_t *scr_mgr_top;

/* 被调用屏幕栈 */
static scr_card_t *scr_stack_root;
static scr_card_t *scr_stack_top;

static const uint32_t default_bg_color = 0xFF0000;
/*********************************************************************************
 *                              STATIC FUNCTION
 *********************************************************************************/
static lv_obj_t *scr_mgr_default_style(scr_card_t *card)
{
    lv_obj_t *obj = lv_obj_create(NULL);
    lv_obj_set_size(obj, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(obj, lv_color_hex(default_bg_color), LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    if (card->life->create)
        card->life->create(obj);
    return obj;
}

static scr_card_t *scr_mgr_find_by_id(int id) // 检查链表中没有没此 id 
{
    scr_card_t *p = scr_mgr_head->next;
    while(p != NULL){
        if(id == p->id)
            return p;
        p = p->next;
    }
    return NULL;
}

static void scr_mgr_active(scr_card_t *card)  // 设置屏幕卡片为活跃状态 
{
    if(card->st == SCR_MGR_STATE_CREATED || card->st == SCR_MGR_STATE_INACTIVE){
        if (card->life->entry)
            card->life->entry();
        card->st = SCR_MGR_STATE_ACTIVE;
    }
}

static void scr_mgr_inactive(scr_card_t *card)
{
    if(card->st == SCR_MGR_STATE_ACTIVE){
        if (card->life->exit)
            card->life->exit();
        card->st = SCR_MGR_STATE_INACTIVE;
    }
}

static void scr_mgr_remove(scr_card_t *card)
{
    if(card->st == SCR_MGR_STATE_ACTIVE){
        if (card->life->exit)
            card->life->exit();
    }
    if(card->st != SCR_MGR_STATE_IDLE){
        if (card->life->destroy)
            card->life->destroy();
    }
    card->st = SCR_MGR_STATE_IDLE;
}

/*********************************************************************************
 *                              GLOBAL FUNCTION
 *********************************************************************************/
void scr_mgr_init(void)
{
    scr_mgr_head = lv_mem_alloc(sizeof(scr_card_t));
    scr_mgr_head->id = -1;
    scr_mgr_head->obj = NULL;
    scr_mgr_head->st = -1;
    scr_mgr_head->life = NULL;
    scr_mgr_head->next = NULL;
    scr_mgr_head->prev = NULL;
    
    scr_mgr_top = scr_mgr_head;

    scr_stack_root = NULL;
    scr_stack_top = NULL;
}

bool scr_mgr_register(int id, scr_lifecycle_t *card_life) // 注册一个屏幕卡片 
{
    if(scr_mgr_find_by_id(id)){
        printf("scr already registered\n");
        return false;
    }

    scr_card_t *new_card = lv_mem_alloc(sizeof(scr_card_t));
    new_card->id = id;
    new_card->obj = NULL;
    new_card->st = SCR_MGR_STATE_IDLE;
    new_card->life = card_life;
    new_card->next = NULL;
    new_card->prev = scr_mgr_top;

    scr_mgr_top->next = new_card;
    scr_mgr_top = new_card;

    return true;
}

bool scr_mgr_switch(int id, bool anim)  // 清空栈，然后切换到指定 id的屏幕卡片上
{
    scr_card_t *tgt_card = scr_mgr_find_by_id(id);
    scr_card_t *stack_scr = NULL;
    lv_obj_t *old_top_obj;

    (void)anim;

    if(tgt_card == NULL) // 没有找到该屏幕
        return false;

    old_top_obj = scr_stack_top ? scr_stack_top->obj : NULL;
    while(scr_stack_top != NULL) { // 清除所有卡片
        scr_card_t *prev = scr_stack_top->prev;
        scr_mgr_remove(scr_stack_top);
        if (scr_stack_top->obj && scr_stack_top->obj != old_top_obj)
            lv_obj_del(scr_stack_top->obj);     // non-active screens can go now
        lv_mem_free(scr_stack_top);
        scr_stack_top = prev;
    }

    stack_scr = lv_mem_alloc(sizeof(scr_card_t));
    stack_scr->id = tgt_card->id;
    stack_scr->obj = scr_mgr_default_style(tgt_card);
    stack_scr->st = SCR_MGR_STATE_CREATED;
    stack_scr->life = tgt_card->life;
    stack_scr->prev = NULL;
    stack_scr->next = NULL;
    scr_stack_root = stack_scr;
    scr_stack_top = stack_scr;

    scr_mgr_active(stack_scr); // 设置屏幕卡片为活跃状态

    lv_scr_load(stack_scr->obj);
    if (old_top_obj)
        lv_obj_del(old_top_obj);   // the previously active screen
    return true;
}

bool scr_mgr_push(int id, bool anim)
{
    scr_card_t *tgt_card = scr_mgr_find_by_id(id);
    scr_card_t *stack_scr = NULL;

    (void)anim;

    if(tgt_card == NULL){ // 没有找到该屏幕
        return false;
    }

    if((scr_stack_top != NULL) && (tgt_card->id == scr_stack_top->id)){ // 当前push屏幕和最顶层屏幕一样
        return false;
    }

    stack_scr = lv_mem_alloc(sizeof(scr_card_t));
    stack_scr->id = tgt_card->id;
    stack_scr->obj = scr_mgr_default_style(tgt_card);
    stack_scr->st = SCR_MGR_STATE_CREATED;
    stack_scr->life = tgt_card->life;
    if(scr_stack_top == NULL){
        stack_scr->prev = NULL;
        stack_scr->next = NULL;
        scr_stack_root = stack_scr;
        scr_stack_top = stack_scr;
    } else{
        scr_mgr_inactive(scr_stack_top);
        scr_stack_top->next = stack_scr;
        stack_scr->prev = scr_stack_top;
        scr_stack_top->next = NULL;
        scr_stack_top = stack_scr;
    }

    scr_mgr_active(stack_scr);

    lv_scr_load(stack_scr->obj);
    return true;
}

bool scr_mgr_pop(bool anim)
{
    scr_card_t *dst_item = NULL;
    lv_obj_t *cur_obj;

    (void)anim;

    if(scr_stack_top == NULL || scr_stack_top == scr_stack_root){
        return false;
    }

    cur_obj = scr_stack_top->obj;
    dst_item = scr_stack_top->prev;
    scr_mgr_remove(scr_stack_top);
    lv_mem_free((void *)scr_stack_top);
    scr_stack_top = dst_item;

    scr_mgr_active(dst_item);

    lv_scr_load(dst_item->obj);
    if (cur_obj) {
        lv_obj_del(cur_obj);
    }
    return true;
}

int scr_mgr_get_curr_scr_id(void)
{
    if (scr_stack_top) {
        return scr_stack_top->id;
    }
    return -1;
}

/* Return the lifecycle table for the currently active screen, or NULL if
 * no screen is on the stack.  Used by the global keypad-dispatch timer
 * to call the current screen's on_key() hook without a switch/if chain. */
scr_lifecycle_t *scr_mgr_get_curr_life(void)
{
    if (scr_stack_top) {
        return scr_stack_top->life;
    }
    return NULL;
}

/* Return the screen object of the currently active screen, or NULL if no
 * screen is on the stack. */
lv_obj_t *scr_mgr_get_curr_obj(void)
{
    return scr_stack_top ? scr_stack_top->obj : NULL;
}
