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
#include <usbh_hid.h>


//#define SDL_JOYSTICK_XBOX_DEBUG
#ifdef SDL_JOYSTICK_XBOX_DEBUG
#define JOY_DBGMSG debugPrint
#else
#define JOY_DBGMSG(...)
#endif

#define MAX_JOYSTICKS CONFIG_HID_MAX_DEV

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
    HID_DEV_T *hdev;
    Uint8 raw_data[MAX_PACKET_SIZE];
    Uint16 current_rumble [2];
    Uint32 rumble_expiry;
} joystick_hwdata, *pjoystick_hwdata;

static Sint32 xboxjoy_parse_input_data(HID_DEV_T *hdev, PXINPUT_GAMEPAD controller, Uint8 *rdata);

//Create SDL events for connection/disconnection. These events can then be handled in the user application
static void xboxjoy_connection_callback(HID_DEV_T *hdev, int status) {
    JOY_DBGMSG("xboxjoy_connection_callback: uid %i connected \n", hdev->uid);
    SDL_PrivateJoystickAdded(hdev->uid);
}

static void xboxjoy_disconnect_callback(HID_DEV_T *hdev, int status) {
    JOY_DBGMSG("xboxjoy_disconnect_callback uid %i disconnected\n", hdev->uid);
    SDL_PrivateJoystickRemoved(hdev->uid);
}

static void xboxjoy_int_write_callback(UTR_T *utr) {
    JOY_DBGMSG("usbh_transfer done\n");
}

static void xboxjoy_int_read_callback(HID_DEV_T *hdev, Uint16 ep_addr, Sint32 status, Uint8 *rdata, Uint32 data_len) {
    if (status < 0 || hdev == NULL || hdev->user_data == NULL)
        return;

    //Check if the incoming data is actually a button report. Otherwise just exit
    switch (hdev->type)
    {
    case XBOXOG_CONTROLLER:
    case XBOX360_WIRED:
        //Check the packet length is atleast the expected amount.
        if (rdata[1] < 0x14) return;
        break;
    case XBOX360_WIRELESS:
        if ((rdata[1] & 0x01) == 0 || rdata[5] != 0x13) return;
        break;
    case XBOXONE:
        if (rdata[0] != 0x20) return;
        break;
    default:
        return;
    }

    SDL_Joystick *joy = (SDL_Joystick *)hdev->user_data;

    //Cap data len to buffer size.
    if (data_len > MAX_PACKET_SIZE)
        data_len = MAX_PACKET_SIZE;

    SDL_memcpy(joy->hwdata->raw_data, rdata, data_len);
}

