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
Point wallKicks[][5] = {
	{ { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 } },
	{ { 0, 0 }, { 1, 0 }, { 1, -1 }, { 0, 2 }, { 1, 2 } },
	{ { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 } },
	{ { 0, 0 }, { -1, 0 }, { -1, -1 }, { 0, 2 }, { -1, 2 } },
};

// Wall kicks for I tetromino
Point wallKicksI[][2][5] = {
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
char comingUpBoard[4][3 * COMING_UP_AMOUNT][25];
int comingUp[COMING_UP_AMOUNT];

TetrominoState currentTetromino;
XsetAttributes attributes;
int rowsCleared = 0, score = 0;
int mpvSubprocessPID = -1;

pthread_t renderThread;
// Use this condition variable to render a frame after locking drawMutex
pthread_cond_t triggerDraw = PTHREAD_COND_INITIALIZER;
// Lock this mutex before modifying contents of currentTetromino
pthread_mutex_t drawMutex = PTHREAD_MUTEX_INITIALIZER;

pthread_t gameplayThread;
// Use this condition variable to cancel piece droping after locking gameplayMutex
pthread_cond_t cancelDrop = PTHREAD_COND_INITIALIZER;
// Lock this mutex before signaling cancelDrop
pthread_mutex_t gameplayMutex = PTHREAD_MUTEX_INITIALIZER;

void reportError(char* message) {
	perror(message);
	// Because the screen is often cleared if we won't exit it will be easy to miss the message
#if DEBUG == 1
	exit(1);
#else
	system("echo Press enter to continue... && read");
#endif
}

int strcomp(char* str1, char* str2, int checkLength) {
	if (checkLength && strlen(str1) != strlen(str2)) return 0;

	if (strlen(str1) > strlen(str2)) {
		char* tmp = str2;
		str2 = str1;
		str1 = tmp;
	}

	for (int i = 0; i < (int)strlen(str1); i++) {
		if (str1[i] != str2[i]) return 0;
	}

	return 1;
}

char* rewriteUntilSpace(char* buf, int offset) {
	int bufSize = 0;
	while(buf[offset + bufSize] != ' ' && buf[offset + bufSize] != '\n' && buf[offset + bufSize] != '\0') {
		bufSize++;
	}
	char* dest = malloc(sizeof(char) * (bufSize + 1));
	for (int x = 0; x < bufSize; x++) {
		dest[x] = buf[offset + x];
	}
	dest[bufSize] = '\0';
	return dest;
}

int crandom(int min, int max) {
	unsigned int value;
	getentropy(&value, sizeof(value));

	return (int)((long long)value * (max - min + 1) / UINT_MAX) + min;
}

int popen2(char *const command[], int *infp, int *outfp) {
	int p_stdin[2], p_stdout[2], p_stderr[2];
	if (pipe(p_stdin) != 0 || pipe(p_stdout) != 0 || pipe(p_stderr) != 0)
		return -1;

	int pid = fork();

	if (pid < 0) {
		reportError("[ERROR] fork()");
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

		int y2 = 3 * COMING_UP_AMOUNT - 1 - (HEIGHT - y);
		if (y2 >= 0) {
			for (int i = 0; i < SEPARATOR_WIDTH; i++) printf("\x1b[0m  ");
			for (int x = 0; x < 4; x++) printf("%s", comingUpBoard[x][y2]);
		}

		printf("\x1b[0m\n");
	}

	printf("\x1b[38;5;2m[SCORE: %d] (Rows cleared: %d)\x1b[0m\n", score, rowsCleared);
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

void fillComingUpBoard() {
	for (int x = 0; x < 4; x++)
		for (int y = 0; y < 3 * COMING_UP_AMOUNT; y++) {
			strcpy(comingUpBoard[x][y], VOID);
		}

	for (int z = 0; z < COMING_UP_AMOUNT; z++) {
		Point* tetromino = getRotatedTetromino(comingUp[z], 0);
		for (int i = 0; i < 4; i++) {
			int x = 0 + tetromino[i].x + 1, y = 3 * z + tetromino[i].y;
			if (0 <= x && x < 4 && 0 <= y && y < 3 * COMING_UP_AMOUNT) strcpy(comingUpBoard[x][y], tetrominoColors[comingUp[z]]);
		}
		free(tetromino);
	}
}

void setupComingUpBoard() {
	for (int i = 0; i < COMING_UP_AMOUNT; i++) comingUp[i] = crandom(0, 6);
	fillComingUpBoard();
}

void consumeComingUpTetromino() {
	for (int i = COMING_UP_AMOUNT - 1; i > 0; i--) comingUp[i] = comingUp[i - 1];
	comingUp[0] = crandom(0, 6);
	fillComingUpBoard();
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
	currentTetromino.rotation = 0;
	currentTetromino.id = comingUp[COMING_UP_AMOUNT - 1];
	consumeComingUpTetromino();

	if (checkCollision(currentTetromino)) {
		printf("\x1b[48;5;196mGame Over!\x1b[0m\n");
		exit(0);
	}
}

/*
 * Checks for full rows and clears them
 */
void cleanupBoard() {
	int originalCleared = rowsCleared;

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

	while(rowsCleared - originalCleared != 0) {
		score += 50 * (rowsCleared - originalCleared);
		originalCleared++;
	}
}

/*
 * Try to rotate passed tetrominoState according to SRS wallkick standards; rotationDirection -> 0 if increased, 1 if decreased
 */
int wallKick(TetrominoState *tetrominoState, int rotationDirection) {
	int result = 0, originalRotation = tetrominoState->rotation;
	Point originalPosition = tetrominoState->position;

	if (rotationDirection == 0) {
		tetrominoState->rotation++;
		tetrominoState->rotation %= 4;
	} else {
		tetrominoState-> rotation--;
		if (tetrominoState->rotation < 0) tetrominoState->rotation = 3;
	}

	if (tetrominoState->id == 0) return 1;
	else if (tetrominoState->id == 1) {
		for (int i = 0; i < 5; i++) {
			tetrominoState->position.x = originalPosition.x + wallKicksI[originalRotation][rotationDirection][i].x;
			tetrominoState->position.y = originalPosition.y + wallKicksI[originalRotation][rotationDirection][i].y;
			if (!checkCollision(*tetrominoState)) {
				result = 1;
				break;
			}
		}
	} else {
		for (int i = 0; i < 5; i++) {
			tetrominoState->position.x = originalPosition.x + (wallKicks[originalRotation][i].x - wallKicks[tetrominoState->rotation][i].x);
			tetrominoState->position.y = originalPosition.y + (wallKicks[originalRotation][i].y - wallKicks[tetrominoState->rotation][i].y);
			if (!checkCollision(*tetrominoState)) {
				result = 1;
				break;
			}
		}
	}

	return result;
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

void readXsetAttributes() {
	int stdoutPipe;
	popen2((char*[]){ "xset", "q", NULL }, NULL, &stdoutPipe);
	char buf[BUFSIZ];
	read(stdoutPipe, buf, sizeof(buf));
	for (int i = 0; i < (int)strlen(buf); i++) {
		if (strcomp(buf + i, AUTOREPEAT_TOGGLE, 0)) {
			attributes.toggle = rewriteUntilSpace(buf, i + (int)strlen(AUTOREPEAT_TOGGLE));
		}

		if (strcomp(buf + i, AUTOREPEAT_DELAY, 0)) {
			attributes.delay = rewriteUntilSpace(buf, i + (int)strlen(AUTOREPEAT_DELAY));
		}

		if (strcomp(buf + i, AUTOREPEAT_RATE, 0)) {
			attributes.rate = rewriteUntilSpace(buf, i + (int)strlen(AUTOREPEAT_RATE));
		}
	}
}

void setupXset() {
	readXsetAttributes();
	system("xset r rate 150 25");
}

int startmpv() {
	int stdoutPipe;
	return popen2((char*[]){ "mpv",  "--no-audio-display",  "--loop", "./soundtrack.mp3", NULL }, NULL, &stdoutPipe);
}

void resetKeypressDelay() {
#if USE_CONFIGURATION_SPECIFIC_QOL_FEATURES == 1
	char xsetCommand[BUFSIZ];
	snprintf(xsetCommand, BUFSIZ, "xset r rate %s %s", attributes.delay, attributes.rate);
	system(xsetCommand);
	snprintf(xsetCommand, BUFSIZ, "xset r %s", attributes.toggle);
	system(xsetCommand);
	free(attributes.delay);
	free(attributes.rate);
	free(attributes.toggle);
	kill(mpvSubprocessPID, SIGKILL);
#endif
	resetTermiosAttributes();
}

void signalHandler() {
	exit(0);
}

void initialize() {
#if USE_CONFIGURATION_SPECIFIC_QOL_FEATURES == 1
	setupXset();
	mpvSubprocessPID = startmpv();
#endif
	setupBoard();
	setupComingUpBoard();
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
				wallKick(&tempTetromino, 0);
				break;
			}
			case 'z': {
				wallKick(&tempTetromino, 1);
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

	Copyright efindus 2022;
}
