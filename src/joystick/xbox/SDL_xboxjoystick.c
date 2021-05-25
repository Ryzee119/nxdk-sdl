/*
  MIT License

  Copyright (c) 2019 Lucas Eriksson
  Copyright (c) 2021 Ryan Wendland

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/

#include "../../SDL_internal.h"

#if SDL_JOYSTICK_XBOX

#include "SDL_joystick.h"
#include "SDL_events.h"
#include "../SDL_joystick_c.h"
#include "../SDL_sysjoystick.h"

#include <SDL.h>
#include <assert.h>
#include <usbh_lib.h>
#include <xid_driver.h>

//#define SDL_JOYSTICK_XBOX_DEBUG
#ifdef SDL_JOYSTICK_XBOX_DEBUG
#include <hal/debug.h>
#define JOY_DBGMSG debugPrint
#else
#define JOY_DBGMSG(...)
#endif

#define MAX_JOYSTICKS CONFIG_XID_MAX_DEV
#define MAX_PACKET_SIZE 32
#define BUTTON_DEADZONE 0x20

//XINPUT defines and struct format from
//https://docs.microsoft.com/en-us/windows/win32/api/xinput/ns-xinput-xinput_gamepad
#define XINPUT_GAMEPAD_DPAD_UP 0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN 0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT 0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT 0x0008
#define XINPUT_GAMEPAD_START 0x0010
#define XINPUT_GAMEPAD_BACK 0x0020
#define XINPUT_GAMEPAD_LEFT_THUMB 0x0040
#define XINPUT_GAMEPAD_RIGHT_THUMB 0x0080
#define XINPUT_GAMEPAD_LEFT_SHOULDER 0x0100
#define XINPUT_GAMEPAD_RIGHT_SHOULDER 0x0200
#define XINPUT_GAMEPAD_A 0x1000
#define XINPUT_GAMEPAD_B 0x2000
#define XINPUT_GAMEPAD_X 0x4000
#define XINPUT_GAMEPAD_Y 0x8000
#define MAX_PACKET_SIZE 32

typedef struct _XINPUT_GAMEPAD
{
    Uint16 wButtons;
    Uint8 bLeftTrigger;
    Uint8 bRightTrigger;
    Sint16 sThumbLX;
    Sint16 sThumbLY;
    Sint16 sThumbRX;
    Sint16 sThumbRY;
} XINPUT_GAMEPAD, *PXINPUT_GAMEPAD;

//Struct linked to SDL_Joystick
typedef struct joystick_hwdata
{
    xid_dev_t *xid_dev;
    Uint8 raw_data[MAX_PACKET_SIZE];
    Uint16 current_rumble[2];
    Uint32 rumble_expiry;
} joystick_hwdata, *pjoystick_hwdata;

static Sint32 parse_input_data(xid_dev_t *xid_dev, PXINPUT_GAMEPAD controller, Uint8 *rdata);

//Create SDL events for connection/disconnection. These events can then be handled in the user application
static void connection_callback(xid_dev_t *xid_dev, int status) {
    JOY_DBGMSG("connection_callback: uid %i connected \n", xid_dev->uid);
    SDL_PrivateJoystickAdded(xid_dev->uid);
}

static void disconnect_callback(xid_dev_t *xid_dev, int status) {
    JOY_DBGMSG("disconnect_callback uid %i disconnected\n", xid_dev->uid);
    SDL_PrivateJoystickRemoved(xid_dev->uid);
}

static void int_read_callback(UTR_T *utr) {
    xid_dev_t *xid_dev = (xid_dev_t *)utr->context;

    if (utr->status < 0 || xid_dev == NULL || xid_dev->user_data == NULL)
    {
        return;
    }

    SDL_Joystick *joy = (SDL_Joystick *)xid_dev->user_data;

    //Cap data len to buffer size.
    Uint32 data_len = utr->xfer_len;
    if (data_len > MAX_PACKET_SIZE)
        data_len = MAX_PACKET_SIZE;

    if (joy->hwdata != NULL)
    {
        SDL_memcpy(joy->hwdata->raw_data, utr->buff, data_len);

        //Re-queue the USB transfer
        utr->xfer_len = 0;
        utr->bIsTransferDone = 0;
        usbh_int_xfer(utr);
    }
}

static xid_dev_t *xid_from_device_index(Sint32 device_index) {
    xid_dev_t *xid_dev = usbh_xid_get_device_list();

    Sint32 i = 0;
    //Scan the xid_dev linked list and finds the nth xid_dev that is a gamepad.
    while (xid_dev != NULL && i <= device_index)
    {
        //FIXME: Include xremote and steel battalion in the joystick API.
        if (xid_dev->xid_desc.bType == XID_TYPE_GAMECONTROLLER)
        {
            if (i == device_index)
                return xid_dev;
            i++;
        }
        xid_dev = xid_dev->next;
    }
    assert(0);
    return NULL;
}

static SDL_bool core_has_init = SDL_FALSE;
static Sint32 SDL_XBOX_JoystickInit(void) {
    if (!core_has_init)
    {
        usbh_core_init();
        usbh_xid_init();
        core_has_init = SDL_TRUE;
    }
    usbh_install_xid_conn_callback(connection_callback, disconnect_callback);

#ifndef SDL_DISABLE_JOYSTICK_INIT_DELAY
    //Ensure all connected devices have completed enumeration and are running
    //This wouldnt be required if user applications correctly handled connection events, but most dont
    //This needs to allow time for port reset, debounce, device reset etc. ~200ms per device. ~500ms is time for 1 hub + 1 controller.
    for (Sint32 i = 0; i < 500; i++)
    {
        usbh_pooling_hubs();
        SDL_Delay(1);
    }
#endif
    return 0;
}

static Sint32 SDL_XBOX_JoystickGetCount() {
    Sint32 pad_cnt = 0;
    xid_dev_t *xid_dev = usbh_xid_get_device_list();
    while (xid_dev != NULL)
    {
        //FIXME: Include xremote and steel battalion in the joystick API.
        if (xid_dev->xid_desc.bType == XID_TYPE_GAMECONTROLLER)
        {
            pad_cnt++;
        }
        xid_dev = xid_dev->next;
    }
    JOY_DBGMSG("SDL_XBOX_JoystickGetCount: Found %i pads\n", pad_cnt);
    return pad_cnt;
}

static void SDL_XBOX_JoystickDetect() {
    usbh_pooling_hubs();
}

static const char* SDL_XBOX_JoystickGetDeviceName(Sint32 device_index) {
    xid_dev_t *xid_dev = xid_from_device_index(device_index);

    if (xid_dev == NULL || device_index >= MAX_JOYSTICKS)
        return "Invalid device index";

    static char name[MAX_JOYSTICKS][64];
    Uint32 max_len = sizeof(name[device_index]);

    //FIXME. See SDL_XBOX_JoystickGetDevicePlayerIndex().
    Sint32 player_index = device_index;
    switch (xid_dev->xid_desc.bType)
    {
    case XID_TYPE_GAMECONTROLLER:
        SDL_snprintf(name[device_index], max_len, "Original Xbox Controller #%u", player_index + 1);
        break;
    case XID_TYPE_XREMOTE:
        SDL_snprintf(name[device_index], max_len, "Original Xbox IR Remote #%u", player_index + 1);
        break;
    case XID_TYPE_STEELBATTALION:
        SDL_snprintf(name[device_index], max_len, "Steel Battalion Controller #%u", player_index + 1);
        break;

    }

    return name[device_index];
}

//FIXME
//Player index is just the order the controllers were plugged in.
//This may not be what the user expects on a Xbox console.
//Player index should consider that Port 1 = player 1, Port 2 = player 2 etc.
static Sint32 SDL_XBOX_JoystickGetDevicePlayerIndex(Sint32 device_index) {
    xid_dev_t *xid_dev = xid_from_device_index(device_index);

    if (xid_dev == NULL)
        return -1;

    Sint32 player_index = device_index;
    JOY_DBGMSG("SDL_XBOX_JoystickGetDevicePlayerIndex: %i\n", player_index);

    return player_index;
}

static SDL_JoystickGUID SDL_XBOX_JoystickGetDeviceGUID(Sint32 device_index) {
    xid_dev_t *xid_dev = xid_from_device_index(device_index);

    SDL_JoystickGUID ret;
    SDL_zero(ret);

    if (xid_dev != NULL)
    {
        //Format based on SDL_gamecontrollerdb.h
        ret.data[0] = 0x03;
        ret.data[4] = xid_dev->idVendor & 0xFF;
        ret.data[5] = (xid_dev->idVendor >> 8) & 0xFF;
        ret.data[8] = xid_dev->idProduct & 0xFF;
        ret.data[9] = (xid_dev->idProduct >> 8) & 0xFF;
    }
    return ret;
}

static SDL_JoystickID SDL_XBOX_JoystickGetDeviceInstanceID(Sint32 device_index) {
    xid_dev_t *xid_dev = xid_from_device_index(device_index);

    SDL_JoystickID ret;
    SDL_zero(ret);

    if (xid_dev != NULL)
    {
        SDL_memcpy(&ret, &xid_dev->uid, sizeof(xid_dev->uid));
    }
    JOY_DBGMSG("SDL_XBOX_JoystickGetDeviceInstanceID: %i\n", xid_dev->uid);
    return ret;
}

static Sint32 SDL_XBOX_JoystickOpen(SDL_Joystick *joystick, Sint32 device_index) {
    xid_dev_t *xid_dev = xid_from_device_index(device_index);

    if (xid_dev == NULL)
    {
        JOY_DBGMSG("SDL_XBOX_JoystickOpen: Could not find device index %i\n", device_index);
        return -1;
    }

    joystick->hwdata = (pjoystick_hwdata)SDL_malloc(sizeof(joystick_hwdata));
    assert(joystick->hwdata != NULL);
    SDL_zerop(joystick->hwdata);

    joystick->hwdata->xid_dev = xid_dev;
    joystick->hwdata->xid_dev->user_data = (void *)joystick;
    joystick->player_index = SDL_XBOX_JoystickGetDevicePlayerIndex(device_index);
    joystick->guid = SDL_XBOX_JoystickGetDeviceGUID(device_index);

    switch (xid_dev->xid_desc.bType)
    {
    case XID_TYPE_GAMECONTROLLER:
        joystick->naxes = 6;     /* LStickY, LStickX, LTrigg, RStickY, RStickX, RTrigg */
        joystick->nballs = 0;    /* No balls here */
        joystick->nhats = 1;     /* D-pad */
        joystick->nbuttons = 10; /* A, B, X, Y, RB, LB, Back, Start, LThumb, RThumb */
        break;
    case XID_TYPE_XREMOTE:
        joystick->naxes = 0;
        joystick->nballs = 0;
        joystick->nhats = 0;
        joystick->nbuttons = 27;
        break;
    case XID_TYPE_STEELBATTALION:
        joystick->naxes = 10; //Tuner dial and gear level are treated like an axis
        joystick->nballs = 0;
        joystick->nhats = 0;
        joystick->nbuttons = 39; //This includes the toggle switches
        break;
    default:
        SDL_free(joystick->hwdata);
        joystick->hwdata = NULL;
        return -1;
    }

    JOY_DBGMSG("JoystickOpened:\n");
    JOY_DBGMSG("joystick device_index: %i\n", device_index);
    JOY_DBGMSG("joystick player_index: %i\n", joystick->player_index);
    JOY_DBGMSG("joystick uid: %i\n", xid_dev->uid);
    JOY_DBGMSG("joystick name: %s\n", SDL_XBOX_JoystickGetDeviceName(device_index));

    //Start reading interrupt pipe
    usbh_xid_read(xid_dev, 0, int_read_callback);

    return 0;
}

