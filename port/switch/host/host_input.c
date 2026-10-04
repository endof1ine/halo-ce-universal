/*
HOST_INPUT.C

The guest's SDL3 gamepads (xinput_sdl.c reads them as the Xbox's), on the
Switch's controllers: up to four players, the first one also the console's
attached Joy-Con (handheld). A gamepad's SDL id is its player number,
1 to 4.

Buttons go by their labels, as the game's prompts read: the Switch's A is
the Xbox's A (jump), B is B, X is X and Y is Y. input.button_positions =
true in config.toml maps them by position instead, as the other ports do
(the bottom face button the Xbox's A). ZL and ZR, which are buttons here,
are the triggers at full travel.

Rumble: the Xbox's low-frequency (heavy) motor plays in HD rumble's low
band and its high-frequency one in the high band.

Held for a second on player 1's controller: + and - open the system's
controller setup (players 2 to 4 each with a pair of Joy-Con or a Pro
Controller: the game needs two sticks, so not one Joy-Con each); - and X
the keyboard for an internet game's invite link (host_sdl.c).

The touchscreen (handheld): a finger lifted is a tap where it was, which the
menus take as a click (d3d8_gl.c's halo_ui_pointer_update).

Gyro aiming (input.gyro_aim = true in config.toml; off unless set): player
1's controller turned turns the view as much (input.gyro_sensitivity, 1 by
default; input.gyro_invert_x and _y), through the platform layer's mouse
look (xinput_sdl.c), so the stick's acceleration does not apply to it. The
stick aims as well. Small turns are smoothed over a few polls, which steadies
a Joy-Con's jitter while aiming finely; larger ones go through at once.
*/

#include "host.h"

#include <SDL3/SDL_gamepad.h>
#include <math.h>
#include <string.h>
#include <switch.h>

#define PLAYERS 4
#define RUMBLE_LOW_HZ 160.0f
#define RUMBLE_HIGH_HZ 320.0f
/* (the Xbox's motors at full strength are much gentler than HD rumble's) */
#define RUMBLE_STRENGTH 0.35f
/* the stick's travel counted as all of it: the Switch's sticks, a Joy-Con's
most, stop short of their full range, and the game turns fastest only at
full deflection */
#define STICK_FULL 0.88f

static PadState pads[PLAYERS];
static int by_position;

/* gyro aiming: the radians of the platform layer's mouse look per unit of
motion (xinput_sdl.c), and below what the controller is held still */
#define MOUSE_LOOK_RADIANS 0.0022f
#define GYRO_DEADZONE 0.004f
#define GYRO_STYLES 3
/* turns a second below which a turn is all smoothed, above which none of it */
#define GYRO_SMOOTH_BELOW 0.01f
#define GYRO_SMOOTH_ABOVE 0.04f
#define GYRO_SMOOTH_SAMPLES 4

static struct
{
	int enabled;
	float sensitivity_x, sensitivity_y;
	/* player 1's sensors in each style: Pro, the pair of Joy-Con (the
	right one), attached to the console (the right one) */
	HidSixAxisSensorHandle handles[GYRO_STYLES][2];
	int started[GYRO_STYLES];
	uint64_t last_tick;
	float recent_yaw[GYRO_SMOOTH_SAMPLES], recent_pitch[GYRO_SMOOTH_SAMPLES];
	unsigned recent;
} gyro;

/* soft smoothing: a small turn rate is mostly the average of the last few,
a large one itself */
static float gyro_smooth(float value, float *recent)
{
	float average = 0.0f, magnitude = fabsf(value), direct;
	unsigned index;

	recent[gyro.recent % GYRO_SMOOTH_SAMPLES] = value;
	for (index = 0; index < GYRO_SMOOTH_SAMPLES; index++)
		average += recent[index];
	average /= GYRO_SMOOTH_SAMPLES;
	direct = (magnitude - GYRO_SMOOTH_BELOW) / (GYRO_SMOOTH_ABOVE - GYRO_SMOOTH_BELOW);
	direct = direct < 0.0f ? 0.0f : direct > 1.0f ? 1.0f : direct;
	return direct * value + (1.0f - direct) * average;
}
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

