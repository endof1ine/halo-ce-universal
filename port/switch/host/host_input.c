/*
HOST_INPUT.C

The guest's SDL3 gamepads (xinput_sdl.c reads them as the Xbox's), on the
Switch's controllers: up to four players, the first one also the console's
attached Joy-Con (handheld). A gamepad's SDL id is its player number,
1 to 4.

Buttons go by position, as on the other ports: the bottom face button is
the Xbox's A (jump), the right one B, the left one X, the top one Y. ZL and
ZR, which are buttons here, are the triggers at full travel.

Rumble: the Xbox's low-frequency (heavy) motor plays in HD rumble's low
band and its high-frequency one in the high band.
*/

#include "host.h"

#include <SDL3/SDL_gamepad.h>
#include <string.h>
#include <switch.h>

#define PLAYERS 4
#define RUMBLE_LOW_HZ 160.0f
#define RUMBLE_HIGH_HZ 320.0f
/* (the Xbox's motors at full strength are much gentler than HD rumble's) */
#define RUMBLE_STRENGTH 0.6f

static PadState pads[PLAYERS];
static int pad_connected[PLAYERS];
static Mutex input_lock;

struct rumble
{
	HidVibrationDeviceHandle handles[2];
	int count;
	u32 style; /* the style the handles were made for */
	uint32_t low, high;
};

static struct rumble rumbles[PLAYERS];

void host_input_initialize(void)
{
	int player;

	mutexInit(&input_lock);
	padConfigureInput(PLAYERS, HidNpadStyleSet_NpadStandard);
	padInitialize(&pads[0], HidNpadIdType_No1, HidNpadIdType_Handheld);
	for (player = 1; player < PLAYERS; player++)
		padInitialize(&pads[player], (HidNpadIdType)(HidNpadIdType_No1 + player));
}

void host_input_update(void)
{
	int player;

	mutexLock(&input_lock);
	for (player = 0; player < PLAYERS; player++)
	{
		int connected;

		padUpdate(&pads[player]);
		connected = padIsConnected(&pads[player]);
		if (connected && !pad_connected[player])
		{
			host_logf(HOST_LOG_INFO, "player %d's controller connected (styles 0x%x)", player + 1,
				padGetStyleSet(&pads[player]));
			host_sdl_queue_gamepad_added((uint32_t)player + 1);
		}
		pad_connected[player] = connected;
	}
	mutexUnlock(&input_lock);
}

static PadState *pad_of(uint32_t gamepad)
{
	if (gamepad < 1 || gamepad > PLAYERS || !pad_connected[gamepad - 1])
		return NULL;
	return &pads[gamepad - 1];
}

/* ---------- the guest's interface */

int host_sdl_get_gamepads(uint32_t *ids, int capacity)
{
	int player, count = 0;

	for (player = 0; player < PLAYERS && count < capacity; player++)
	{
		if (pad_connected[player])
			ids[count++] = (uint32_t)player + 1;
	}
	return count;
}

uint32_t host_sdl_open_gamepad(uint32_t id)
{
	return id >= 1 && id <= PLAYERS ? id : 0;
}

uint32_t host_sdl_gamepad_from_id(uint32_t id)
{
	return host_sdl_open_gamepad(id);
}

int host_sdl_gamepad_button(uint32_t gamepad, int button)
{
	PadState *pad = pad_of(gamepad);
	u64 held, mask;

	if (!pad)
		return 0;
	held = padGetButtons(pad);
	switch (button)
	{
	case SDL_GAMEPAD_BUTTON_SOUTH: mask = HidNpadButton_B; break;
	case SDL_GAMEPAD_BUTTON_EAST: mask = HidNpadButton_A; break;
	case SDL_GAMEPAD_BUTTON_WEST: mask = HidNpadButton_Y; break;
	case SDL_GAMEPAD_BUTTON_NORTH: mask = HidNpadButton_X; break;
	case SDL_GAMEPAD_BUTTON_BACK: mask = HidNpadButton_Minus; break;
	case SDL_GAMEPAD_BUTTON_START: mask = HidNpadButton_Plus; break;
	case SDL_GAMEPAD_BUTTON_LEFT_STICK: mask = HidNpadButton_StickL; break;
	case SDL_GAMEPAD_BUTTON_RIGHT_STICK: mask = HidNpadButton_StickR; break;
	case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: mask = HidNpadButton_L; break;
	case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: mask = HidNpadButton_R; break;
	case SDL_GAMEPAD_BUTTON_DPAD_UP: mask = HidNpadButton_Up; break;
	case SDL_GAMEPAD_BUTTON_DPAD_DOWN: mask = HidNpadButton_Down; break;
	case SDL_GAMEPAD_BUTTON_DPAD_LEFT: mask = HidNpadButton_Left; break;
	case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: mask = HidNpadButton_Right; break;
	default: return 0;
	}
	return (held & mask) != 0;
}

