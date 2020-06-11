#include "display.h"

#include <stdbool.h>
#include <errno.h>
#include <signal.h>
#include <string.h>

#include <applibs/log.h>
#include <applibs/gpio.h>

#define SPI_STRUCTS_VERSION 1
#include <applibs/spi.h>

#include "epoll_timerfd_utilities.h"

#include "font.h"

static int spiFd = -1; /*!< File descriptor for SPI peripheral. */
static int modePinFd = -1; /*!< File descriptor for mode selection pin LOW for command, HIGH for data. */
static int resetPinFd = -1; /*!< File descriptor for rest pin LOW resets the display. */

static const int modePin = 42; /*!< number of GPIO for mode pin. */
static const int resetPin = 16; /*!< number of GPIO for reset pin. */

/**
* Compares expected number of bytes to be send through SPI with actual number of bytes sent through SPI.
*
* @param expectedBytes number of expected bytes to be send through SPI.
* @param actualBytes number of actual bytes sent through SPI.
*/
static bool CheckTransferSize(size_t expectedBytes, ssize_t actualBytes)
{
	if (actualBytes < 0)
		return false;

	if (actualBytes != (ssize_t)expectedBytes)
		return false;

	return true;
}

/**
* Structure that holds color in three bytes: red, blue and green.
*/
struct colorStruct {
	uint8_t r; /**< Red color. */
	uint8_t g; /**< Green color. */
	uint8_t b; /**< Blue color. */
};

/**
* Converts hexadecimal color value to colorStruct structure.
*
* @param color Hexadecimal color to be converted to colorStruct structure.
* @return Converted color.
*/
static struct colorStruct hexToColor(uint32_t color)
{
	struct colorStruct c;
	c.r = ((color >> 16) & 0xFF);
	c.g = ((color >> 8) & 0xFF);
	c.b = ((color) & 0xFF);

	//shift to right so colors are represented in 6 or 5 bits
	c.r = (c.r >> 3) & 0b00111110;
	c.g = (c.g >> 2) & 0b00111111;
	c.b = (c.b >> 2) & 0b00111110;

	return c;
}

/**
* Set modePin to low so display awaits commands.
*/
static int displayCommandMode()
{
	return GPIO_SetValue(modePinFd, GPIO_Value_Low);
}

/**
* Set modePin to high so display awaits data to be filled in RAM.
*/
static int displayDataMode()
{
	return GPIO_SetValue(modePinFd, GPIO_Value_High);
}

/**
* Wait given amount of microseconds
*
* @param us Number of microseconds to wait.
*/
static void wait(int us)
{
	const struct timespec sleepTime = { 0, 1000 * us };
	nanosleep(&sleepTime, NULL);
}

/**
* Send to display whether next drawn rectangle should be filled with color.
*
* @param fill Set to true if rectangle should be filled.
* @return 0 or -1 if something went wrong.
*/
static int shouldFillRectangle(bool fill)
{
	if(displayCommandMode() < 0)
		return -1;

	const size_t transferCount = 1;
	SPIMaster_Transfer transfer;

	int result = SPIMaster_InitTransfers(&transfer, transferCount);
	if (result != 0)
		return -1;

	const uint8_t command[] = { 0x26, (char)fill };
	transfer.flags = SPI_TransferFlags_Write;
	transfer.writeData = command;
	transfer.length = sizeof(command);

	ssize_t transferredBytes = SPIMaster_TransferSequential(spiFd, &transfer, transferCount);

	if (!CheckTransferSize(transfer.length, transferredBytes))
		return -1;

	wait(5);

	return 0;
}

/**
* Toggle resetPin HIGH -> LOW -> HIGH to reset display.
*/
static int resetDisplay()
{
	

	const struct timespec sleepTime = { 0, 6000 };
	int res = GPIO_SetValue(resetPinFd, GPIO_Value_High);
	if (res < 0)
		return -1;
	nanosleep(&sleepTime, NULL);
	res = GPIO_SetValue(resetPinFd, GPIO_Value_Low);
	if (res < 0)
		return -1;
	nanosleep(&sleepTime, NULL);
	res = GPIO_SetValue(resetPinFd, GPIO_Value_High);
	if (res < 0)
		return -1;

	return 0;
}

/**
* Draw one pixel on the display.
*
* @param posX Horizontal position of the pixel. Should be between 0 and 95.
* @param posY Vertical position of the pixel. Should be between 0 and 63.
* @param color Color of the pixel.
* @return 0 or -1 if something went wrong.
*/
int drawPixel(int posX, int posY, uint32_t color)
{
	return drawRectangle(posX, posY, 0, 0, color, 0, 0);
}

