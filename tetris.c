#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// Constants
#define HEIGHT 20
#define WIDTH 15
#define TPS 2

// Drawing blocks
#define VOID "\x1b[0m  "
#define BORDER "\x1b[44m  "

struct Point {
	int x, y;
};

// Tetrominos
// 0 -> O shape; 1 -> I shape; 2 -> L shape; 3 -> J shape; 4 -> S shape; 5 -> Z shape; 6 -> T shape
struct Point tetrominos[][4] = {
	{ { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } },
	{ { 0, 0 }, { 1, 0 }, { -1, 0 }, { -2, 0 } },
	{ { 0, 0 }, { -1, 0 }, { 1, 0 }, { 1, 1 } },
	{ { 0, 0 }, { 1, 0 }, { -1, 0 }, { -1, 1 } },
	{ { 0, 0 }, { -1, 0 }, { 0, 1 }, { 1, 1 } },
	{ { 0, 0 }, { 1, 0 }, { 0, 1 }, { -1, 1 } },
	{ { 0, 0 }, { -1, 0 }, { 1, 0 }, { 0, 1 } },
};

// Tetromino colors
// 0 -> O shape; 1 -> I shape; 2 -> L shape; 3 -> J shape; 4 -> S shape; 5 -> Z shape; 6 -> T shape
char* tetrominoColors[] = {
	"\x1b[48;5;226m  ",
	"\x1b[48;5;51m  ",
	"\x1b[48;5;208m  ",
	"\x1b[44m  ",
	"\x1b[42m  ",
	"\x1b[41m  ",
	"\x1b[45m  ",
};

char board[WIDTH + 2][HEIGHT + 1][25];
char frameBuffer[WIDTH + 2][HEIGHT + 1][25];

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
}

/*
 * rotation -> (0, 1, 2, 3) ordered as in here https://cdn.wikimg.net/en/strategywiki/images/7/7f/Tetris_rotation_super.png
 * but for I shaped tetromino there are only two rotations
 */
struct Point* getRotatedTetromino(int tetrominoId, int rotation) {
	struct Point* returnValue = malloc(sizeof(struct Point) * 4);
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
};

// Draws selected tetromino using contents of board array as background (it only ensures nothing is set out of the array, so no collision checks are performed here).
// If tetrominoId is -1 it will ignore it and simply draw contents of the board array
void drawTetromino(struct Point position, int tetrominoId, int rotation) {
	for (int x = 0; x < WIDTH + 2; x++)
		for (int y = 0; y < HEIGHT + 1; y++)
			strcpy(frameBuffer[x][y], board[x][y]);

	if (tetrominoId != -1) {
		struct Point* tetromino = getRotatedTetromino(tetrominoId, rotation);
		for(int i = 0; i < 4; i++) {
			int x = position.x + tetromino[i].x + 1, y = position.y + tetromino[i].y + 1;
			if (0 <= x && x < WIDTH + 2 && 0 <= y && y < HEIGHT + 1) strcpy(frameBuffer[x][y], tetrominoColors[tetrominoId]);
		}
		free(tetromino);
	}

	drawFrame();
}

int main() {
	struct timespec time;
	time.tv_sec = 0;
	time.tv_nsec = 1000 * 1000 * (1000 / TPS);

	int rotation = 0, currentTetromino = 0;

	setupBoard();
	struct Point basePosition = { WIDTH / 2, HEIGHT / 4 * 3 };

	while(1) {
		drawTetromino(basePosition, currentTetromino, rotation);

		rotation++;
		if (rotation == 4) {
			rotation %= 4;
			currentTetromino++;
			currentTetromino %= 7;
		}

		if (nanosleep(&time, NULL) < 0)
			perror("[ERROR] nanosleep()");
	}
}