static Sint32 xboxjoy_rumble(HID_DEV_T *hdev,
                             Uint16 low_frequency_rumble,
                             Uint16 high_frequency_rumble) {

    //Rumble commands for known controllers
    static const Uint8 xbox360_wireless[] = {0x00, 0x01, 0x0F, 0xC0, 0x00, 0x00, 0x00, 0x00};
    static const Uint8 xbox360_wired[] = {0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    static const Uint8 xbox_og[] = {0x00, 0x06, 0x00, 0x00, 0x00, 0x00};
    static const Uint8 xbox_one[] = {0x09, 0x00, 0x00, 0x09, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xEB};

    Uint8 writeBuf[MAX_PACKET_SIZE];
    Sint32 ret, len;
    switch (hdev->type)
    {
    case XBOX360_WIRELESS:
        SDL_memcpy(writeBuf, xbox360_wireless, sizeof(xbox360_wireless));
        writeBuf[5] = low_frequency_rumble >> 8;
        writeBuf[6] = high_frequency_rumble >> 8;
        len = sizeof(xbox360_wireless);
        break;
    case XBOX360_WIRED:
        SDL_memcpy(writeBuf, xbox360_wired, sizeof(xbox360_wired));
        writeBuf[3] = low_frequency_rumble >> 8;
        writeBuf[4] = high_frequency_rumble >> 8;
        len = sizeof(xbox360_wired);
        break;
    case XBOXOG_CONTROLLER:
        SDL_memcpy(writeBuf, xbox_og, sizeof(xbox_og));
        writeBuf[2] = low_frequency_rumble & 0xFF;
        writeBuf[3] = low_frequency_rumble >> 8;
        writeBuf[4] = high_frequency_rumble & 0xFF;
        writeBuf[5] = high_frequency_rumble >> 8;
        len = sizeof(xbox_og);
        break;
    case XBOXONE:
        SDL_memcpy(writeBuf, xbox_one, sizeof(xbox_one));
        writeBuf[8] = low_frequency_rumble / 655; //Scale is 0 to 100
        writeBuf[9] = high_frequency_rumble / 655; //Scale is 0 to 100
        len = sizeof(xbox_one);
        break;
    default:
        return -1;
    }

    ret = usbh_hid_int_write(hdev, 0, writeBuf, len, xboxjoy_int_write_callback);

    if (ret != HID_RET_OK)
    {
        return -1;
    }

    return 0;
}

static HID_DEV_T *hdev_from_device_index(Sint32 device_index) {
    HID_DEV_T *hdev = usbh_hid_get_device_list();

    Sint32 i = 0;
    //Scan the hdev linked list and finds the nth hdev that is a gamepad.
    while (hdev != NULL && i <= device_index)
    {
        if (hdev->type == XBOXOG_CONTROLLER || hdev->type == XBOXONE ||
            hdev->type == XBOX360_WIRED || hdev->type == XBOX360_WIRELESS)
        {
            if (i == device_index)
                return hdev;
            i++;
        }
        hdev = hdev->next;
    }
    assert(0);
    return hdev;
}

static SDL_bool core_has_init = SDL_FALSE;
static Sint32 SDL_XBOX_JoystickInit(void) {
    if (!core_has_init)
    {
        usbh_core_init();
        usbh_hid_init();
        core_has_init = SDL_TRUE;
    }
    usbh_install_hid_conn_callback(xboxjoy_connection_callback, xboxjoy_disconnect_callback);

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
    HID_DEV_T *hdev = usbh_hid_get_device_list();
    while (hdev != NULL)
    {
        if (hdev->type == XBOXOG_CONTROLLER || hdev->type == XBOXONE ||
            hdev->type == XBOX360_WIRED || hdev->type == XBOX360_WIRELESS)
        {
            pad_cnt++;
        }
        hdev = hdev->next;
    }
    JOY_DBGMSG("SDL_XBOX_JoystickGetCount: Found %i pads\n", pad_cnt);
    return pad_cnt;
}

static void SDL_XBOX_JoystickDetect() {
    usbh_pooling_hubs();
}

static const char* SDL_XBOX_JoystickGetDeviceName(Sint32 device_index) {
    HID_DEV_T *hdev = hdev_from_device_index(device_index);

    if (hdev == NULL || device_index >= MAX_JOYSTICKS)
        return "Invalid device index";

    static char name[MAX_JOYSTICKS][32];
    Uint32 max_len = sizeof(name[device_index]);
    Sint32 player_index = device_index;

    switch (hdev->type)
    {
    case XBOXOG_CONTROLLER:
        SDL_snprintf(name[device_index], max_len, "Original Xbox Controller #%u", player_index + 1);
        break;
    case XBOX360_WIRED:
    case XBOX360_WIRELESS:
        SDL_snprintf(name[device_index], max_len, "Xbox 360 Controller #%u", player_index + 1);
        break;
    case XBOXONE:
        SDL_snprintf(name[device_index], max_len, "Xbox One Controller #%u", player_index + 1);
        break;
    default:
        SDL_snprintf(name[device_index], max_len, "Unknown Controller #%u", player_index + 1);
    }
    return name[device_index];
}

//FIXME
//Player index is just the order the controllers were plugged in.
//This may not be what the user expects on a Xbox console.
//Player index should consider that Port 1 = player 1, Port 2 = player 2 etc.
static Sint32 SDL_XBOX_JoystickGetDevicePlayerIndex(Sint32 device_index) {
    HID_DEV_T *hdev = hdev_from_device_index(device_index);

    if (hdev == NULL)
        return -1;

    Sint32 player_index = device_index;
    JOY_DBGMSG("SDL_XBOX_JoystickGetDevicePlayerIndex: %i\n", player_index);

    return device_index;
}

static SDL_JoystickGUID SDL_XBOX_JoystickGetDeviceGUID(Sint32 device_index) {
    HID_DEV_T *hdev = hdev_from_device_index(device_index);

    SDL_JoystickGUID ret;
    SDL_zero(ret);

    if (hdev != NULL)
    {
        //Format based on SDL_gamecontrollerdb.h
        ret.data[0] = 0x03;
        ret.data[4] = hdev->idVendor & 0xFF;
        ret.data[5] = (hdev->idVendor >> 8) & 0xFF;
        ret.data[8] = hdev->idProduct & 0xFF;
        ret.data[9] = (hdev->idProduct >> 8) & 0xFF;
    }
    return ret;
}

static SDL_JoystickID SDL_XBOX_JoystickGetDeviceInstanceID(Sint32 device_index) {
    HID_DEV_T *hdev = hdev_from_device_index(device_index);

    SDL_JoystickID ret;
    SDL_zero(ret);

    if (hdev != NULL)
    {
        SDL_memcpy(&ret, &hdev->uid, sizeof(hdev->uid));
    }
    JOY_DBGMSG("SDL_XBOX_JoystickGetDeviceInstanceID: %i\n", hdev->uid);
    return ret;
}

static Sint32 SDL_XBOX_JoystickOpen(SDL_Joystick *joystick, Sint32 device_index) {
    HID_DEV_T *hdev = hdev_from_device_index(device_index);

    if (hdev == NULL)
    {
        JOY_DBGMSG("SDL_XBOX_JoystickOpen: Could not find device index %i\n", device_index);
        return -1;
    }

    joystick->hwdata = (pjoystick_hwdata)SDL_malloc(sizeof(joystick_hwdata));
    assert(joystick->hwdata != NULL);
    SDL_zerop(joystick->hwdata);

    joystick->hwdata->hdev = hdev;
    joystick->hwdata->hdev->user_data = (void *)joystick;
    joystick->player_index = SDL_XBOX_JoystickGetDevicePlayerIndex(device_index);
    joystick->guid = SDL_XBOX_JoystickGetDeviceGUID(device_index);

    switch (hdev->type)
    {
    case XBOXOG_CONTROLLER:
    case XBOXONE:
    case XBOX360_WIRELESS:
    case XBOX360_WIRED:
        joystick->naxes = 6;     /* LStickY, LStickX, LTrigg, RStickY, RStickX, RTrigg */
        joystick->nballs = 0;    /* No balls here */
        joystick->nhats = 1;     /* D-pad */
        joystick->nbuttons = 10; /* A, B, X, Y, RB, LB, Back, Start, LThumb, RThumb */
        break;
    default:
        JOY_DBGMSG("SDL_XBOX_JoystickOpen: Not a supported joystick, hdev->type: %i\n", hdev->type);
        if (joystick->hwdata != NULL)
            SDL_free(joystick->hwdata);
        return -1;
    }

    JOY_DBGMSG("JoystickOpened:\n");
    JOY_DBGMSG("joystick device_index: %i\n", device_index);
    JOY_DBGMSG("joystick player_index: %i\n", joystick->player_index);
    JOY_DBGMSG("joystick uid: %i\n", hdev->uid);
    JOY_DBGMSG("joystick name: %s\n", SDL_XBOX_JoystickGetDeviceName(device_index));

    //Start reading interrupt pipe
    usbh_hid_start_int_read(hdev, 0, xboxjoy_int_read_callback);

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
        //Reset the expiry timer and leave
        joystick->hwdata->rumble_expiry = SDL_GetTicks() + duration_ms;
        return 0;
    }

    Sint32 ret = xboxjoy_rumble(joystick->hwdata->hdev, low_frequency_rumble, high_frequency_rumble);

    if (ret == -1)
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

    if (joystick == NULL || joystick->hwdata == NULL || joystick->hwdata->hdev == NULL)
    {
        return;
    }

    //Check if the rumble timer has expired.
    if (joystick->hwdata->rumble_expiry && SDL_GetTicks() > joystick->hwdata->rumble_expiry)
    {
        xboxjoy_rumble(joystick->hwdata->hdev, 0, 0);
        joystick->hwdata->rumble_expiry = 0;
    }

    uint8_t button_data[MAX_PACKET_SIZE];
    SDL_memcpy(button_data, joystick->hwdata->raw_data, MAX_PACKET_SIZE);
    if (xboxjoy_parse_input_data(joystick->hwdata->hdev, &xpad, button_data))
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

    xboxjoy_rumble(joystick->hwdata->hdev, 0, 0);

    HID_DEV_T *hdev = joystick->hwdata->hdev;
    hdev->user_data = NULL;
    if (hdev != NULL && hdev->read_func != NULL)
    {
        JOY_DBGMSG("Closing joystick:\n", joystick->hwdata->hdev->uid);
        JOY_DBGMSG("joystick player_index: %i\n", joystick->player_index);
        //The hdev is still registered in the backend usb driver to allow it to be repoened easily
        //but it stops reading the interrupt pipe to free up resources.
        usbh_hid_stop_int_read(hdev, 0);
    }
    SDL_free(joystick->hwdata);
    return;
}