/**
* Draw line on the display.
*
* @param startX Horizontal position of the start of the line. Should be between 0 and 95.
* @param startY Vertical position of the start of the line. Should be between 0 and 63.
* @param endX Horizontal position of the end of the line. Should be between 0 and 95.
* @param endY Vertical position of the end of the line. Should be between 0 and 63.
* @param color Color of the line.
* @return 0 or -1 if something went wrong.
*/
int drawLine(int startX, int startY, int endX, int endY, uint32_t color)
{
	struct colorStruct c = hexToColor(color);

	if(displayCommandMode() < 0)
		return -1;

	const size_t transferCount = 1;
	SPIMaster_Transfer transfer;

	int result = SPIMaster_InitTransfers(&transfer, transferCount);
	if (result != 0) {
		return -1;
	}

	const uint8_t command[] = { 0x21, startX, startY, endX, endY, c.r, c.g, c.b };
	transfer.flags = SPI_TransferFlags_Write;
	transfer.writeData = command;
	transfer.length = sizeof(command);

	ssize_t transferredBytes = SPIMaster_TransferSequential(spiFd, &transfer, transferCount);

	if (!CheckTransferSize(transfer.length, transferredBytes))
		return -1;

	wait(10);

	return 0;
}

/**
* Draw character on the display.
*
* @param c Character to be drawn. Should be between 32 and 127.
* @param startX Leftmost pixel of the character.
* @param startY Topmost pixel of the character
* @param color Color of the character.
* @return width of drawn character in pixels or -1 if something went wrong.
*/
int drawChar(char c, int startX, int startY, uint32_t color)
{
	int fontTableCursor = START_OF_CHAR_WIDTHS;//first char's width in font table
	fontTableCursor += c - fontTable[FIRST_CHAR];//get index of char's width of given element
	int charWidthInWords = fontTable[fontTableCursor];//get width (number of words (16 bits)) of given element from font table

	//now see how much cursor needs to jump from end of char widths part of the font table to the first byte of given character c
	int offset = 0;
	for (int i = fontTableCursor - 1; i >= START_OF_CHAR_WIDTHS; i--)
	{
		offset += fontTable[i];//add to offset width (word) of every character that is before given character c in font table
	}
	offset *= 2;//convert from word to byte

	//get the position of the first byte of given character c in font table
	fontTableCursor = START_OF_CHAR_WIDTHS + fontTable[CHAR_COUNT] + offset;

	int maxWidth = 0;//while drawing look for true width of the given character

	//probably can be done in one loop but whatever
	for (int i = 0; i < charWidthInWords; i++)//first half of the character
	{
		for (int j = 0; j < 8; j++)//for every bit in byte
		{
			if (fontTable[fontTableCursor + i] & (1 << j))//see if current bit is set
			{
				if (i > maxWidth)//check if it expanded character and if so then update max width
					maxWidth = i;

				int result = drawPixel(startX + i, startY + j - 3, color);//finally draw the pixel
				if (result != 0)
					return -1;
			}
		}
	}
	for (int i = charWidthInWords; i < charWidthInWords*2; i++)//second half of the character
	{
		for (int j = 0; j < 8; j++)//for every bit in byte
		{
			if (fontTable[fontTableCursor + i] & (1 << j))//see if current bit is set
			{
				if (i - charWidthInWords > maxWidth)//check if it expanded character and if so then update max width
					maxWidth = i - charWidthInWords;

				int result = drawPixel(startX + i - charWidthInWords, startY + j, color);// finally draw the pixel
				if (result != 0)
					return -1;
			}
		}
	}

	return maxWidth;
}

/**
* Draw text on the display.
*
* @param text Text to be drawn.
* @param x Leftmost side of the text
* @param y Topmost side of the text
* @param color Color of the text.
* @return 0 or -1 if something went wrong.
*/
int drawText(const char* text, int x, int y, uint32_t color)
{
	int len = strlen(text);//get length of given string
	int cursor = 0;
	for (int i = 0; i < len; i++)//draw every character one by one
	{
		int charWidth = drawChar(text[i], x + cursor, y, color);
		if (charWidth < 0)
			return -1;
		cursor += charWidth + 2;//place where next character should be drawn add 2 to increase spacing a bit
	}
	return 0;
}

