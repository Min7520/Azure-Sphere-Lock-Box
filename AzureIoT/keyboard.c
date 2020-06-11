#include "keyboard.h"

#include <applibs/log.h>
#include <applibs/gpio.h>
#include "epoll_timerfd_utilities.h"

const int columnPins[4] = { 26, 28, 2, 1 };
const int rowPins[4] = { 43, 17, 38, 37 };

const signed char matrix[4][4] = {
	{'1', '2', '3', 'A'},
	{'4', '5', '6', 'B'},
	{'7', '8', '9', 'C'},
	{'*', '0', '#', 'D'},
};

int columnPinsFds[4];
int rowPinsFds[4];

int initKeyboard()
{
	for (int i = 0; i < 4; i++)
	{
		columnPinsFds[i] = GPIO_OpenAsOutput(columnPins[i], GPIO_OutputMode_PushPull, GPIO_Value_High);
		if (columnPinsFds[i] < 0)
			return -1;
	}
	for (int i = 0; i < 4; i++)
	{
		rowPinsFds[i] = GPIO_OpenAsInput(rowPins[i]);
		if (rowPinsFds[i] < 0)
			return -1;
	}
	return 0;
}

int cleanupKeyboard()
{
	for (int i = 0; i < 4; i++)
	{
		CloseFdAndPrintError(columnPinsFds[i], "Mode pin");
	}
	for (int i = 0; i < 4; i++)
	{
		CloseFdAndPrintError(rowPinsFds[i], "Mode pin");
	}

	return 0;
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

int checkForKeyPress(char *c)
{

	for (int i = 0; i < 4; i++)
	{
		int result = GPIO_SetValue(columnPinsFds[i], GPIO_Value_Low);
		if (result < 0)
			return -1;
		for (int j = 0; j < 4; j++)
		{
			  GPIO_Value_Type val;
			int result = GPIO_GetValue(rowPinsFds[j], &val);
			if (result < 0)
				return -1;

			if (val == GPIO_Value_Low)
			{
				//Log_Debug("%d, %d\n", i, j);

				result = GPIO_SetValue(columnPinsFds[i], GPIO_Value_High);
				if (result < 0)
					return -1;

				*c = matrix[j][i];
				return 0;
			}
				
		}
		result = GPIO_SetValue(columnPinsFds[i], GPIO_Value_High);
		if (result < 0)
			return -1;
	}
	return 0;
}