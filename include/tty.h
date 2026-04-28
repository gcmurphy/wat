#ifndef WAT_TTY_H
#define WAT_TTY_H

/*
 * Put stdin into raw, non-echo, non-canonical mode so we can read
 * individual keystrokes without waiting for newline.
 *
 * Returns 0 on success, -1 if stdin is not a TTY or setup failed.
 * On success, the caller MUST call tty_restore() before exit.
 */
int tty_raw(void);

/* Restore the terminal to its pre-raw state. Safe to call twice. */
void tty_restore(void);

/*
 * Non-blocking read of one byte from stdin.
 * Returns the byte (>= 0), -1 if no data available, -2 on error.
 */
int tty_peek(void);

#endif
