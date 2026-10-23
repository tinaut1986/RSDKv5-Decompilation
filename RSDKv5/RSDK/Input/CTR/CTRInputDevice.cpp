#include "RSDK/Dev/CTR/BottomUi.hpp"

using namespace RSDK;

InputState mappings[12];

static u32 remap[12] = {
  KEY_UP,
  KEY_DOWN,
  KEY_LEFT,
  KEY_RIGHT,
  KEY_B,
  KEY_A,
  0,
  KEY_Y,
  KEY_X,
  0,
  KEY_START,
  KEY_SELECT
};

static touchPosition touchPos;
static touchPosition touchPosPrev;

void RSDK::SKU::InputDeviceCTR::UpdateInput()
{
  hidScanInput();

  u32 kDown = hidKeysDown();
  u32 kHeld = hidKeysHeld();
#if MANIA_DEV_TOOLS
  // Keys injected over FTP (SITE INPUT) count as held; a newly set one also counts as pressed.
  static u32 prevInject;
  const u32 inject = RSDK::DevTools::injectKeys;
  kHeld |= inject;
  kDown |= inject & ~prevInject;
  prevInject = inject;
  RSDK::DevTools::RunKeys(&kHeld, &kDown);
#endif

  if (kDown || kHeld)
    this->anyPress = 1;

  for (int i = 0; i < 12; i++) {
    mappings[i].down = kDown & remap[i];
    mappings[i].press = kHeld & remap[i];
  }

  hidTouchRead(&touchPos);

  // A touch toggled the libctru text console (consoleClear wiped the bottom screen: it flashed on every tap and
  // kept clearing while a finger moved). Not while the graphical bottom screen owns it.
  if (RSDK::engineDebugMode && !RSDK::BottomUi::Ready()) {
    if (touchPos.px != touchPosPrev.px || touchPos.py != touchPosPrev.py) {
      printToConsole = !printToConsole;

      if (!printToConsole)
        consoleClear();
      else {
        printf("Console log enabled!\n");
      }
    }
  }

  touchPosPrev = touchPos;
}

// TODO: the code below *technically* works, but is kind of a mess.
// Clean up later.
void RSDK::SKU::InputDeviceCTR::ProcessInput(int32 controllerID)
{
  for (int i = 0; i < PLAYER_COUNT; i++) {
    if (i == 2)
      continue;

    controller[i].keyUp.press       |= mappings[0].press;
    controller[i].keyDown.press     |= mappings[1].press;
    controller[i].keyLeft.press     |= mappings[2].press;
    controller[i].keyRight.press    |= mappings[3].press;
    controller[i].keyA.press        |= mappings[4].press;
    controller[i].keyB.press        |= mappings[5].press;
    controller[i].keyC.press        |= mappings[6].press;
    controller[i].keyX.press        |= mappings[7].press;
    controller[i].keyY.press        |= mappings[8].press;
    controller[i].keyZ.press        |= mappings[9].press;
    controller[i].keyStart.press    |= mappings[10].press;
    controller[i].keySelect.press   |= mappings[11].press;
  }
}

// code below here borrowed liberally from the other backends and
// modified accordingly
RSDK::SKU::InputDeviceCTR *RSDK::SKU::InitCTRDevice(uint32 id) {
  if (inputDeviceCount == INPUTDEVICE_COUNT)
    return NULL;

  if (inputDeviceList[inputDeviceCount] && 
      inputDeviceList[inputDeviceCount]->active)
    return NULL;

  if (inputDeviceList[inputDeviceCount])
    delete inputDeviceList[inputDeviceCount];

  inputDeviceList[inputDeviceCount] = new InputDeviceCTR();

  InputDeviceCTR *device = (InputDeviceCTR*) inputDeviceList[inputDeviceCount];
  device->gamepadType = (DEVICE_API_NONE << 16) | (DEVICE_TYPE_CONTROLLER << 8) | (DEVICE_SWITCH_HANDHELD << 0);
  device->disabled = false;
  device->id = id;
  device->active = true;
  device->anyPress = 1;

  inputSlots[0] = device->id;
  inputSlotDevices[0] = device;
  device->isAssigned = true;

  hidScanInput();
  hidTouchRead(&touchPos);
  hidTouchRead(&touchPosPrev);
  
  inputDeviceCount++;
  return device;
}

void RSDK::SKU::InitCTRInputAPI() {
  // this is to get the compiler to shut up
  char deviceName[12];
  snprintf(deviceName, 12, "CTRDevice0");

  uint32 id = 1;
  GenerateHashCRC(&id, deviceName);

  inputDeviceCount = 0;

  InputDeviceCTR* device = InitCTRDevice(id);
  if (device) {
    device->controllerID = 1;
  }

  return;
}