/* the buttons held together for a second that open a system screen */
#define GESTURE_NANOSECONDS 1000000000ull

enum
{
	_gesture_none,
	_gesture_controllers,
	_gesture_invite,
};

static struct
{
	int held;
	uint64_t since;
	int fired;
} gesture;

static u64 gesture_buttons(int held)
{
	return held == _gesture_controllers ? HidNpadButton_Plus | HidNpadButton_Minus :
		held == _gesture_invite ? HidNpadButton_Minus | HidNpadButton_X : 0;
}

/* the system's controller setup: who plays as which player */
static void controller_setup(void)
{
	HidLaControllerSupportArg arg;
	HidLaControllerSupportResultInfo result;

	hidLaCreateControllerSupportArg(&arg);
	arg.hdr.player_count_min = 1;
	arg.hdr.player_count_max = PLAYERS;
	arg.hdr.enable_permit_joy_dual = 1;
	host_input_stop_rumble();
	if (R_SUCCEEDED(hidLaShowControllerSupport(&result, &arg)))
		host_logf(HOST_LOG_INFO, "controller setup: %d players", (int)result.player_count);
}

/* the buttons player 1 holds together, and after a second the screen they
open (once until they are let go). On the game's thread, between frames:
the system's screens stop it while they are up */
static void gesture_update(void)
{
	u64 buttons = pad_connected[0] ? padGetButtons(&pads[0]) : 0;
	int held = _gesture_none;
	uint64_t now = armTicksToNs(armGetSystemTick());

	if ((buttons & (HidNpadButton_Plus | HidNpadButton_Minus)) == (HidNpadButton_Plus | HidNpadButton_Minus))
		held = _gesture_controllers;
	else if ((buttons & (HidNpadButton_Minus | HidNpadButton_X)) == (HidNpadButton_Minus | HidNpadButton_X))
		held = _gesture_invite;
	if (held != gesture.held)
	{
		gesture.held = held;
		gesture.since = now;
		gesture.fired = 0;
		return;
	}
	if (!held || gesture.fired || now - gesture.since < GESTURE_NANOSECONDS)
		return;
	gesture.fired = 1;
	if (held == _gesture_controllers)
		controller_setup();
	else
		host_sdl_invite_keyboard();
}

void host_input_settings_read(void)
{
	int positions = host_config_boolean("input.button_positions");
	int enabled = host_config_boolean("input.gyro_aim");
	float sensitivity = (float)host_config_real("input.gyro_sensitivity", 1.0);
	int invert_x = host_config_boolean("input.gyro_invert_x"), invert_y = host_config_boolean("input.gyro_invert_y");

	if (sensitivity <= 0.0f)
		sensitivity = 1.0f;
	mutexLock(&input_lock);
	by_position = positions;
	gyro.enabled = enabled;
	gyro.sensitivity_x = invert_x ? -sensitivity : sensitivity;
	gyro.sensitivity_y = invert_y ? -sensitivity : sensitivity;
	mutexUnlock(&input_lock);
	host_logf(HOST_LOG_INFO, "buttons by %s, gyro aiming %s (sensitivity %.2f)", positions ? "position" : "label",
		enabled ? "on" : "off", sensitivity);
}

void host_input_initialize(void)
{
	int player;

	mutexInit(&input_lock);
	hidInitializeTouchScreen();
	padConfigureInput(PLAYERS, HidNpadStyleSet_NpadStandard);
	padInitialize(&pads[0], HidNpadIdType_No1, HidNpadIdType_Handheld);
	for (player = 1; player < PLAYERS; player++)
		padInitialize(&pads[player], (HidNpadIdType)(HidNpadIdType_No1 + player));
	host_input_settings_read();
}

