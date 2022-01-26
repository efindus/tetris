#include <stdlib.h>
#include <stdio.h>

#define HEIGHT 20
#define WIDTH 10
#define TPS 5

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
    { { 0, 0 }, { -1, 0 }, { 1, 0 }, { 2, 0 } },
    { { 0, 0 }, { -1, 0 }, { 1, 0 }, { 1, 1 } },
    { { 0, 0 }, { 1, 0 }, { -1, 0 }, { -1, 1 } },
    { { 0, 0 }, { -1, 0 }, { 0, 1 }, { 1, 1 } },
    { { 0, 0 }, { 1, 0 }, { 0, 1 }, { -1, 1 } },
    { { 0, 0 }, { -1, 0 }, { 1, 0 }, { 0, 1 } }
};

// rotation -> (0, 1, 2, 3) ordered as in here https://cdn.wikimg.net/en/strategywiki/images/7/7f/Tetris_rotation_super.png
struct Point* getRotatedTetromino(int tetrominoId, int rotation) {
    struct Point* returnValue = malloc(sizeof(struct Point) * 4);
    for (int i = 0; i < 4; i++) returnValue[i] = tetrominos[tetrominoId][i];

    if (tetrominoId == 1) {
        // TODO: make sth here
    } else if (tetrominoId != 0) {
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

int main() {
    // getRotatedTetromino(1, 2);
}
