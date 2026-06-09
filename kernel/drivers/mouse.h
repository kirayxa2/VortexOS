#ifndef VOS_MOUSE_H
#define VOS_MOUSE_H

#include "types.h"

typedef struct {
    int32_t x, y;       /* позиция курсора */
    uint8_t left;       /* левая кнопка */
    uint8_t right;      /* правая кнопка */
    uint8_t middle;     /* средняя кнопка */
} mouse_state_t;

void          mouse_init(void);
mouse_state_t mouse_get_state(void);

#endif