/* the sensor of player 1's controller in its style, started the first time;
NULL when it has none */
static const HidSixAxisSensorHandle *gyro_sensor(void)
{
	u32 styles = padGetStyleSet(&pads[0]);
	int style, count = 1, use = 0;
	HidNpadIdType id = HidNpadIdType_No1;
	HidNpadStyleTag tag;

	if (styles & HidNpadStyleTag_NpadHandheld)
	{
		style = 2;
		tag = HidNpadStyleTag_NpadHandheld;
		id = HidNpadIdType_Handheld;
		count = 2;
		use = 1;
	}
	else if (styles & HidNpadStyleTag_NpadJoyDual)
	{
		style = 1;
		tag = HidNpadStyleTag_NpadJoyDual;
		count = 2;
		use = 1;
	}
	else if (styles & HidNpadStyleTag_NpadFullKey)
	{
		style = 0;
		tag = HidNpadStyleTag_NpadFullKey;
	}
	else
	{
		return NULL;
	}
	if (!gyro.started[style])
	{
		int index;

		if (R_FAILED(hidGetSixAxisSensorHandles(gyro.handles[style], count, id, tag)))
			return NULL;
		for (index = 0; index < count; index++)
			hidStartSixAxisSensor(gyro.handles[style][index]);
		gyro.started[style] = 1;
	}
	return &gyro.handles[style][use];
}

/* the view's turn since the last poll, as relative mouse motion */
static void gyro_update(void)
{
	const HidSixAxisSensorHandle *sensor;
	HidSixAxisSensorState state;
	uint64_t tick = armGetSystemTick();
	float seconds, yaw, pitch;

	if (!gyro.enabled || !pad_connected[0])
		return;
	seconds = gyro.last_tick ? (float)armTicksToNs(tick - gyro.last_tick) / 1e9f : 0.0f;
	gyro.last_tick = tick;
	/* (after a pause - a menu, HOME - the controller's turn is not the view's,
	nor are the turn rates from before it to smooth with) */
	if (seconds > 0.1f)
	{
		memset(gyro.recent_yaw, 0, sizeof(gyro.recent_yaw));
		memset(gyro.recent_pitch, 0, sizeof(gyro.recent_pitch));
	}
	if (seconds <= 0.0f || seconds > 0.1f)
		return;
	sensor = gyro_sensor();
	if (!sensor || !hidGetSixAxisSensorStates(*sensor, &state, 1))
		return;
	/* angular velocity in turns a second: z is about the controller's
	vertical axis (yaw), x across it (pitch) */
	yaw = fabsf(state.angular_velocity.z) < GYRO_DEADZONE ? 0.0f : state.angular_velocity.z;
	pitch = fabsf(state.angular_velocity.x) < GYRO_DEADZONE ? 0.0f : state.angular_velocity.x;
	yaw = gyro_smooth(yaw, gyro.recent_yaw);
	pitch = gyro_smooth(pitch, gyro.recent_pitch);
	gyro.recent++;
	if (yaw == 0.0f && pitch == 0.0f)
		return;
	host_sdl_queue_mouse_motion(-yaw * 6.2831853f * seconds * gyro.sensitivity_x / MOUSE_LOOK_RADIANS,
		-pitch * 6.2831853f * seconds * gyro.sensitivity_y / MOUSE_LOOK_RADIANS);
}

/* the touchscreen's taps since they were last read, under input_lock */
static struct
{
	int down, x, y;
	int taps, tap_x, tap_y;
} touch;

static void touch_update(void)
{
	HidTouchScreenState state;

	if (!hidGetTouchScreenStates(&state, 1))
		return;
	if (state.count > 0)
	{
		touch.down = 1;
		touch.x = (int)state.touches[0].x;
		touch.y = (int)state.touches[0].y;
	}
	else if (touch.down)
	{
		touch.down = 0;
		touch.taps++;
		touch.tap_x = touch.x;
		touch.tap_y = touch.y;
	}
}

/* the taps since the last call, and where the last one was (the
touchscreen's 1280x720) */
int host_touch_taps(int *x, int *y)
{
	int taps;

	mutexLock(&input_lock);
	taps = touch.taps;
	*x = touch.tap_x;
	*y = touch.tap_y;
	touch.taps = 0;
	mutexUnlock(&input_lock);
	return taps;
}