static Sint32 SDL_XBOX_JoystickRumble(SDL_Joystick *joystick,
                                      Uint16 low_frequency_rumble,
                                      Uint16 high_frequency_rumble,
                                      Uint32 duration_ms) {

    //Check if rumble values are new values.
    if (joystick->hwdata->current_rumble[0] == low_frequency_rumble &&
        joystick->hwdata->current_rumble[1] == high_frequency_rumble)
    {
        //Rumble values not changed, reset the expiry timer and leave.
        joystick->hwdata->rumble_expiry = SDL_GetTicks() + duration_ms;
        return 0;
    }

    if (usbh_xid_rumble(joystick->hwdata->xid_dev, low_frequency_rumble, high_frequency_rumble) != USBH_OK)
    {
        return -1;
    }

    joystick->hwdata->current_rumble[0] = low_frequency_rumble;
    joystick->hwdata->current_rumble[1] = high_frequency_rumble;
    joystick->hwdata->rumble_expiry = SDL_GetTicks() + duration_ms;
    return 0;
}

static void SDL_XBOX_JoystickUpdate(SDL_Joystick *joystick) {
    Sint16 wButtons, axis, this_joy;
    Sint32 hat = SDL_HAT_CENTERED;
    XINPUT_GAMEPAD xpad;

    if (joystick == NULL || joystick->hwdata == NULL || joystick->hwdata->xid_dev == NULL)
    {
        return;
    }

    //Check if the rumble timer has expired.
    if (joystick->hwdata->rumble_expiry && SDL_GetTicks() > joystick->hwdata->rumble_expiry)
    {
        usbh_xid_rumble(joystick->hwdata->xid_dev, 0, 0);
        joystick->hwdata->rumble_expiry = 0;
        joystick->hwdata->current_rumble[0] = 0;
        joystick->hwdata->current_rumble[1] = 0;
    }

    Uint8 button_data[MAX_PACKET_SIZE];
    SDL_memcpy(button_data, joystick->hwdata->raw_data, MAX_PACKET_SIZE);

    //FIXME. Steel Battalion and XREMOTE should be parsed differently.
    if (parse_input_data(joystick->hwdata->xid_dev, &xpad, button_data))
    {
        wButtons = xpad.wButtons;

        //HAT
        if (wButtons & XINPUT_GAMEPAD_DPAD_UP)    hat |= SDL_HAT_UP;
        if (wButtons & XINPUT_GAMEPAD_DPAD_DOWN)  hat |= SDL_HAT_DOWN;
        if (wButtons & XINPUT_GAMEPAD_DPAD_LEFT)  hat |= SDL_HAT_LEFT;
        if (wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) hat |= SDL_HAT_RIGHT;
        if (hat != joystick->hats[0]) {
            SDL_PrivateJoystickHat(joystick, 0, hat);
        }

        //DIGITAL BUTTONS
        static const Sint32 btn_map[10][2] = 
        {
          {0, XINPUT_GAMEPAD_A},
          {1, XINPUT_GAMEPAD_B},
          {2, XINPUT_GAMEPAD_X},
          {3, XINPUT_GAMEPAD_Y},
          {4, XINPUT_GAMEPAD_LEFT_SHOULDER},
          {5, XINPUT_GAMEPAD_RIGHT_SHOULDER},
          {6, XINPUT_GAMEPAD_BACK},
          {7, XINPUT_GAMEPAD_START},
          {8, XINPUT_GAMEPAD_LEFT_THUMB},
          {9, XINPUT_GAMEPAD_RIGHT_THUMB}
        };
        for (Sint32 i = 0; i < (sizeof(btn_map) / sizeof(btn_map[0])); i++)
        {
          if (joystick->buttons[btn_map[i][0]] != ((wButtons & btn_map[i][1]) > 0))
              SDL_PrivateJoystickButton(joystick, btn_map[i][0], (wButtons & btn_map[i][1]) ? SDL_PRESSED : SDL_RELEASED);
        }

        //TRIGGERS
        //LEFT TRIGGER (0-255 must be converted to signed short)
        if (xpad.bLeftTrigger != joystick->axes[2].value)
            SDL_PrivateJoystickAxis(joystick, 2, ((xpad.bLeftTrigger << 8) | xpad.bLeftTrigger) - (1 << 15));
        //RIGHT TRIGGER (0-255 must be converted to signed short)
        if (xpad.bRightTrigger != joystick->axes[5].value)
            SDL_PrivateJoystickAxis(joystick, 5, ((xpad.bRightTrigger << 8) | xpad.bRightTrigger) - (1 << 15));

        //ANALOG STICKS
        //LEFT X-AXIS
        axis = xpad.sThumbLX;
        if (axis != joystick->axes[0].value)
            SDL_PrivateJoystickAxis(joystick, 0, axis);
        //LEFT Y-AXIS
        axis = xpad.sThumbLY;
        if (axis != joystick->axes[1].value)
            SDL_PrivateJoystickAxis(joystick, 1, ~axis);
        //RIGHT X-AXIS
        axis = xpad.sThumbRX;
        if (axis != joystick->axes[3].value)
            SDL_PrivateJoystickAxis(joystick, 3, axis);
        //RIGHT Y-AXIS
        axis = xpad.sThumbRY;
        if (axis != joystick->axes[4].value)
            SDL_PrivateJoystickAxis(joystick, 4, ~axis);
    }
    return;
}

