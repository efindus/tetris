#include <sys/random.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>
#include <pthread.h>
#include <limits.h>
#include <signal.h>

// TODO: parse xset q output to determine defaults for autorepeat
// TODO: add wall kicks
// TODO: make vertical stripes to make aligning pieces easier
// TODO: add music

// Constants
#define HEIGHT 22
#define WIDTH 10
#define TPS 2

// Toggles
#define DEBUG 1
#define USE_LINUX_ONLY_QOL_FEATURES 1

// Drawing blocks
#define VOID "\x1b[0m  "
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
	{ { 0, 0 }, { -1, 0 }, { 1, 0 }, { 2, 0 } },
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

pthread_t renderThread;
pthread_t gameplayThread;
// Use this condition variable to render a frame after locking drawMutex
pthread_cond_t triggerDraw = PTHREAD_COND_INITIALIZER;
// Lock this mutex before modifying contents of currentTetromino
pthread_mutex_t drawMutex = PTHREAD_MUTEX_INITIALIZER;

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
	getrandom(&value, sizeof(value), GRND_RANDOM);

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
		for (int y = 1; y < HEIGHT + 1; y++)
			strcpy(board[x][y], VOID);
	
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
 * but for I shaped tetromino there are only two possible rotations
 */
Point* getRotatedTetromino(int tetrominoId, int rotation) {
	Point* returnValue = malloc(sizeof(Point) * 4);
	for (int i = 0; i < 4; i++) returnValue[i] = tetrominos[tetrominoId][i];

	if (tetrominoId == 1) {
		rotation %= 2;
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
			if (!strcomp(board[x][y], VOID, 1)) {
				result = 1;
				break;
			}
		}
	}

	free(tetromino);
	return result;
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
			if (!strcomp(board[x][y], VOID, 1)) {
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
				strcpy(board[x2][HEIGHT], VOID);
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
	struct timespec time;
	time.tv_sec = 0;
	time.tv_nsec = 1000 * 1000 * (1000 / TPS);

	while (1) {
		if (nanosleep(&time, NULL) < 0)
			reportError("[ERROR] nanosleep()");

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

void resetKeypressDelay() {
#if USE_LINUX_ONLY_QOL_FEATURES == 1
	system("xset r rate 600 25");
	printf("As this program uses xset to modify input delay here is a quick tooltip how to bring back your favorite setting: xset r rate <delay> <repeats/s> or xset r rate for defaults.\n");
#endif
	resetTermiosAttributes();
}

void signalHandler() {
	exit(1);
}

void initialize() {
#if USE_LINUX_ONLY_QOL_FEATURES == 1
	system("xset r rate 150 25");
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
				while(!checkCollision(tempTetromino)) {
					tempTetromino.position.y--;
				}
				tempTetromino.position.y++;
			}
		}

		if (!checkCollision(tempTetromino)) {
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
