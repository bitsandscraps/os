#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <stdbool.h>
#include <stddef.h>

void init_frame (void);
bool add_frames (void * address_, size_t cnt);
void delete_frames (void * address_, size_t cnt);

#endif  /* vm/frame.h */
