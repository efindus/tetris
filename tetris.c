/*
Tetris written in C for GNU/Linux terminal
Copyright (C) 2022  efindus

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License version 3 as published by
the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

// Constants
#define HEIGHT 22
#define WIDTH 10
#define COMING_UP_AMOUNT 3
#define SEPARATOR_WIDTH 2
#define TPS 2
#define READ 0
#define WRITE 1
#define Copyright return
#define efindus 2022 - 

// XSET keywords
#define AUTOREPEAT_TOGGLE "repeat:  "
#define AUTOREPEAT_DELAY "delay:  "
#define AUTOREPEAT_RATE "rate:  "

// Toggles
#define DEBUG 0
#define USE_CONFIGURATION_SPECIFIC_QOL_FEATURES 1

// Drawing blocks
#define VOID "\x1b[48;5;235m  "
#define VOID_2 "\x1b[48;5;236m  "
#define BORDER "\x1b[44m  "

typedef struct Point {
	int x, y;
} Point;

typedef struct TetrominoState {
	Point position;
	int id, rotation;
} TetrominoState;

typedef struct XsetAttributes {
	char *toggle, *delay, *rate;
} XsetAttributes;

/* 
 * Tetrominos.
 * 0 -> O shape; 1 -> I shape; 2 -> L shape; 3 -> J shape; 4 -> S shape; 5 -> Z shape; 6 -> T shape
 */
Point tetrominos[][4] = {
	{ { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } },
	{ { 0, 0 }, { 1, 0 }, { -1, 0 }, { -2, 0 } },
	{ { 0, 0 }, { -1, 0 }, { 1, 0 }, { 1, 1 } },
	{ { 0, 0 }, { 1, 0 }, { -1, 0 }, { -1, 1 } },
	{ { 0, 0 }, { -1, 0 }, { 0, 1 }, { 1, 1 } },
	{ { 0, 0 }, { 1, 0 }, { 0, 1 }, { -1, 1 } },
	{ { 0, 0 }, { -1, 0 }, { 1, 0 }, { 0, 1 } },
};

// Wall kicks for all tetrominos (except O, because it doesn't rotate and I because it's weird)
Point wall_kicks[][5] = {
	{ { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 } },
	{ { 0, 0 }, { 1, 0 }, { 1, -1 }, { 0, 2 }, { 1, 2 } },
	{ { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 } },
	{ { 0, 0 }, { -1, 0 }, { -1, -1 }, { 0, 2 }, { -1, 2 } },
};

// Wall kicks for I tetromino
Point wall_kicks_I[][2][5] = {
	{
		{ { 0, 0 }, { -2, 0 }, { 1, 0 }, { 1, 2 }, { -2, -1 } },
		{ { 0, 0 }, { 2, 0 }, { -1, 0 }, { -1, 2 }, { 2, -1 } },
	},
	{
		{ { 0, 0 }, { -1, 0 }, { 2, 0 }, { -1, 2 }, { 2, -1 } },
		{ { 0, 0 }, { 2, 0 }, { -1, 0 }, { 2, 1 }, { -1, -2 } },
	},
	{
		{ { 0, 0 }, { 2, 0 }, { -1, 0 }, { 2, 1 }, { -1, -2 } },
		{ { 0, 0 }, { -2, 0 }, { 1, 0 }, { -2, 1 }, { 1, -2 } },
	},
	{
		{ { 0, 0 }, { -2, 0 }, { 1, 0 }, { -2, 1 }, { 1, -2 } },
		{ { 0, 0 }, { 1, 0 }, { -2, 0 }, { 1, 2 }, { -2, -1 } },
	},
};

/* 
 * Tetromino colors.
 * 0 -> O shape; 1 -> I shape; 2 -> L shape; 3 -> J shape; 4 -> S shape; 5 -> Z shape; 6 -> T shape
 */