/*
* Draw rectangle on the display.
*
* @param startX Horizontal start point of the rectangle.
* @param startY Vertical start point of the rectangle.
* @param width Width of the rectangle.
* @param height Height of the rectangle.
* @param color Color of the outline.
* @param fill Set to true if rectangle should be filled with fillColor.
* @param fillColor Color of the fill. Set to whatever if fill is false.
* @return 0 or -1 if something went wrong.
*/
int drawRectangle(int startX, int startY, int width, int height, uint32_t color, bool fill, uint32_t fillColor)
{
	struct colorStruct c = hexToColor(color);

	struct colorStruct f = hexToColor(fillColor);

	int result = shouldFillRectangle(fill);
	if (result != 0)
		return -1;

	if (displayCommandMode() < 0)
		return -1;

	const size_t transferCount = 1;
	SPIMaster_Transfer transfer;

	result = SPIMaster_InitTransfers(&transfer, transferCount);
	if (result != 0)
		return -1;

	const uint8_t command[] = { 0x22, startX, startY, startX + width, startY + height, c.r, c.g, c.b, f.r, f.g, f.b };
	transfer.flags = SPI_TransferFlags_Write;
	transfer.writeData = command;
	transfer.length = sizeof(command);

	ssize_t transferredBytes = SPIMaster_TransferSequential(spiFd, &transfer, transferCount);

	if (!CheckTransferSize(transfer.length, transferredBytes))
		return -1;

	wait(200);

	return 0;
}

/**
* Fill screen with given color.
*
* @param color color to fill screen with.
* @return 0 or -1 if something went wrong.
*/
int fillScreen(uint32_t color)
{
	struct colorStruct c = hexToColor(color);

	int result = shouldFillRectangle(true);
	if (result != 0)
		return -1;

	if (displayCommandMode() < 0)
		return -1;

	const size_t transferCount = 1;
	SPIMaster_Transfer transfer;

	result = SPIMaster_InitTransfers(&transfer, transferCount);
	if (result != 0)
		return -1;

	const uint8_t command[] = { 0x22, 0, 0, 95, 63, c.r, c.g, c.b, c.r, c.g, c.b };
	transfer.flags = SPI_TransferFlags_Write;
	transfer.writeData = command;
	transfer.length = sizeof(command);

	ssize_t transferredBytes = SPIMaster_TransferSequential(spiFd, &transfer, transferCount);

	if (!CheckTransferSize(transfer.length, transferredBytes))
		return -1;

	wait(300);

	return 0;
}

/**
* Init peripherals required for display to work.
*
* Inits ISU1 SPI and two GPIO 42 and 16.
* @return 0 or -1 if something went wrong.
*/
int initDisplay()
{
	modePinFd = GPIO_OpenAsOutput(modePin, GPIO_OutputMode_PushPull, GPIO_Value_High);
	if (modePinFd < 0)
		return -1;

	resetPinFd = GPIO_OpenAsOutput(resetPin, GPIO_OutputMode_PushPull, GPIO_Value_High);
	if (resetPinFd < 0)
		return -1;
	
	SPIMaster_Config config;
	int ret = SPIMaster_InitConfig(&config);
	if (ret != 0)
		return -1;

	config.csPolarity = SPI_ChipSelectPolarity_ActiveLow;
	spiFd = SPIMaster_Open(1, -1, &config);
	if (spiFd < 0)
		return -1;

	int result = SPIMaster_SetBusSpeed(spiFd, 400000);
	if (result != 0)
		return -1;
	
	if (resetDisplay() < 0)
		return -1;

	if (displayCommandMode() < 0)
		return -1;

	const size_t transferCount = 1;
	SPIMaster_Transfer transfer;

	result = SPIMaster_InitTransfers(&transfer, transferCount);
	if (result != 0)
		return -1;

	const uint8_t command[] = { 0xAF, 0xA0, 0b00100000 };
	transfer.flags = SPI_TransferFlags_Write;
	transfer.writeData = command;
	transfer.length = sizeof(command);

	ssize_t transferredBytes = SPIMaster_TransferSequential(spiFd, &transfer, transferCount);

	if (!CheckTransferSize(transfer.length, transferredBytes))
		return -1;

	result = fillScreen(0x000000);
	if (result != 0)
		return -1;

	return 0;
}

/**
* Cleanup peripherals used by the display.
*/
void cleanupDisplay()
{
	CloseFdAndPrintError(spiFd, "Spi");
	CloseFdAndPrintError(resetPinFd, "Reset pin");
	CloseFdAndPrintError(modePinFd, "Mode pin");
}