static int clamp_axis(int value)
{
	return value < -32768 ? -32768 : value > 32767 ? 32767 : value;
}

int host_sdl_gamepad_axis(uint32_t gamepad, int axis)
{
	PadState *pad = pad_of(gamepad);
	HidAnalogStickState stick;

	if (!pad)
		return 0;
	switch (axis)
	{
	case SDL_GAMEPAD_AXIS_LEFTX:
		return clamp_axis(padGetStickPos(pad, 0).x);
	case SDL_GAMEPAD_AXIS_LEFTY:
		/* (SDL's y grows downwards) */
		stick = padGetStickPos(pad, 0);
		return clamp_axis(-stick.y);
	case SDL_GAMEPAD_AXIS_RIGHTX:
		return clamp_axis(padGetStickPos(pad, 1).x);
	case SDL_GAMEPAD_AXIS_RIGHTY:
		stick = padGetStickPos(pad, 1);
		return clamp_axis(-stick.y);
	case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
		return (padGetButtons(pad) & HidNpadButton_ZL) ? 32767 : 0;
	case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
		return (padGetButtons(pad) & HidNpadButton_ZR) ? 32767 : 0;
	default:
		return 0;
	}
}

int host_sdl_gamepad_type(uint32_t gamepad)
{
	PadState *pad = pad_of(gamepad);
	u32 style;

	if (!pad)
		return SDL_GAMEPAD_TYPE_UNKNOWN;
	style = padGetStyleSet(pad);
	if (style & HidNpadStyleTag_NpadJoyDual)
		return SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
	return SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO;
}

/* ---------- rumble */

/* the vibration devices of the player's controller in its current style;
0 when it has none. Called with the lock */
static int rumble_devices(int player, struct rumble *rumble)
{
	u32 styles = padGetStyleSet(&pads[player]);
	HidNpadStyleTag style;
	HidNpadIdType id;

	if (styles & HidNpadStyleTag_NpadHandheld)
	{
		style = HidNpadStyleTag_NpadHandheld;
		id = HidNpadIdType_Handheld;
	}
	else
	{
		static const HidNpadStyleTag order[] = {HidNpadStyleTag_NpadFullKey, HidNpadStyleTag_NpadJoyDual,
			HidNpadStyleTag_NpadJoyLeft, HidNpadStyleTag_NpadJoyRight};
		unsigned index;

		style = 0;
		for (index = 0; index < sizeof(order) / sizeof(*order) && !style; index++)
		{
			if (styles & order[index])
				style = order[index];
		}
		if (!style)
			return 0;
		id = (HidNpadIdType)(HidNpadIdType_No1 + player);
	}
	if (rumble->style != (u32)style)
	{
		rumble->count = style == HidNpadStyleTag_NpadJoyLeft || style == HidNpadStyleTag_NpadJoyRight ? 1 : 2;
		if (R_FAILED(hidInitializeVibrationDevices(rumble->handles, rumble->count, id, style)))
		{
			rumble->count = 0;
			return 0;
		}
		rumble->style = (u32)style;
	}
	return rumble->count;
}

static void rumble_send(int player, uint32_t low, uint32_t high)
{
	struct rumble *rumble = &rumbles[player];
	HidVibrationValue values[2];
	int count, index;

	if (low == rumble->low && high == rumble->high)
		return;
	count = rumble_devices(player, rumble);
	if (!count)
		return;
	for (index = 0; index < count; index++)
	{
		values[index].amp_low = RUMBLE_STRENGTH * (float)low / 65535.0f;
		values[index].freq_low = RUMBLE_LOW_HZ;
		values[index].amp_high = RUMBLE_STRENGTH * (float)high / 65535.0f;
		values[index].freq_high = RUMBLE_HIGH_HZ;
	}
	if (R_SUCCEEDED(hidSendVibrationValues(rumble->handles, values, count)))
	{
		rumble->low = low;
		rumble->high = high;
	}
}

int host_sdl_rumble_gamepad(uint32_t gamepad, uint32_t low, uint32_t high, uint32_t milliseconds)
{
	(void)milliseconds; /* the game sends its state every frame */
	if (!pad_of(gamepad))
		return 0;
	mutexLock(&input_lock);
	rumble_send((int)gamepad - 1, low, high);
	mutexUnlock(&input_lock);
	return 1;
}

void host_input_stop_rumble(void)
{
	int player;

	mutexLock(&input_lock);
	for (player = 0; player < PLAYERS; player++)
	{
		if (rumbles[player].low || rumbles[player].high)
			rumble_send(player, 0, 0);
	}
	mutexUnlock(&input_lock);
}
