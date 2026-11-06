/* SPDX-License-Identifier: GPL-3.0-only */

/*
 * Stacker: a falling-block game on the diagnostics menu, a gimmick that
 * doubles as a test of the D-pad and the panel.
 *
 * Seven shapes of four blocks fall into a well 10 wide and 20 deep. Left
 * and right move the falling one (held, they repeat), up turns it, down
 * drops it faster and OK drops it all the way. A full row disappears;
 * every ten rows the shapes fall faster. The game ends when a new shape
 * has no room.
 *
 * The game reads the buttons itself (inputs_diag()), so the controller
 * only has to keep its own hands off them while the screen is open.
 * Leaving the screen pauses the game, coming back resumes it.
 */

#ifndef GAME_H_
#define GAME_H_

#include <stdbool.h>
#include <stdint.h>

#define GAME_COLS 10
#define GAME_ROWS 20

enum game_phase {
	GAME_PLAYING,
	GAME_OVER,      /* OK starts a new game */
};

struct game_view {
	enum game_phase phase;
	/* Settled blocks and the falling shape, a bit per column, row 0 on top */
	uint16_t well[GAME_ROWS];
	uint16_t falling[GAME_ROWS];
	uint16_t ghost[GAME_ROWS];     /* where the falling shape would land */
	uint16_t next[4];              /* the next shape, in a 4x4 box */
	uint32_t score;
	uint32_t best;                 /* since power on */
	uint32_t lines;
	uint32_t level;
	uint32_t version;              /* changes whenever anything above does */
};

/* Advance the game to @p now (uptime, ms). Call every tick while shown. */
const struct game_view *game_tick(int64_t now);

#endif /* GAME_H_ */
