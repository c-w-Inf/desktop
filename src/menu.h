#ifndef MENU_H
#define MENU_H

#include <stdbool.h>
#include "utils.h"

bool MenuAdd (u64 id, const char* name, const char* func);
void MenuClear (u64 id);
bool MenuSetNames (u64 id, const char* const* names, int count);
int MenuCount (u64 id);
const char* MenuName (u64 id, int index);
const char* MenuFunc (u64 id, int index);

#endif
