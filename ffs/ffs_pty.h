#pragma once

/*
 * PTY management for A1 MCTP serial bridge.
 *
 * Opens /dev/ptmx and attaches the N_MCTP line discipline to the slave,
 * causing the kernel to create a mctpserial net device backed by the PTY.
 *
 * The slave fd must stay open for the lifetime of the bridge — closing it
 * detaches the line discipline and destroys the net device.
 */

/*
 * Open a PTY pair and attach N_MCTP to the slave.
 * On success: *master_fd and *slave_fd are set; caller must close both
 * when done (slave_fd exists only to keep the line discipline alive).
 * Returns 0 on success, -1 on error.
 */
int ffs_pty_open(int *master_fd, int *slave_fd);
