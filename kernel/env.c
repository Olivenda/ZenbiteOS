/* Environment variable store. Simple static arrays, case-insensitive names. */
#include "env.h"
#include "string.h"
#include "kio.h"

static char env_names[ENV_MAX][ENV_NAME_MAX];
static char env_vals [ENV_MAX][ENV_VAL_MAX];
static int  env_used [ENV_MAX];

void env_init(void) {
    for (int i = 0; i < ENV_MAX; i++) env_used[i] = 0;
    env_set("PATH",    "C:\\SYSTEM;C:\\BIN");
    env_set("OS",      "Zenbite");
    env_set("VERSION", "0.2");
}

int env_set(const char *name, const char *val) {
    /* Update existing slot */
    for (int i = 0; i < ENV_MAX; i++) {
        if (env_used[i] && strcasecmp(env_names[i], name) == 0) {
            strncpy(env_vals[i], val, ENV_VAL_MAX - 1);
            env_vals[i][ENV_VAL_MAX - 1] = '\0';
            return 0;
        }
    }
    /* Find a free slot */
    for (int i = 0; i < ENV_MAX; i++) {
        if (!env_used[i]) {
            strncpy(env_names[i], name, ENV_NAME_MAX - 1);
            env_names[i][ENV_NAME_MAX - 1] = '\0';
            strncpy(env_vals[i], val, ENV_VAL_MAX - 1);
            env_vals[i][ENV_VAL_MAX - 1] = '\0';
            env_used[i] = 1;
            return 0;
        }
    }
    return -1; /* table full */
}

const char *env_get(const char *name) {
    for (int i = 0; i < ENV_MAX; i++) {
        if (env_used[i] && strcasecmp(env_names[i], name) == 0)
            return env_vals[i];
    }
    return (const char *)0;
}

void env_del(const char *name) {
    for (int i = 0; i < ENV_MAX; i++) {
        if (env_used[i] && strcasecmp(env_names[i], name) == 0) {
            env_used[i] = 0;
            return;
        }
    }
}

void env_list(void) {
    for (int i = 0; i < ENV_MAX; i++) {
        if (env_used[i])
            kprintf("%s=%s\n", env_names[i], env_vals[i]);
    }
}
