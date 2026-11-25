/* SPDX-License-Identifier: MIT */
#pragma once
#include <string.h>

/* UIKit builds key images in the text field's host as well as InputUI. */
static inline int diting_keyboard_host(const char *name)
{
    return name && (!strcmp(name,"InputUI") || !strcmp(name,"Spotlight") ||
        !strcmp(name,"SpringBoard") || !strcmp(name,"Preferences") || !strcmp(name,"MobilePhone"));
}
