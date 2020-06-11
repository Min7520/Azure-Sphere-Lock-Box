#pragma once
#include <stdint.h>
#include <stdbool.h>

int initDisplay();
void cleanupDisplay();

int drawPixel(int posX, int posY, uint32_t color);
int drawLine(int startX, int startY, int endX, int endY, uint32_t color);
int drawChar(char ascii, int startX, int startY, uint32_t color);
int drawText(const char *text, int x, int y, uint32_t color);
int drawRectangle(int startX, int startY, int width, int height, uint32_t color, bool fill, uint32_t fillColor);
int fillScreen(uint32_t color);