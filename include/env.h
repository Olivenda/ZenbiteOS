#pragma once
#include "types.h"

#define ENV_MAX      32
#define ENV_NAME_MAX 32
#define ENV_VAL_MAX  256

void        env_init(void);
int         env_set(const char *name, const char *val);
const char *env_get(const char *name);
void        env_del(const char *name);
void        env_list(void);