static void SDL_XBOX_JoystickQuit(void) {
    JOY_DBGMSG("SDL_XBOX_JoystickQuit\n");
    usbh_install_hid_conn_callback(NULL, NULL);
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

static Sint32 xboxjoy_parse_input_data(HID_DEV_T *hdev, PXINPUT_GAMEPAD controller, Uint8 *rdata) {
    Uint16 wButtons;
    controller->wButtons = 0;

    if (hdev == NULL)
        return 0;

    switch(hdev->type)
    {
    case XBOXOG_CONTROLLER:
        wButtons = *((Uint16*)&rdata[2]);

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
    case XBOX360_WIRED:
        wButtons = *((Uint16*)&rdata[2]);

        //Map digital buttons
        if (wButtons & (1 << 0)) controller->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
        if (wButtons & (1 << 1)) controller->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
        if (wButtons & (1 << 2)) controller->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
        if (wButtons & (1 << 3)) controller->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
        if (wButtons & (1 << 4)) controller->wButtons |= XINPUT_GAMEPAD_START;
        if (wButtons & (1 << 5)) controller->wButtons |= XINPUT_GAMEPAD_BACK;
        if (wButtons & (1 << 6)) controller->wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
        if (wButtons & (1 << 7)) controller->wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
        if (wButtons & (1 << 8)) controller->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        if (wButtons & (1 << 9)) controller->wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        if (wButtons & (1 << 12)) controller->wButtons |= XINPUT_GAMEPAD_A;
        if (wButtons & (1 << 13)) controller->wButtons |= XINPUT_GAMEPAD_B;
        if (wButtons & (1 << 14)) controller->wButtons |= XINPUT_GAMEPAD_X;
        if (wButtons & (1 << 15)) controller->wButtons |= XINPUT_GAMEPAD_Y;

        //Map the left and right triggers
        controller->bLeftTrigger = rdata[4];
        controller->bRightTrigger = rdata[5];

        //Map analog sticks
        controller->sThumbLX = *((Sint16 *)&rdata[6]);
        controller->sThumbLY = *((Sint16 *)&rdata[8]);
        controller->sThumbRX = *((Sint16 *)&rdata[10]);
        controller->sThumbRY = *((Sint16 *)&rdata[12]);
        return 1;
    case XBOX360_WIRELESS:
        wButtons = *((Uint16*)&rdata[6]);

        //Map digital buttons
        if (wButtons & (1 << 0)) controller->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
        if (wButtons & (1 << 1)) controller->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
        if (wButtons & (1 << 2)) controller->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
        if (wButtons & (1 << 3)) controller->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
        if (wButtons & (1 << 4)) controller->wButtons |= XINPUT_GAMEPAD_START;
        if (wButtons & (1 << 5)) controller->wButtons |= XINPUT_GAMEPAD_BACK;
        if (wButtons & (1 << 6)) controller->wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
        if (wButtons & (1 << 7)) controller->wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
        if (wButtons & (1 << 8)) controller->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        if (wButtons & (1 << 9)) controller->wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        if (wButtons & (1 << 12)) controller->wButtons |= XINPUT_GAMEPAD_A;
        if (wButtons & (1 << 13)) controller->wButtons |= XINPUT_GAMEPAD_B;
        if (wButtons & (1 << 14)) controller->wButtons |= XINPUT_GAMEPAD_X;
        if (wButtons & (1 << 15)) controller->wButtons |= XINPUT_GAMEPAD_Y;

        //Map the left and right triggers
        controller->bLeftTrigger = rdata[8];
        controller->bRightTrigger = rdata[9];

        //Map analog sticks
        controller->sThumbLX = *((Sint16 *)&rdata[10]);
        controller->sThumbLY = *((Sint16 *)&rdata[12]);
        controller->sThumbRX = *((Sint16 *)&rdata[14]);
        controller->sThumbRY = *((Sint16 *)&rdata[16]);
        return 1;
    case XBOXONE:
        wButtons = *((Uint16*)&rdata[4]);

        //Map digital buttons
        if (wButtons & (1 << 8)) controller->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
        if (wButtons & (1 << 9)) controller->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
        if (wButtons & (1 << 10)) controller->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
        if (wButtons & (1 << 11)) controller->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
        if (wButtons & (1 << 2)) controller->wButtons |= XINPUT_GAMEPAD_START;
        if (wButtons & (1 << 3)) controller->wButtons |= XINPUT_GAMEPAD_BACK;
        if (wButtons & (1 << 14)) controller->wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
        if (wButtons & (1 << 15)) controller->wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
        if (wButtons & (1 << 12)) controller->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        if (wButtons & (1 << 13)) controller->wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        if (wButtons & (1 << 4)) controller->wButtons |= XINPUT_GAMEPAD_A;
        if (wButtons & (1 << 5)) controller->wButtons |= XINPUT_GAMEPAD_B;
        if (wButtons & (1 << 6)) controller->wButtons |= XINPUT_GAMEPAD_X;
        if (wButtons & (1 << 7)) controller->wButtons |= XINPUT_GAMEPAD_Y;

        //Map the left and right triggers
        controller->bLeftTrigger = rdata[6];
        controller->bRightTrigger = rdata[8];

        //Map analog sticks
        controller->sThumbLX = *((Sint16 *)&rdata[10]);
        controller->sThumbLY = *((Sint16 *)&rdata[12]);
        controller->sThumbRX = *((Sint16 *)&rdata[14]);
        controller->sThumbRY = *((Sint16 *)&rdata[16]);
        return 1;
    default:
        return 0;
    }
    return 0;
}

#endif /* SDL_JOYSTICK_XBOX */