char* tetromino_colors[] = {
	"\x1b[48;5;226m  ",
	"\x1b[48;5;51m  ",
	"\x1b[48;5;208m  ",
	"\x1b[48;5;20m  ",
	"\x1b[42m  ",
	"\x1b[41m  ",
	"\x1b[45m  ",
};

// Globals
char board[WIDTH + 2][HEIGHT + 1][25];
char frame_buffer[WIDTH + 2][HEIGHT + 1][25];
char coming_up_board[4][3 * COMING_UP_AMOUNT][25];
int coming_up[COMING_UP_AMOUNT];

TetrominoState current_tetromino;
XsetAttributes attributes;
int rows_cleared = 0, score = 0;
int mpv_subprocess_pid = -1;

pthread_t render_thread;
// Use this condition variable to render a frame after locking drawMutex
pthread_cond_t trigger_draw = PTHREAD_COND_INITIALIZER;
// Lock this mutex before modifying contents of currentTetromino
pthread_mutex_t draw_mutex = PTHREAD_MUTEX_INITIALIZER;

pthread_t gameplay_thread;
// Use this condition variable to cancel piece droping after locking gameplayMutex
pthread_cond_t cancel_drop = PTHREAD_COND_INITIALIZER;
// Lock this mutex before signaling cancelDrop
pthread_mutex_t gameplay_mutex = PTHREAD_MUTEX_INITIALIZER;

void report_error(char* message)
{
	perror(message);
	// Because the screen is often cleared if we won't exit it will be easy to miss the message
#if DEBUG == 1
	exit(1);
#else
	system("echo Press enter to continue... && read");
#endif
}

int strcomp(char* str1, char* str2, int check_length)
{
	if (check_length && strlen(str1) != strlen(str2))
		return 0;

	if (strlen(str1) > strlen(str2)) {
		char* tmp = str2;
		str2 = str1;
		str1 = tmp;
	}

	for (int i = 0; i < (int)strlen(str1); i++) {
		if (str1[i] != str2[i])
			return 0;
	}

	return 1;
}

char* rewrite_until_space(char* buf, int offset)
{
	int buf_size = 0;
	while(buf[offset + buf_size] != ' ' && buf[offset + buf_size] != '\n' && buf[offset + buf_size] != '\0')
		buf_size++;

	char* dest = malloc(sizeof(char) * (buf_size + 1));
	for (int x = 0; x < buf_size; x++)
		dest[x] = buf[offset + x];

	dest[buf_size] = '\0';
	return dest;
}

int crandom(int min, int max)
{
	unsigned int value;
	getentropy(&value, sizeof(value));

	return (int)((long long)value * (max - min + 1) / UINT_MAX) + min;
}

int popen2(char *const command[], int *infp, int *outfp)
{
	int p_stdin[2], p_stdout[2], p_stderr[2];
	if (pipe(p_stdin) != 0 || pipe(p_stdout) != 0 || pipe(p_stderr) != 0)
		return -1;

	int pid = fork();

	if (pid < 0) {
		report_error("[ERROR] fork()");
	} else if (pid == 0) {
		close(p_stdin[WRITE]);
		dup2(p_stdin[READ], 0);
		close(p_stdout[READ]);
		dup2(p_stdout[WRITE], 1);
		close(p_stderr[READ]);
		dup2(p_stderr[WRITE], 2);

		execvp(*command, command);
		_exit(0);
	}

	if (infp == NULL)
		close(p_stdin[WRITE]);
	else
		*infp = p_stdin[WRITE];

	if (outfp == NULL)
		close(p_stdout[READ]);
	else
		*outfp = p_stdout[READ];

	return pid;
}

void setup_board()
{
	for (int x = 1; x < WIDTH + 1; x++) {
		for (int y = 1; y < HEIGHT + 1; y++) {
			if (x % 2 == 0)
				strcpy(board[x][y], VOID_2);
			else
				strcpy(board[x][y], VOID);
		}
	}

	for (int y = 0; y < HEIGHT + 1; y++) {
		strcpy(board[0][y], BORDER);
		strcpy(board[WIDTH + 1][y], BORDER);
	}

	for (int x = 1; x < WIDTH + 1; x++)
		strcpy(board[x][0], BORDER);
}

