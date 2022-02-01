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

// TODO: parse xset q output to determine defaults for autorepeat
// TODO: add wall kicks
// TODO: show next 3 pieces on the right of the board

// Constants
#define HEIGHT 22
#define WIDTH 10
#define TPS 2

// Toggles
#define DEBUG 1
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

/* 
 * Tetromino colors.
 * 0 -> O shape; 1 -> I shape; 2 -> L shape; 3 -> J shape; 4 -> S shape; 5 -> Z shape; 6 -> T shape
 */
char* tetrominoColors[] = {
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
char frameBuffer[WIDTH + 2][HEIGHT + 1][25];

TetrominoState currentTetromino;
int rowsCleared = 0;

int mpvSubprocessPID = -1;

pthread_t renderThread;
// Use this condition variable to render a frame after locking drawMutex
pthread_cond_t triggerDraw = PTHREAD_COND_INITIALIZER;
// Lock this mutex before modifying contents of currentTetromino
pthread_mutex_t drawMutex = PTHREAD_MUTEX_INITIALIZER;

pthread_t gameplayThread;
// Use this condition variable to cancel piece droping after locking drawMutex
pthread_cond_t cancelDrop = PTHREAD_COND_INITIALIZER;
// Lock this mutex before signaling cancelDrop
pthread_mutex_t gameplayMutex = PTHREAD_MUTEX_INITIALIZER;

int strcomp(char* str1, char* str2, int checkLength) {
	if (checkLength && strlen(str1) != strlen(str2)) return 0;

	if (strlen(str1) > strlen(str2)) {
		char* tmp = str2;
		str2 = str1;
		str1 = tmp;
	}

	for (int i = 0; i < strlen(str1); i++) {
		if (str1[i] != str2[i]) return 0;
	}

	return 1;
}

int crandom(int min, int max) {
	unsigned int value;
	getentropy(&value, sizeof(value));

	return (int)((long long)value * (max - min + 1) / UINT_MAX) + min;
}

void reportError(char* message) {
#if DEBUG == 1
	perror(message);
	// Because the screen is often cleared if we won't exit it will be easy to miss the message
	exit(1);
#endif
}

void setupBoard() {
	for (int x = 1; x < WIDTH + 1; x++)
		for (int y = 1; y < HEIGHT + 1; y++) {
			if (x % 2 == 0) strcpy(board[x][y], VOID_2);
			else strcpy(board[x][y], VOID);
		}
			
	
	for (int y = 0; y < HEIGHT + 1; y++) {
		strcpy(board[0][y], BORDER);
		strcpy(board[WIDTH + 1][y], BORDER);
	}

	for (int x = 1; x < WIDTH + 1; x++)
		strcpy(board[x][0], BORDER);
}

void drawFrame() {
	printf("\x1b[2J\x1b[H");

	for (int y = HEIGHT; y >= 0; y--) {
		for (int x = 0; x < WIDTH + 2; x++)
			printf("%s", frameBuffer[x][y]);

		printf("\x1b[0m\n");
	}

	printf("\x1b[38;5;2m[SCORE] Rows cleared: %d\x1b[0m\n", rowsCleared);
}

/*
 * rotation -> (0, 1, 2, 3) ordered as in here https://cdn.wikimg.net/en/strategywiki/images/7/7f/Tetris_rotation_super.png
 */
Point* getRotatedTetromino(int tetrominoId, int rotation) {
	Point* returnValue = malloc(sizeof(Point) * 4);
	for (int i = 0; i < 4; i++) returnValue[i] = tetrominos[tetrominoId][i];

	if (tetrominoId == 1) {
		if (rotation != 3) for (int i = 0; i < 4; i++) returnValue[i].y++;
		if (rotation == 0) for (int i = 0; i < 4; i++) returnValue[i].x++;
	}

	if (tetrominoId != 0) {
		for (int i = 0; i < 4; i++) {
			for (int j = 0; j < rotation; j++) {
				int x = returnValue[i].x, y = returnValue[i].y;
				returnValue[i].x = y;
				returnValue[i].y = 0 - x;
			}
		}
	}

	return returnValue;
}

/* 
 * Draws selected tetromino using contents of board array as background (it only ensures nothing is set out of the array, so no collision checks are performed here).
 * If tetrominoId is -1 it will ignore it and simply draw contents of the board array
 */
void drawTetromino(TetrominoState tetrominoState) {
	for (int x = 0; x < WIDTH + 2; x++)
		for (int y = 0; y < HEIGHT + 1; y++)
			strcpy(frameBuffer[x][y], board[x][y]);

	if (tetrominoState.id != -1) {
		Point* tetromino = getRotatedTetromino(tetrominoState.id, tetrominoState.rotation);
		for (int i = 0; i < 4; i++) {
			int x = tetrominoState.position.x + tetromino[i].x + 1, y = tetrominoState.position.y + tetromino[i].y + 1;
			if (0 <= x && x < WIDTH + 2 && 0 <= y && y < HEIGHT + 1) strcpy(frameBuffer[x][y], tetrominoColors[tetrominoState.id]);
		}
		free(tetromino);
	}

	drawFrame();
}

/* 
 * Puts selected tetromino inside the board array (it only ensures nothing is set out of the array, so no collision checks are performed here).
 */
void imprintTetromino(TetrominoState tetrominoState) {
	Point* tetromino = getRotatedTetromino(tetrominoState.id, tetrominoState.rotation);
	for (int i = 0; i < 4; i++) {
		int x = tetrominoState.position.x + tetromino[i].x + 1, y = tetrominoState.position.y + tetromino[i].y + 1;
		if (0 <= x && x < WIDTH + 2 && 0 <= y && y < HEIGHT + 1) strcpy(board[x][y], tetrominoColors[tetrominoState.id]);
	}
	free(tetromino);
}

/*
 * Checks whether passed tetromino at selected position will collide with anything in board array.
 * This will ignore any positions outside the board. Returns 1 if collision happens and 0 when it doesn't.
 */
int checkCollision(TetrominoState tetrominoState) {
	int result = 0;
	Point* tetromino = getRotatedTetromino(tetrominoState.id, tetrominoState.rotation);
	
	for (int i = 0; i < 4; i++) {
		int x = tetrominoState.position.x + tetromino[i].x + 1, y = tetrominoState.position.y + tetromino[i].y + 1;
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
 * Execute checkCollision() at a position one lower than the current one
 */
int checkLowerCollision(TetrominoState tetrominoState) {
	tetrominoState.position.y--;

	return checkCollision(tetrominoState);
}

/*
 * Randomly selects new tetromino type and places it in the default position
 */
void createNewTetromino() {
	currentTetromino.position = (Point){ WIDTH / 2 - 1, HEIGHT - 2 };
	currentTetromino.id = crandom(0, 6);
	currentTetromino.rotation = 0;

	if (checkCollision(currentTetromino)) {
		printf("\x1b[38;5;196mGame Over!\x1b[0m\n");
		exit(0);
	}
}

/*
 * Checks for full rows and clears them
 */
void cleanupBoard() {
	for (int y = 1; y < HEIGHT + 1; y++) {
		int amount = 0;
		for (int x = 1; x < WIDTH + 1; x++) {
			if (!strcomp(board[x][y], VOID, 1) && !strcomp(board[x][y], VOID_2, 1)) {
				amount++;
			}
		}

		if (amount == WIDTH) {
			for (int y2 = y + 1; y2 < HEIGHT + 1; y2++) {
				for (int x2 = 1; x2 < WIDTH + 1; x2++) {
					strcpy(board[x2][y2 - 1], board[x2][y2]);
				}
			}

			for (int x2 = 1; x2 < WIDTH + 1; x2++) {
				if (x2 % 2 == 0) strcpy(board[x2][HEIGHT], VOID_2);
				else strcpy(board[x2][HEIGHT], VOID);
			}

			y--;
			rowsCleared++;
		}
	}
}

void tick() {
	TetrominoState tempTetromino = currentTetromino;
	tempTetromino.position.y--;

	pthread_mutex_lock(&drawMutex);
	if (checkCollision(tempTetromino)) {
		imprintTetromino(currentTetromino);
		cleanupBoard();
		createNewTetromino();
	} else {
		currentTetromino = tempTetromino;
	}

	pthread_cond_signal(&triggerDraw);
	pthread_mutex_unlock(&drawMutex);
}

void *screenManager() {
	pthread_mutex_lock(&drawMutex);
	drawTetromino(currentTetromino);

	while (1) {
		pthread_cond_wait(&triggerDraw, &drawMutex);
		drawTetromino(currentTetromino);
	}
}

void setupScreenManager() {
	pthread_create(&renderThread, NULL, screenManager, NULL);
}

void *gameplayManager() {
	struct timeval now;
	struct timespec waitUntil;
	long long delayNs = 1000 * 1000 * (1000 / TPS), overflow;	

	pthread_mutex_lock(&gameplayMutex);
	while (1) {
		gettimeofday(&now, NULL);
		waitUntil.tv_sec = now.tv_sec;
		overflow = (delayNs / 1000 + now.tv_usec) - (1000 * 1000);
		if (overflow >= 0) {
			waitUntil.tv_sec++;
			waitUntil.tv_nsec = overflow * 1000;
		} else {
			waitUntil.tv_nsec = now.tv_usec * 1000 + delayNs;
		}

		if (pthread_cond_timedwait(&cancelDrop, &gameplayMutex, &waitUntil) == 0) {
			continue;
		} else {
			if (errno != 0 && errno != EAGAIN && errno != ETIMEDOUT) reportError("[ERROR] pthread_cond_timedwait()");
		}

		tick();
	}
}

void startGameplayManager() {
	pthread_create(&gameplayThread, NULL, gameplayManager, NULL);
}

void setupTermiosAttributes() {
	struct termios old = {0};
	if (tcgetattr(0, &old) < 0)
		reportError("[ERROR] tcgetattr()");

	old.c_lflag &= ~ICANON & ~ECHO;
	if (tcsetattr(0, TCSANOW, &old) < 0)
		reportError("[ERROR] tcsetattr()");
}

void resetTermiosAttributes() {
	struct termios old = {0};
	if (tcgetattr(0, &old) < 0)
		reportError("[ERROR] tcgetattr()");

	old.c_lflag |= ICANON | ECHO;
	if (tcsetattr(0, TCSANOW, &old) < 0)
		reportError("[ERROR] tcsetattr()");
}

void setupXset() {
	system("xset r rate 150 25");
}

void resetKeypressDelay() {
#if USE_CONFIGURATION_SPECIFIC_QOL_FEATURES == 1
	system("xset r rate 600 25");
	printf("As this program uses xset to modify input delay here is a quick tooltip how to bring back your favorite setting: xset r rate <delay> <repeats/s> or xset r rate for defaults.\n");
	kill(mpvSubprocessPID, SIGKILL);
#endif
	resetTermiosAttributes();
}

void signalHandler() {
	exit(0);
}

int startmpv() {
	int p_stdin[2], p_stdout[2], p_stderr[2];
	if (pipe(p_stdin) != 0 || pipe(p_stdout) != 0 || pipe(p_stderr) != 0)
		return -1;

	int pid = fork();

	if (pid < 0) {
		reportError("[ERROR] fork()");
	} else if (pid == 0) {
		close(p_stdin[1]);
		dup2(p_stdin[0], 0);
		close(p_stdout[0]);
		dup2(p_stdout[1], 1);
		close(p_stderr[0]);
		dup2(p_stderr[1], 2);
		execvp("mpv", (char*[]){ "mpv",  "--no-audio-display",  "--loop", "./soundtrack.mp3", NULL });
		exit(1);
	}

	return pid;
}

void initialize() {
#if USE_CONFIGURATION_SPECIFIC_QOL_FEATURES == 1
	setupXset();
	mpvSubprocessPID = startmpv();
#endif
	setupBoard();
	createNewTetromino();
	setupTermiosAttributes();

	atexit(*resetKeypressDelay);
	at_quick_exit(*resetKeypressDelay);
	signal(SIGTERM, signalHandler);
	signal(SIGINT, signalHandler);

	setupScreenManager();
}

int main() {
	initialize();

	TetrominoState tempTetromino = currentTetromino;
	int isFirstTime = 1;

	while (1) {
		char x;
		read(0, &x, 1);

		tempTetromino = currentTetromino;
		switch (x) {
			case 'w': {
				tempTetromino.rotation++;
				tempTetromino.rotation %= 4;
				break;
			}
			case 'z': {
				tempTetromino.rotation--;
				if (tempTetromino.rotation < 0) tempTetromino.rotation = 3;
				break;
			}
			case 's': {
				tempTetromino.position.y--;
				break;
			}
			case 'a': {
				tempTetromino.position.x--;
				break;
			}
			case 'd': {
				tempTetromino.position.x++;
				break;
			}
			case ' ': {
				while(!checkLowerCollision(tempTetromino)) {
					tempTetromino.position.y--;
				}
			}
		}

		if (!checkCollision(tempTetromino)) {
			switch(x) {
				case 'w':
				case 'z':
				case 'a':
				case 'd': {
					if (checkLowerCollision(tempTetromino)) {
						pthread_mutex_lock(&gameplayMutex);
						pthread_cond_signal(&cancelDrop);
						pthread_mutex_unlock(&gameplayMutex);
					}
					break;
				}
				case 's': {
					pthread_mutex_lock(&gameplayMutex);
					pthread_cond_signal(&cancelDrop);
					pthread_mutex_unlock(&gameplayMutex);
				}
			}
			pthread_mutex_lock(&drawMutex);

			currentTetromino = tempTetromino;

			if (x == ' ') {
				pthread_mutex_unlock(&drawMutex);
				tick();
			} else {
				pthread_cond_signal(&triggerDraw);
				pthread_mutex_unlock(&drawMutex);
			}
		} else {
			tempTetromino = currentTetromino;
		}

		if (isFirstTime) {
			isFirstTime = 0;
			startGameplayManager();
		}
	}
}