static void SDL_XBOX_JoystickClose(SDL_Joystick *joystick) {
    JOY_DBGMSG("SDL_XBOX_JoystickClose:\n");
    if (joystick->hwdata == NULL)
        return;

    usbh_xid_rumble(joystick->hwdata->xid_dev, 0, 0);

    xid_dev_t *xid_dev = joystick->hwdata->xid_dev;
    xid_dev->user_data = NULL;
    if (xid_dev != NULL)
    {
        JOY_DBGMSG("Closing joystick:\n", joystick->hwdata->xid_dev->uid);
        JOY_DBGMSG("joystick player_index: %i\n", joystick->player_index);
    }
    SDL_free(joystick->hwdata);
    joystick->hwdata = NULL;
    return;
}

static void SDL_XBOX_JoystickQuit(void) {
    JOY_DBGMSG("SDL_XBOX_JoystickQuit\n");
    usbh_install_xid_conn_callback(NULL, NULL);
    //We dont call usbh_core_deinit() here incase the user is using
    //the USB stack in other parts of their application other than game controllers.
}

SDL_JoystickDriver SDL_XBOX_JoystickDriver = {
    SDL_XBOX_JoystickInit,
    SDL_XBOX_JoystickGetCount,
    SDL_XBOX_JoystickDetect,
    SDL_XBOX_JoystickGetDeviceName,
    SDL_XBOX_JoystickGetDevicePlayerIndex,
    SDL_XBOX_JoystickGetDeviceGUID,
    SDL_XBOX_JoystickGetDeviceInstanceID,
    SDL_XBOX_JoystickOpen,
    SDL_XBOX_JoystickRumble,
    SDL_XBOX_JoystickUpdate,
    SDL_XBOX_JoystickClose,
    SDL_XBOX_JoystickQuit
};