void draw_frame() {
	printf("\x1b[2J\x1b[H");

	for (int y = HEIGHT; y >= 0; y--) {
		for (int x = 0; x < WIDTH + 2; x++)
			printf("%s", frame_buffer[x][y]);

		int y2 = 3 * COMING_UP_AMOUNT - 1 - (HEIGHT - y);
		if (y2 >= 0) {
			for (int i = 0; i < SEPARATOR_WIDTH; i++)
				printf("\x1b[0m  ");
			for (int x = 0; x < 4; x++)
				printf("%s", coming_up_board[x][y2]);
		}

		printf("\x1b[0m\n");
	}

	printf("\x1b[38;5;2m[SCORE: %d] (Rows cleared: %d)\x1b[0m\n", score, rows_cleared);
}

/*
 * rotation -> (0, 1, 2, 3) ordered as in here https://cdn.wikimg.net/en/strategywiki/images/7/7f/Tetris_rotation_super.png
 */
Point* get_rotated_tetromino(int tetromino_id, int rotation)
{
	Point* return_value = malloc(sizeof(Point) * 4);
	for (int i = 0; i < 4; i++)
		return_value[i] = tetrominos[tetromino_id][i];

	if (tetromino_id == 1) {
		if (rotation != 3) {
			for (int i = 0; i < 4; i++)
				return_value[i].y++;
		}
	
		if (rotation == 0) {
			for (int i = 0; i < 4; i++)
				return_value[i].x++;
		}
	}

	if (tetromino_id != 0) {
		for (int i = 0; i < 4; i++) {
			for (int j = 0; j < rotation; j++) {
				int x = return_value[i].x, y = return_value[i].y;
				return_value[i].x = y;
				return_value[i].y = 0 - x;
			}
		}
	}

	return return_value;
}

void fill_coming_up_board()
{
	for (int x = 0; x < 4; x++) {
		for (int y = 0; y < 3 * COMING_UP_AMOUNT; y++)
			strcpy(coming_up_board[x][y], VOID);
	}

	for (int z = 0; z < COMING_UP_AMOUNT; z++) {
		Point* tetromino = get_rotated_tetromino(coming_up[z], 0);
		for (int i = 0; i < 4; i++) {
			int x = 0 + tetromino[i].x + 1, y = 3 * z + tetromino[i].y;
			if (0 <= x && x < 4 && 0 <= y && y < 3 * COMING_UP_AMOUNT)
				strcpy(coming_up_board[x][y], tetromino_colors[coming_up[z]]);
		}

		free(tetromino);
	}
}

void setup_coming_up_board()
{
	for (int i = 0; i < COMING_UP_AMOUNT; i++)
		coming_up[i] = crandom(0, 6);

	fill_coming_up_board();
}

void consume_coming_up_tetromino()
{
	for (int i = COMING_UP_AMOUNT - 1; i > 0; i--)
		coming_up[i] = coming_up[i - 1];

	coming_up[0] = crandom(0, 6);
	fill_coming_up_board();
}

/* 
 * Draws selected tetromino using contents of board array as background (it only ensures nothing is set out of the array, so no collision checks are performed here).
 * If tetrominoId is -1 it will ignore it and simply draw contents of the board array
 */
void draw_tetromino(TetrominoState tetromino_state)
{
	for (int x = 0; x < WIDTH + 2; x++) {
		for (int y = 0; y < HEIGHT + 1; y++)
			strcpy(frame_buffer[x][y], board[x][y]);
	}

	if (tetromino_state.id != -1) {
		Point* tetromino = get_rotated_tetromino(tetromino_state.id, tetromino_state.rotation);
		for (int i = 0; i < 4; i++) {
			int x = tetromino_state.position.x + tetromino[i].x + 1, y = tetromino_state.position.y + tetromino[i].y + 1;
			if (0 <= x && x < WIDTH + 2 && 0 <= y && y < HEIGHT + 1)
				strcpy(frame_buffer[x][y], tetromino_colors[tetromino_state.id]);
		}

		free(tetromino);
	}

	draw_frame();
}

