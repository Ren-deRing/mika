#pragma once

#define KSTACK_ORDER 2

void* kstack_alloc(void);
void kstack_free(void* stack);
