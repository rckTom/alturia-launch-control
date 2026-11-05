#ifndef UI_H_
#define UI_H_

void ui_init(void);
/* Redraw from ctrl_view(). Call from the LVGL thread after ctrl_tick(). */
void ui_refresh(void);

#endif /* UI_H_ */