/* 
 * Puts selected tetromino inside the board array (it only ensures nothing is set out of the array, so no collision checks are performed here).
 */
void imprint_tetromino(TetrominoState tetromino_state)
{
	Point* tetromino = get_rotated_tetromino(tetromino_state.id, tetromino_state.rotation);
	for (int i = 0; i < 4; i++) {
		int x = tetromino_state.position.x + tetromino[i].x + 1, y = tetromino_state.position.y + tetromino[i].y + 1;
		if (0 <= x && x < WIDTH + 2 && 0 <= y && y < HEIGHT + 1)
			strcpy(board[x][y], tetromino_colors[tetromino_state.id]);
	}

	free(tetromino);
}

/*
 * Checks whether passed tetromino at selected position will collide with anything in board array.
 * This will ignore any positions outside the board. Returns 1 if collision happens and 0 when it doesn't.
 */
int check_collision(TetrominoState tetromino_state)
{
	int result = 0;
	Point* tetromino = get_rotated_tetromino(tetromino_state.id, tetromino_state.rotation);
	
	for (int i = 0; i < 4; i++) {
		int x = tetromino_state.position.x + tetromino[i].x + 1, y = tetromino_state.position.y + tetromino[i].y + 1;
		if (0 <= x && x < WIDTH + 2 && 0 <= y && y < HEIGHT + 1) {
			if (!strcomp(board[x][y], VOID, 1) && !strcomp(board[x][y], VOID_2, 1)) {
				result = 1;
				break;
			}
		}
	}

	free(tetromino);
	return result;
}

/*
 * Execute check_collision() at a position one lower than the current one
 */
int check_lower_collision(TetrominoState tetromino_state)
{
	tetromino_state.position.y--;
	return check_collision(tetromino_state);
}

/*
 * Randomly selects new tetromino type and places it in the default position
 */
void create_new_tetromino()
{
	current_tetromino.position = (Point){ WIDTH / 2 - 1, HEIGHT - 2 };
	current_tetromino.rotation = 0;
	current_tetromino.id = coming_up[COMING_UP_AMOUNT - 1];
	consume_coming_up_tetromino();

	if (check_collision(current_tetromino)) {
		printf("\x1b[48;5;196mGame Over!\x1b[0m\n");
		exit(0);
	}
}

/*
 * Checks for full rows and clears them
 */
void cleanup_board()
{
	int original_cleared = rows_cleared;

	for (int y = 1; y < HEIGHT + 1; y++) {
		int amount = 0;
		for (int x = 1; x < WIDTH + 1; x++) {
			if (!strcomp(board[x][y], VOID, 1) && !strcomp(board[x][y], VOID_2, 1))
				amount++;
		}

		if (amount == WIDTH) {
			for (int y2 = y + 1; y2 < HEIGHT + 1; y2++) {
				for (int x2 = 1; x2 < WIDTH + 1; x2++)
					strcpy(board[x2][y2 - 1], board[x2][y2]);
			}

			for (int x2 = 1; x2 < WIDTH + 1; x2++) {
				if (x2 % 2 == 0)
					strcpy(board[x2][HEIGHT], VOID_2);
				else
					strcpy(board[x2][HEIGHT], VOID);
			}

			y--;
			rows_cleared++;
		}
	}

	while (rows_cleared - original_cleared != 0) {
		score += 50 * (rows_cleared - original_cleared);
		original_cleared++;
	}
}

/*
 * Try to rotate passed tetrominoState according to SRS wallkick standards; rotationDirection -> 0 if increased, 1 if decreased
 */