static Sint32 parse_input_data(xid_dev_t *xid_dev, PXINPUT_GAMEPAD controller, Uint8 *rdata) {

    if (xid_dev == NULL)
    {
        return 0;
    }

    Uint16 wButtons = *((Uint16*)&rdata[2]);
    controller->wButtons = 0;

    //Map digital buttons
    if (wButtons & (1 << 0)) controller->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
    if (wButtons & (1 << 1)) controller->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (wButtons & (1 << 2)) controller->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (wButtons & (1 << 3)) controller->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
    if (wButtons & (1 << 4)) controller->wButtons |= XINPUT_GAMEPAD_START;
    if (wButtons & (1 << 5)) controller->wButtons |= XINPUT_GAMEPAD_BACK;
    if (wButtons & (1 << 6)) controller->wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (wButtons & (1 << 7)) controller->wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;

    //Analog buttons are converted to digital
    if (rdata[4] > BUTTON_DEADZONE) controller->wButtons |= XINPUT_GAMEPAD_A;
    if (rdata[5] > BUTTON_DEADZONE) controller->wButtons |= XINPUT_GAMEPAD_B;
    if (rdata[6] > BUTTON_DEADZONE) controller->wButtons |= XINPUT_GAMEPAD_X;
    if (rdata[7] > BUTTON_DEADZONE) controller->wButtons |= XINPUT_GAMEPAD_Y;
    if (rdata[8] > BUTTON_DEADZONE) controller->wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER; //BLACK
    if (rdata[9] > BUTTON_DEADZONE) controller->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER; //WHITE

    //Map the left and right triggers
    controller->bLeftTrigger = rdata[10];
    controller->bRightTrigger = rdata[11];

    //Map analog sticks
    controller->sThumbLX = *((Sint16 *)&rdata[12]);
    controller->sThumbLY = *((Sint16 *)&rdata[14]);
    controller->sThumbRX = *((Sint16 *)&rdata[16]);
    controller->sThumbRY = *((Sint16 *)&rdata[18]);
    return 1;
}

#endif /* SDL_JOYSTICK_XBOX */