void host_input_update(void)
{
	int player;

	mutexLock(&input_lock);
	touch_update();
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
	gyro_update();
	mutexUnlock(&input_lock);
	gesture_update();
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
	/* (SDL's south is the Xbox's A, east B, west X, north Y) */
	case SDL_GAMEPAD_BUTTON_SOUTH: mask = by_position ? HidNpadButton_B : HidNpadButton_A; break;
	case SDL_GAMEPAD_BUTTON_EAST: mask = by_position ? HidNpadButton_A : HidNpadButton_B; break;
	case SDL_GAMEPAD_BUTTON_WEST: mask = by_position ? HidNpadButton_Y : HidNpadButton_X; break;
	case SDL_GAMEPAD_BUTTON_NORTH: mask = by_position ? HidNpadButton_X : HidNpadButton_Y; break;
	case SDL_GAMEPAD_BUTTON_BACK: mask = HidNpadButton_Minus; break;
	case SDL_GAMEPAD_BUTTON_START: mask = HidNpadButton_Plus; break;
	case SDL_GAMEPAD_BUTTON_LEFT_STICK: mask = HidNpadButton_StickL; break;
	case SDL_GAMEPAD_BUTTON_RIGHT_STICK: mask = HidNpadButton_StickR; break;
	/* (the Xbox's black button, the grenades', on L beside ZL, which throws
	one; its white button, the flashlight, on R: xinput_sdl.c reads SDL's
	left shoulder as white) */
	case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: mask = HidNpadButton_R; break;
	case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: mask = HidNpadButton_L; break;
	case SDL_GAMEPAD_BUTTON_DPAD_UP: mask = HidNpadButton_Up; break;
	case SDL_GAMEPAD_BUTTON_DPAD_DOWN: mask = HidNpadButton_Down; break;
	case SDL_GAMEPAD_BUTTON_DPAD_LEFT: mask = HidNpadButton_Left; break;
	case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: mask = HidNpadButton_Right; break;
	default: return 0;
	}
	/* (the buttons of a screen being opened are not the game's) */
	if (gamepad == 1 && gesture.held)
		held &= ~gesture_buttons(gesture.held);
	return (held & mask) != 0;
}

static int clamp_axis(int value)
{
	return value < -32768 ? -32768 : value > 32767 ? 32767 : value;
}

/* a stick's position (SDL's y downwards), STICK_FULL of its travel (by its
distance from the centre, so diagonals alike) being all of it */
static void stick_position(PadState *pad, int stick, int *x, int *y)
{
	HidAnalogStickState position = padGetStickPos(pad, (unsigned int)stick);
	float fx = (float)position.x / STICK_FULL, fy = (float)-position.y / STICK_FULL;
	float length = sqrtf(fx * fx + fy * fy);

	if (length > 32767.0f)
	{
		fx *= 32767.0f / length;
		fy *= 32767.0f / length;
	}
	*x = clamp_axis((int)fx);
	*y = clamp_axis((int)fy);
}

int host_sdl_gamepad_axis(uint32_t gamepad, int axis)
{
	PadState *pad = pad_of(gamepad);
	int x, y;

	if (!pad)
		return 0;
	switch (axis)
	{
	case SDL_GAMEPAD_AXIS_LEFTX:
	case SDL_GAMEPAD_AXIS_LEFTY:
		stick_position(pad, 0, &x, &y);
		return axis == SDL_GAMEPAD_AXIS_LEFTX ? x : y;
	case SDL_GAMEPAD_AXIS_RIGHTX:
	case SDL_GAMEPAD_AXIS_RIGHTY:
		stick_position(pad, 1, &x, &y);
		return axis == SDL_GAMEPAD_AXIS_RIGHTX ? x : y;
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
		/* (the new devices are still) */
		rumble->low = rumble->high = 0;
	}
	return rumble->count;
}

static void rumble_send(int player, uint32_t low, uint32_t high)
{
	struct rumble *rumble = &rumbles[player];
	HidVibrationValue values[2];
	int count, index;

	count = rumble_devices(player, rumble);
	if (!count || (low == rumble->low && high == rumble->high))
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
