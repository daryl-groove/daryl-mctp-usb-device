#pragma once

#include <signal.h>
#include <stdint.h>

/* FunctionFS MCTP event loop. */

typedef enum {
	FFS_MODE_MCTP,
	FFS_MODE_ECHO,
} ffs_mode_t;

/* Set by signal handler to stop ffs_serve(). */
extern volatile sig_atomic_t g_stop;

/*
 * Open <mount>/ep0, write descriptors + strings, call on_ready (if not NULL),
 * then service the ep0/ep1 poll loop until ep0 closes or g_stop is set.
 * on_ready: return 0 to continue, non-zero to abort.
 * Returns 0 on clean exit, 1 on error.
 */
typedef int (*ffs_on_ready_fn)(void *userdata);

int ffs_serve(const char *mount, ffs_mode_t mode,
              ffs_on_ready_fn on_ready, void *userdata);
