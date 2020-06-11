#include "app.h"

#include <string.h>
//#include <time.h>
#include <math.h>
#include <sys/time.h>

#include "display.h"
#include "keyboard.h"
#include "epoll_timerfd_utilities.h"
#include <applibs/log.h>
#include <applibs/gpio.h>

bool alert = false;
extern void SendTelemetry(const unsigned char* key, const unsigned char* value);
enum operationTypeEnum {
	PICK,
	POST
};

enum appStateEnum {
	NONE,
	SELECT,
	CODE,
	ID,
	OPEN,
	CLOSED,
	WAIT,
	INVALID_CREDENTIALS,
	DONE,
	ERROR,
	DRAWER_LOCKED
};

enum actionEnum {
	NOTHING,
	NEXT_SCREEN,
	PREVIOUS_SCREEN,
	ADD_TO_VALUE,
	REMOVE_FROM_VALUE
};

enum lockStateEnum {
	LOCK_OPEN = GPIO_Value_High,
	LOCK_CLOSED = GPIO_Value_Low
};

struct appStateContainer {
	enum appStateEnum appState;
	enum operationTypeEnum operationType;
	enum lockStateEnum lockState;
	bool isKeyPressed;
	bool redrawRequired;
	bool isReopen;
	bool isValidationSuccessful;
	bool isEmpty;
	uint8_t wrongAttempts;
	bool alert;
};

static char secretCode[7] =  "";
static char savedCode[7] = "";

static const int lockPin = 0;
static const int lockStatePin = 27;
static int lockPinFd = -1;
static int lockStatePinFd = -1;

static bool stateChanged(enum appStateEnum currentState)
{
	static enum appStateEnum previousState = NONE;

	if (currentState != previousState)
	{
		previousState = currentState;
		return true;
	}

	return false;
}

static void saveSecretCode()
{
	strcpy(savedCode, secretCode);
}

static bool lockStateChanged(enum lockStateEnum* state)
{
	static GPIO_Value_Type previousState = GPIO_Value_Low;

	GPIO_Value_Type lockState;
	int result = GPIO_GetValue(lockStatePinFd, &lockState);
	if (result < 0)
		return -1;

	if (lockState != previousState)
	{
		*state = lockState;
		previousState = lockState;
		return true;
	}

	return false;
}

static void clearSecretCode()
{
	for (int i = 0; i < 6; i++)
		secretCode[i] = '\0';
}

int initApp()
{
	lockPinFd = GPIO_OpenAsOutput(lockPin, GPIO_OutputMode_OpenDrain, GPIO_Value_High);
	if (lockPinFd < 0)
		return -1;

	lockStatePinFd = GPIO_OpenAsInput(lockStatePin);
	if (lockStatePinFd < 0)
		return -1;

	int result = initDisplay();
	if (result < 0)
		return -1;

	result = initKeyboard();
	if (result < 0)
		return -1;

	return 0;
}

void cleanupApp()
{
	CloseFdAndPrintError(lockPinFd, "Lock pin");
	cleanupDisplay();
	cleanupKeyboard();
}

static bool isValidCode()
{
	if (!strcmp(secretCode, savedCode))
		return true;
	return false;
}

static int setPulse(int uS)
{
	struct timespec sleepTime = { 0, uS*1000 };
	int result = GPIO_SetValue(lockPinFd, GPIO_Value_High);
	if (result < 0)
		return -1;
	nanosleep(&sleepTime, NULL);

	sleepTime.tv_nsec = 20 * 1000 * 1000 - uS * 1000;
	result = GPIO_SetValue(lockPinFd, GPIO_Value_Low);
	if (result < 0)
		return -1;
	nanosleep(&sleepTime, NULL);
}

static long getTimeMs()
{
	struct timeval te;
	gettimeofday(&te, NULL); // get current time
	long long milliseconds = te.tv_sec * 1000LL + te.tv_usec / 1000; // calculate milliseconds
	// printf("milliseconds: %lld\n", milliseconds);
	return milliseconds;
}

static int unlock()
{
	long start = getTimeMs();
	long now = start;
	while (now - start < 500)
	{
		now = getTimeMs();
		if (setPulse(1900) < 0)
			return -1;
	}

	start = getTimeMs();
	now = start;
	while (now - start < 500)
	{
		now = getTimeMs();
		if (setPulse(1000) < 0)
			return -1;
	}
}

static enum actionEnum keyToAction(char key)
{
	switch (key)
	{
	case '!':
	case '#':
	case 'A':
	case 'B':
		return NEXT_SCREEN;
		break;
	case '*':
		return PREVIOUS_SCREEN;
		break;
	case 'D':
		return REMOVE_FROM_VALUE;
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
	case '0':
		return ADD_TO_VALUE;
		break;
	}
	return NOTHING;
}