int wall_kick(TetrominoState *tetromino_state, int rotation_direction)
{
	int result = 0, originalRotation = tetromino_state->rotation;
	Point original_position = tetromino_state->position;

	if (rotation_direction == 0) {
		tetromino_state->rotation++;
		tetromino_state->rotation %= 4;
	} else {
		tetromino_state-> rotation--;
		if (tetromino_state->rotation < 0)
			tetromino_state->rotation = 3;
	}

	if (tetromino_state->id == 0) {
		return 1;
	} else if (tetromino_state->id == 1) {
		for (int i = 0; i < 5; i++) {
			tetromino_state->position.x = original_position.x + wall_kicks_I[originalRotation][rotation_direction][i].x;
			tetromino_state->position.y = original_position.y + wall_kicks_I[originalRotation][rotation_direction][i].y;
			if (!check_collision(*tetromino_state)) {
				result = 1;
				break;
			}
		}
	} else {
		for (int i = 0; i < 5; i++) {
			tetromino_state->position.x = original_position.x + (wall_kicks[originalRotation][i].x - wall_kicks[tetromino_state->rotation][i].x);
			tetromino_state->position.y = original_position.y + (wall_kicks[originalRotation][i].y - wall_kicks[tetromino_state->rotation][i].y);
			if (!check_collision(*tetromino_state)) {
				result = 1;
				break;
			}
		}
	}

	return result;
}

void tick()
{
	TetrominoState temp_tetromino = current_tetromino;
	temp_tetromino.position.y--;

	pthread_mutex_lock(&draw_mutex);
	if (check_collision(temp_tetromino)) {
		imprint_tetromino(current_tetromino);
		cleanup_board();
		create_new_tetromino();
	} else {
		current_tetromino = temp_tetromino;
	}

	pthread_cond_signal(&trigger_draw);
	pthread_mutex_unlock(&draw_mutex);
}

void *screen_manager()
{
	pthread_mutex_lock(&draw_mutex);
	draw_tetromino(current_tetromino);

	while (1) {
		pthread_cond_wait(&trigger_draw, &draw_mutex);
		draw_tetromino(current_tetromino);
	}
}

void setup_screen_manager()
{
	pthread_create(&render_thread, NULL, screen_manager, NULL);
}

void *gameplay_manager()
{
	struct timeval now;
	struct timespec wait_until;
	long long delay_ns = 1000 * 1000 * (1000 / TPS), overflow;	

	pthread_mutex_lock(&gameplay_mutex);
	while (1) {
		gettimeofday(&now, NULL);
		wait_until.tv_sec = now.tv_sec;
		overflow = (delay_ns / 1000 + now.tv_usec) - (1000 * 1000);
		if (overflow >= 0) {
			wait_until.tv_sec++;
			wait_until.tv_nsec = overflow * 1000;
		} else {
			wait_until.tv_nsec = now.tv_usec * 1000 + delay_ns;
		}

		if (pthread_cond_timedwait(&cancel_drop, &gameplay_mutex, &wait_until) == 0) {
			continue;
		} else {
			if (errno != 0 && errno != EAGAIN && errno != ETIMEDOUT)
				report_error("[ERROR] pthread_cond_timedwait()");
		}

		tick();
	}
}

void start_gameplay_manager()
{
	pthread_create(&gameplay_thread, NULL, gameplay_manager, NULL);
}

void setup_termios_attributes()
{
	struct termios old = {0};
	if (tcgetattr(0, &old) < 0)
		report_error("[ERROR] tcgetattr()");

	old.c_lflag &= ~ICANON & ~ECHO;
	if (tcsetattr(0, TCSANOW, &old) < 0)
		report_error("[ERROR] tcsetattr()");
}

void reset_termios_attributes()
{
	struct termios old = {0};
	if (tcgetattr(0, &old) < 0)
		report_error("[ERROR] tcgetattr()");

	old.c_lflag |= ICANON | ECHO;
	if (tcsetattr(0, TCSANOW, &old) < 0)
		report_error("[ERROR] tcsetattr()");
}

