#ifndef UI_MAIN_NK_H
#define UI_MAIN_NK_H

enum nk_nav_t {
    NK_NAV_NONE,
    NK_NAV_NEXT,
    NK_NAV_PREVIOUS,
    NK_NAV_CONFIRM,
    NK_NAV_CANCEL,
};
extern enum nk_nav_t nk_nav_cmd;

#endif