static int drawSelect(bool isEmpty)
{

	int result = fillScreen(0xba9b02);
	if (result < 0)
		return -1;

	result = drawLine(0, 15, 95, 15, 0xFFFFFF);
	if (result < 0)
		return -1;

	if (isEmpty)
	{
		result = drawText("Drawer is empty", 5, 5, 0xFFFFFF);
		if (result < 0)
			return -1;

		result = drawText("A. store", 25, 30, 0xFFFFFF);
		if (result < 0)
			return -1;
	}
	else
	{
		result = drawText("Drawer is occupied", 5, 5, 0xFFFFFF);
		if (result < 0)
			return -1;

		result = drawText("A. pick up", 25, 30, 0xFFFFFF);
		if (result < 0)
			return -1;
	}

	return 0;
}

static int drawCode(bool redrawAll)
{
	int result;
	if (redrawAll)
	{
		result = fillScreen(0xba9b02);
		if (result < 0)
			return -1;

		result = drawText("Type your", 5, 5, 0xFFFFFF);
		if (result < 0)
			return -1;

		result = drawText("6-digits code", 5, 15, 0xFFFFFF);
		if (result < 0)
			return -1;

		result = drawLine(0, 23, 95, 23, 0xFFFFFF);
		if (result < 0)
			return -1;

		result = drawText(secretCode, 15, 35, 0xFFFFFF);
		if (result < 0)
			return -1;

		result = drawLine(10, 45, 85, 45, 0x36342e);
		if (result < 0)
			return -1;
	}
	else {
		result = drawRectangle(15, 35, 85, 8, 0xba9b02, 1, 0xba9b02);
		if (result < 0)
			return -1;

		result = drawText(secretCode, 15, 35, 0xFFFFFF);
		if (result < 0)
			return -1;
	}
}

static int drawOpen()
{
	int result = fillScreen(0xba9b02);
	if (result < 0)
		return -1;

	result = drawText("Locker is now", 5, 5, 0xFFFFFF);
	if (result < 0)
		return -1;

	result = drawText("open", 5, 15, 0xFFFFFF);
	if (result < 0)
		return -1;

	result = drawLine(0, 23, 95, 23, 0xFFFFFF);
	if (result < 0)
		return -1;
}

static int drawClosed()
{
	int result = fillScreen(0xba9b02);
	if (result < 0)
		return -1;

	result = drawText("Locker is now", 5, 5, 0xFFFFFF);
	if (result < 0)
		return -1;

	result = drawText("closed", 5, 15, 0xFFFFFF);
	if (result < 0)
		return -1;

	result = drawLine(0, 23, 95, 23, 0xFFFFFF);
	if (result < 0)
		return -1;

	result = drawText("A. Done", 20, 35, 0xFFFFFF);
	if (result < 0)
		return -1;

	result = drawText("B. Open again", 20, 45, 0xFFFFFF);
	if (result < 0)
		return -1;

	return 0;
}

static int drawWait()
{
	int result = fillScreen(0x404040);
	if (result < 0)
		return -1;

	result = drawText("Please wait...", 15, 28, 0xFFFFFF);
	if (result < 0)
		return -1;

	return 0;
}

static int drawInvalidCredentials()
{
	int result = fillScreen(0x404040);
	if (result < 0)
		return -1;

	result = drawText("Invalid code", 15, 28, 0xFFFFFF);
	if (result < 0)
		return -1;

	return 0;
}

static int drawDrawerLocked()
{
	int result = fillScreen(0x404040);
	if (result < 0)
		return -1;

	result = drawText("Too many", 15, 20, 0xFFFFFF);
	if (result < 0)
		return -1;

	result = drawText("failed attempts", 15, 30, 0xFFFFFF);
	if (result < 0)
		return -1;

	return 0;
}

static int draw(bool isNewState, const struct appStateContainer*appState)
{
	return 0;
	switch (appState->appState)
	{
	case SELECT:
		return drawSelect(appState->isEmpty);
	case CODE:
		return drawCode(isNewState);
		break;
	case OPEN:
		return drawOpen();
		break;
	case CLOSED:
		return drawClosed();
		break;
	case WAIT:
		return drawWait();
		break;
	case INVALID_CREDENTIALS:
		return drawInvalidCredentials();
		break;
	case DRAWER_LOCKED:
		return drawDrawerLocked();
		break;
	}

	return 0;
}

static bool isValidCodeValue()
{
	if (strlen(secretCode) == 6)
		return true;
	return false;
}

static bool nextScreen(char keyPressed, struct appStateContainer* appState)
{
	switch (appState->appState)
	{
	case SELECT:
		if (keyPressed == 'A')
		{
			appState->appState = CODE;
			appState->operationType = PICK;
			return true;
		}
	case CODE:
		if (isValidCodeValue() && keyPressed == '#')
		{
			if (!(appState->isEmpty) && !isValidCode())
			{
				appState->wrongAttempts++;
				appState->appState = INVALID_CREDENTIALS;
				return true;
			}
			appState->appState = WAIT;
			return true;
		}
		break;

	case WAIT:
		if (keyPressed = '!' && appState->lockState == LOCK_OPEN)
		{
			clearSecretCode();
			appState->appState = OPEN;
			return true;
		}
		break;
	case OPEN:
		if (keyPressed = '!' && appState->lockState == LOCK_CLOSED)
		{
			appState->appState = CLOSED;
			SendTelemetry("LockClosed", "Lock is now closed.");
			return true;
		}
		break;
	case CLOSED:
		if (keyPressed == 'A')
		{
			appState->appState = SELECT;
			return true;
		}
		else if (keyPressed == 'B')
		{
			appState->appState = WAIT;
			appState->isReopen = true;
			return true;
		}
		break;
	case INVALID_CREDENTIALS:
		clearSecretCode();
		if (appState->wrongAttempts >= 3)
		{
			appState->appState = DRAWER_LOCKED;
			return true;
		}
		appState->appState = CODE;
		return true;
		break;
	case DRAWER_LOCKED:
		appState->appState = SELECT;
		appState->wrongAttempts = 0;
		return true;
		break;
	}
	return false;
}