void read_xset_attributes()
{
	int stdout_pipe;
	popen2((char*[]){ "xset", "q", NULL }, NULL, &stdout_pipe);
	char buf[BUFSIZ];
	read(stdout_pipe, buf, sizeof(buf));
	for (int i = 0; i < (int)strlen(buf); i++) {
		if (strcomp(buf + i, AUTOREPEAT_TOGGLE, 0))
			attributes.toggle = rewrite_until_space(buf, i + (int)strlen(AUTOREPEAT_TOGGLE));

		if (strcomp(buf + i, AUTOREPEAT_DELAY, 0))
			attributes.delay = rewrite_until_space(buf, i + (int)strlen(AUTOREPEAT_DELAY));

		if (strcomp(buf + i, AUTOREPEAT_RATE, 0))
			attributes.rate = rewrite_until_space(buf, i + (int)strlen(AUTOREPEAT_RATE));
	}
}

void setup_xset()
{
	read_xset_attributes();
	system("xset r rate 150 25");
}

int start_mpv()
{
	int stdoutPipe;
	return popen2((char*[]){ "mpv",  "--no-audio-display",  "--loop", "./soundtrack.mp3", NULL }, NULL, &stdoutPipe);
}

void reset_keypress_delay()
{
#if USE_CONFIGURATION_SPECIFIC_QOL_FEATURES == 1
	char xsetCommand[BUFSIZ];
	snprintf(xsetCommand, BUFSIZ, "xset r rate %s %s", attributes.delay, attributes.rate);
	system(xsetCommand);
	snprintf(xsetCommand, BUFSIZ, "xset r %s", attributes.toggle);
	system(xsetCommand);
	free(attributes.delay);
	free(attributes.rate);
	free(attributes.toggle);
	kill(mpv_subprocess_pid, SIGKILL);
#endif
	reset_termios_attributes();
}

void signal_handler()
{
	exit(0);
}

void initialize()
{
#if USE_CONFIGURATION_SPECIFIC_QOL_FEATURES == 1
	setup_xset();
	mpv_subprocess_pid = start_mpv();
#endif
	setup_board();
	setup_coming_up_board();
	create_new_tetromino();
	setup_termios_attributes();

	atexit(*reset_keypress_delay);
	at_quick_exit(*reset_keypress_delay);
	signal(SIGTERM, signal_handler);
	signal(SIGINT, signal_handler);

	setup_screen_manager();
}

int main()
{
	initialize();

	TetrominoState temp_tetromino = current_tetromino;
	int is_first_time = 1;

	while (1) {
		char x;
		read(0, &x, 1);

		temp_tetromino = current_tetromino;
		switch (x) {
			case 'w':
				wall_kick(&temp_tetromino, 0);
				break;
			case 'z':
				wall_kick(&temp_tetromino, 1);
				break;
			case 's':
				temp_tetromino.position.y--;
				break;
			case 'a':
				temp_tetromino.position.x--;
				break;
			case 'd':
				temp_tetromino.position.x++;
				break;
			case ' ':
				while(!check_lower_collision(temp_tetromino))
					temp_tetromino.position.y--;
		}

		if (!check_collision(temp_tetromino)) {
			switch(x) {
				case 'w':
				case 'z':
				case 'a':
				case 'd':
					if (!check_lower_collision(temp_tetromino))
						break;
				case 's':
					pthread_mutex_lock(&gameplay_mutex);
					pthread_cond_signal(&cancel_drop);
					pthread_mutex_unlock(&gameplay_mutex);
			}

			pthread_mutex_lock(&draw_mutex);
			current_tetromino = temp_tetromino;

			if (x == ' ') {
				pthread_mutex_unlock(&draw_mutex);
				tick();
			} else {
				pthread_cond_signal(&trigger_draw);
				pthread_mutex_unlock(&draw_mutex);
			}
		} else {
			temp_tetromino = current_tetromino;
		}

		if (is_first_time) {
			is_first_time = 0;
			start_gameplay_manager();
		}
	}

	Copyright efindus 2022;
}