static bool previousScreen(struct appStateContainer* appState)
{
	switch (appState->appState)
	{
	case CODE:
		appState->appState = SELECT;
		clearSecretCode();
		return true;
	}
	return false;
}

bool updateCodeValue(char c)
{
	int len = strlen(secretCode);
	if (len == 6)
		return false;
	secretCode[len] = c;
	return true;
}

bool removeDigitFromCodeValue()
{
	int len = strlen(secretCode);
	if (len == 0)
		return false;
	secretCode[len - 1] = '\0';
	return true;
}



static bool doAction(char key, struct appStateContainer* appState)
{
	if (key == '!' && appState->appState != OPEN && appState->appState != WAIT && !alert)
	{
		Log_Debug("Alert!\n");
		SendTelemetry("ButtonPress", "Alert! Lock open.");
		alert = true;
	}

	enum actionEnum nextAction;
	nextAction = keyToAction(key);

	switch (nextAction)
	{
	case NEXT_SCREEN:
		return nextScreen(key, appState);
		break;
	case PREVIOUS_SCREEN:
		return previousScreen(appState);
		break;
		break;
	case ADD_TO_VALUE:
		if (appState->appState == CODE)
		{
			return updateCodeValue(key);
		}
		break;
	case REMOVE_FROM_VALUE:
		if (appState->appState == CODE)
		{
			return removeDigitFromCodeValue();
		}
		break;
	default:
		break;
	}
	return 0;
}

static void manageState(bool isNewState, struct appStateContainer* appState)
{
	const struct timespec sleepTime = { 3, 0 };
	const struct timespec sleepTime1 = { 60, 0 };
	switch (appState->appState)
	{
	case WAIT:
		if (isNewState)
		{
			if (appState->isReopen)
			{
				unlock();
				SendTelemetry("LockOpened", "Lock reopened.");
				appState->isReopen = false;
			}
			else
			{
				if (appState->isEmpty)
				{
					appState->isEmpty = false;
					saveSecretCode();
					//clearSecretCode();
					unlock();
					SendTelemetry("LockOpened", "Lock opened to store item.");
				}
				else if (isValidCodeValue())
				{
					if (isValidCode())
					{
						appState->isEmpty = true;
						appState->wrongAttempts = 0;
						unlock();
						SendTelemetry("LockOpened", "Lock reopened to pick up item");
						//clearSecretCode();
					}
				}
			}
		}
		break;
	case INVALID_CREDENTIALS:
		nanosleep(&sleepTime, NULL);
		break;
	case DRAWER_LOCKED:
		nanosleep(&sleepTime1, NULL);
		break;
	}
}

void appStateStructInit(struct appStateContainer* appState)
{
	appState->appState = SELECT;
	appState->operationType = PICK;
	appState->isKeyPressed = false;
	appState->redrawRequired = true;
	appState->isReopen = false;
	appState->isValidationSuccessful = true;
	appState->lockState = GPIO_Value_Low;
	appState->isEmpty = true;
	appState->wrongAttempts = 0;
	appState->alert = false;
}

int runApp()
{
	static struct appStateContainer appState;
	static bool fstRun = true;

	//manage events
	bool changed = lockStateChanged(&(appState.lockState));

	if (fstRun)
	{
		appStateStructInit(&appState);
		fstRun = false;
	}
	if (appState.appState == INVALID_CREDENTIALS || appState.appState == DRAWER_LOCKED)
	{
		appState.redrawRequired = doAction('!', &appState);
	}
	else if (changed)
	{
		appState.redrawRequired = doAction('!', &appState);
	}

	char key = 0;
	int result = checkForKeyPress(&key);
	if (result < 0)
		return -1;
	else if (key != 0 && !appState.isKeyPressed)
	{
		appState.isKeyPressed = true;

		appState.redrawRequired = doAction(key, &appState);
	}
	else if (key == 0)
	{
		appState.isKeyPressed = false;
	}

	bool isNewState = stateChanged(appState.appState);

	//manage drawing
	if (appState.redrawRequired)
	{
		int result = draw(isNewState, &appState);
		if (result < 0)
			return -1;
		appState.redrawRequired = false;
	}

	//manage state
	manageState(isNewState, &appState);

	return 0;
}