// TouchpadKeypad.cpp : Defines the entry point for the application.
//

#include "framework.h"
#include "TouchpadKeypad.h"
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <Windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <hidusage.h>
#include <stdio.h>

#define MAX_LOADSTRING 100

// Global Variables:
HINSTANCE hInst;                                // current instance
WCHAR szTitle[MAX_LOADSTRING];                  // The title bar text
WCHAR szWindowClass[MAX_LOADSTRING];            // the main window class name

// Forward declarations of functions included in this code module:
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);

// Whether to output debugging information
#define DEBUG_MODE 0

// HID usages that are not already defined
#define HID_USAGE_DIGITIZER_CONTACT_ID 0x51
#define HID_USAGE_DIGITIZER_CONTACT_COUNT 0x54

// C++ exception wrapping the Win32 GetLastError() status
class win32_error : std::exception
{
public:
    win32_error(DWORD errorCode)
        : m_errorCode(errorCode)
    {

    }

    win32_error()
        : win32_error(GetLastError())
    {

    }

    DWORD code() const
    {
        return m_errorCode;
    }

private:
    DWORD m_errorCode;
};

// C++ exception wrapping the HIDP_STATUS_* codes
class hid_error : std::exception
{
public:
    hid_error(NTSTATUS status)
        : m_errorCode(status)
    {

    }

    NTSTATUS code() const
    {
        return m_errorCode;
    }

private:
    NTSTATUS m_errorCode;
};

// Wrapper for malloc with unique_ptr semantics, to allow
// for variable-sized structures.
struct free_deleter { void operator()(void* ptr) { free(ptr); } };
template<typename T> using malloc_ptr = std::unique_ptr<T, free_deleter>;

// Contact information parsed from the HID report descriptor.
struct contact_info
{
    USHORT link;
    RECT touchArea;
};

// The data for a touch event.
struct contact
{
    contact_info info;
    ULONG id;
    POINT point;
};

// Device information, such as touch area bounds and HID offsets.
// This can be reused across HID events, so we only have to parse
// this info once.
struct device_info
{
    malloc_ptr<_HIDP_PREPARSED_DATA> preparsedData; // HID internal data
    USHORT linkContactCount = 0; // Link collection for number of contacts present
    std::vector<contact_info> contactInfo; // Link collection and touch area for each contact
    std::optional<RECT> touchAreaOverride; // Override touch area for all points if set
};

// Caches per-device info for better performance
static std::unordered_map<HANDLE, device_info> g_devices;

// Holds the current primary touch point ID
static thread_local ULONG t_primaryContactID;

// Key press state
bool xkp = false;
bool zkp = false;

// Split axis. true = x, false = y
bool split = false;

// Calibration
int maxx, maxy, minx, miny;

// Allocates a malloc_ptr with the given size. The size must be
// greater than or equal to sizeof(T).
template<typename T>
static malloc_ptr<T>
make_malloc(size_t size)
{
    T* ptr = (T*)malloc(size);
    if (ptr == nullptr) {
        throw std::bad_alloc();
    }
    return malloc_ptr<T>(ptr);
}

// C-style printf for debug output.
#if DEBUG_MODE
static void
vfdebugf(FILE* f, const char* fmt, va_list args)
{
    vfprintf(f, fmt, args);
    putc('\n', f);
}
static void
debugf(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfdebugf(stderr, fmt, args);
    va_end(args);
}
#else
#define debugf(...) ((void)0)
#endif

// Reads the raw input header for the given raw input handle.
static RAWINPUTHEADER GetRawInputHeader(HRAWINPUT hInput)
{
    RAWINPUTHEADER hdr;
    UINT size = sizeof(hdr);
    if (GetRawInputData(hInput, RID_HEADER, &hdr, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) {
        throw win32_error();
    }
    return hdr;
}

// Reads the raw input data for the given raw input handle.
static malloc_ptr<RAWINPUT> GetRawInput(HRAWINPUT hInput, RAWINPUTHEADER hdr)
{
    malloc_ptr<RAWINPUT> input = make_malloc<RAWINPUT>(hdr.dwSize);
    UINT size = hdr.dwSize;
    if (GetRawInputData(hInput, RID_INPUT, input.get(), &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) {
        throw win32_error();
    }
    return input;
}

// Gets a list of raw input devices attached to the system.
static std::vector<RAWINPUTDEVICELIST> GetRawInputDeviceList()
{
    std::vector<RAWINPUTDEVICELIST> devices(64);
    while (true) {
        UINT numDevices = (UINT)devices.size();
        UINT ret = GetRawInputDeviceList(&devices[0], &numDevices, sizeof(RAWINPUTDEVICELIST));
        if (ret != (UINT)-1) {
            devices.resize(ret);
            return devices;
        }
        else if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            devices.resize(numDevices);
        }
        else {
            throw win32_error();
        }
    }
}

// Gets info about a raw input device.
static RID_DEVICE_INFO GetRawInputDeviceInfo(HANDLE hDevice)
{
    RID_DEVICE_INFO info;
    info.cbSize = sizeof(RID_DEVICE_INFO);
    UINT size = sizeof(RID_DEVICE_INFO);
    if (GetRawInputDeviceInfoW(hDevice, RIDI_DEVICEINFO, &info, &size) == (UINT)-1) {
        throw win32_error();
    }
    return info;
}

// Reads the preparsed HID report descriptor for the device
// that generated the given raw input.
static malloc_ptr<_HIDP_PREPARSED_DATA> GetHidPreparsedData(HANDLE hDevice)
{
    UINT size = 0;
    if (GetRawInputDeviceInfoW(hDevice, RIDI_PREPARSEDDATA, nullptr, &size) == (UINT)-1) {
        throw win32_error();
    }
    malloc_ptr<_HIDP_PREPARSED_DATA> preparsedData = make_malloc<_HIDP_PREPARSED_DATA>(size);
    if (GetRawInputDeviceInfoW(hDevice, RIDI_PREPARSEDDATA, preparsedData.get(), &size) == (UINT)-1) {
        throw win32_error();
    }
    return preparsedData;
}

// Returns all input button caps for the given preparsed
// HID report descriptor.
static std::vector<HIDP_BUTTON_CAPS> GetHidInputButtonCaps(PHIDP_PREPARSED_DATA preparsedData)
{
    NTSTATUS status;
    HIDP_CAPS caps;
    status = HidP_GetCaps(preparsedData, &caps);
    if (status != HIDP_STATUS_SUCCESS) {
        throw hid_error(status);
    }
    USHORT numCaps = caps.NumberInputButtonCaps;
    std::vector<HIDP_BUTTON_CAPS> buttonCaps(numCaps);
    status = HidP_GetButtonCaps(HidP_Input, &buttonCaps[0], &numCaps, preparsedData);
    if (status != HIDP_STATUS_SUCCESS) {
        throw hid_error(status);
    }
    buttonCaps.resize(numCaps);
    return buttonCaps;
}

// Returns all input value caps for the given preparsed
// HID report descriptor.
static std::vector<HIDP_VALUE_CAPS> GetHidInputValueCaps(PHIDP_PREPARSED_DATA preparsedData)
{
    NTSTATUS status;
    HIDP_CAPS caps;
    status = HidP_GetCaps(preparsedData, &caps);
    if (status != HIDP_STATUS_SUCCESS) {
        throw hid_error(status);
    }
    USHORT numCaps = caps.NumberInputValueCaps;
    std::vector<HIDP_VALUE_CAPS> valueCaps(numCaps);
    status = HidP_GetValueCaps(HidP_Input, &valueCaps[0], &numCaps, preparsedData);
    if (status != HIDP_STATUS_SUCCESS) {
        throw hid_error(status);
    }
    valueCaps.resize(numCaps);
    return valueCaps;
}

// Reads the pressed status of a single HID report button.
static bool GetHidUsageButton(
    HIDP_REPORT_TYPE reportType,
    USAGE usagePage,
    USHORT linkCollection,
    USAGE usage,
    PHIDP_PREPARSED_DATA preparsedData,
    PBYTE report,
    ULONG reportLen)
{
    ULONG numUsages = HidP_MaxUsageListLength(
        reportType,
        usagePage,
        preparsedData);
    std::vector<USAGE> usages(numUsages);
    NTSTATUS status = HidP_GetUsages(
        reportType,
        usagePage,
        linkCollection,
        &usages[0],
        &numUsages,
        preparsedData,
        (PCHAR)report,
        reportLen);
    if (status != HIDP_STATUS_SUCCESS) {
        throw hid_error(status);
    }
    usages.resize(numUsages);
    return std::find(usages.begin(), usages.end(), usage) != usages.end();
}

// Reads a single HID report value in logical units.
static ULONG GetHidUsageLogicalValue(
    HIDP_REPORT_TYPE reportType,
    USAGE usagePage,
    USHORT linkCollection,
    USAGE usage,
    PHIDP_PREPARSED_DATA preparsedData,
    PBYTE report,
    ULONG reportLen)
{
    ULONG value;
    NTSTATUS status = HidP_GetUsageValue(
        reportType,
        usagePage,
        linkCollection,
        usage,
        &value,
        preparsedData,
        (PCHAR)report,
        reportLen);
    if (status != HIDP_STATUS_SUCCESS) {
        throw hid_error(status);
    }
    return value;
}

// Reads a single HID report value in physical units.
static LONG GetHidUsagePhysicalValue(
    HIDP_REPORT_TYPE reportType,
    USAGE usagePage,
    USHORT linkCollection,
    USAGE usage,
    PHIDP_PREPARSED_DATA preparsedData,
    PBYTE report,
    ULONG reportLen)
{
    LONG value;
    NTSTATUS status = HidP_GetScaledUsageValue(
        reportType,
        usagePage,
        linkCollection,
        usage,
        &value,
        preparsedData,
        (PCHAR)report,
        reportLen);
    if (status != HIDP_STATUS_SUCCESS) {
        return -1;
    }
    return value;
}

// Registers the specified window to receive touchpad HID events.
static void RegisterTouchpadInput(HWND hWnd)
{
    RAWINPUTDEVICE dev;
    dev.usUsagePage = HID_USAGE_PAGE_DIGITIZER;
    dev.usUsage = HID_USAGE_DIGITIZER_TOUCH_PAD;
    dev.dwFlags = RIDEV_INPUTSINK;
    dev.hwndTarget = hWnd;
    if (!RegisterRawInputDevices(&dev, 1, sizeof(RAWINPUTDEVICE))) {
        throw win32_error();
    }
}

// Gets the device info associated with the given raw input. Uses the
// cached info if available; otherwise parses the HID report descriptor
// and stores it into the cache.
static device_info& GetDeviceInfo(HANDLE hDevice)
{
    if (g_devices.count(hDevice)) {
        return g_devices.at(hDevice);
    }

    device_info dev;
    std::optional<USHORT> linkContactCount;
    dev.preparsedData = GetHidPreparsedData(hDevice);

    // Struct to hold our parser state
    struct contact_info_tmp
    {
        bool hasContactID = false;
        bool hasTip = false;
        bool hasX = false;
        bool hasY = false;
        RECT touchArea;
    };
    std::unordered_map<USHORT, contact_info_tmp> contacts;

    // Get the touch area for all the contacts. Also make sure that each one
    // is actually a contact, as specified by:
    // https://docs.microsoft.com/en-us/windows-hardware/design/component-guidelines/windows-precision-touchpad-required-hid-top-level-collections
    for (const HIDP_VALUE_CAPS& cap : GetHidInputValueCaps(dev.preparsedData.get())) {
        if (cap.IsRange || !cap.IsAbsolute) {
            continue;
        }

        if (cap.UsagePage == HID_USAGE_PAGE_GENERIC) {
            if (cap.NotRange.Usage == HID_USAGE_GENERIC_X) {
                contacts[cap.LinkCollection].touchArea.left = cap.PhysicalMin;
                contacts[cap.LinkCollection].touchArea.right = cap.PhysicalMax;
                contacts[cap.LinkCollection].hasX = true;
            }
            else if (cap.NotRange.Usage == HID_USAGE_GENERIC_Y) {
                contacts[cap.LinkCollection].touchArea.top = cap.PhysicalMin;
                contacts[cap.LinkCollection].touchArea.bottom = cap.PhysicalMax;
                contacts[cap.LinkCollection].hasY = true;
            }
        }
        else if (cap.UsagePage == HID_USAGE_PAGE_DIGITIZER) {
            if (cap.NotRange.Usage == HID_USAGE_DIGITIZER_CONTACT_COUNT) {
                linkContactCount = cap.LinkCollection;
            }
            else if (cap.NotRange.Usage == HID_USAGE_DIGITIZER_CONTACT_ID) {
                contacts[cap.LinkCollection].hasContactID = true;
            }
        }
    }

    for (const HIDP_BUTTON_CAPS& cap : GetHidInputButtonCaps(dev.preparsedData.get())) {
        if (cap.UsagePage == HID_USAGE_PAGE_DIGITIZER) {
            if (cap.NotRange.Usage == HID_USAGE_DIGITIZER_TIP_SWITCH) {
                contacts[cap.LinkCollection].hasTip = true;
            }
        }
    }

    if (!linkContactCount.has_value()) {
        throw std::runtime_error("No contact count usage found");
    }
    dev.linkContactCount = linkContactCount.value();

    for (const auto& kvp : contacts) {
        USHORT link = kvp.first;
        const contact_info_tmp& info = kvp.second;
        if (info.hasContactID && info.hasTip && info.hasX && info.hasY) {
            debugf("Contact for device %p: link=%d, touchArea={%d,%d,%d,%d}",
                hDevice,
                link,
                info.touchArea.left,
                info.touchArea.top,
                info.touchArea.right,
                info.touchArea.bottom);
            dev.contactInfo.push_back({ link, info.touchArea });
        }
    }

    return g_devices[hDevice] = std::move(dev);
}

// Reads all touch contact points from a raw input event.
static std::vector<contact> GetContacts(device_info& dev, RAWINPUT* input)
{
    std::vector<contact> contacts;

    DWORD sizeHid = input->data.hid.dwSizeHid;
    DWORD count = input->data.hid.dwCount;
    BYTE* rawData = input->data.hid.bRawData;
    if (count == 0) {
        debugf("Raw input contained no HID events");
        return contacts;
    }

    ULONG numContacts = GetHidUsageLogicalValue(
        HidP_Input,
        HID_USAGE_PAGE_DIGITIZER,
        dev.linkContactCount,
        HID_USAGE_DIGITIZER_CONTACT_COUNT,
        dev.preparsedData.get(),
        rawData,
        sizeHid);

    if (numContacts > dev.contactInfo.size()) {
        debugf("Device reported more contacts (%u) than we have links (%zu)", numContacts, dev.contactInfo.size());
        numContacts = (ULONG)dev.contactInfo.size();
    }

    // It's a little ambiguous as to whether contact count includes
    // released contacts. I interpreted the specs as a yes, but this
    // may require additional testing.
    for (ULONG i = 0; i < numContacts; ++i) {
        contact_info& info = dev.contactInfo[i];
        bool tip = GetHidUsageButton(
            HidP_Input,
            HID_USAGE_PAGE_DIGITIZER,
            info.link,
            HID_USAGE_DIGITIZER_TIP_SWITCH,
            dev.preparsedData.get(),
            rawData,
            sizeHid);

        if (!tip) {
            debugf("Contact has tip = 0, ignoring");
            continue;
        }

        ULONG id = GetHidUsageLogicalValue(
            HidP_Input,
            HID_USAGE_PAGE_DIGITIZER,
            info.link,
            HID_USAGE_DIGITIZER_CONTACT_ID,
            dev.preparsedData.get(),
            rawData,
            sizeHid);

        LONG x = GetHidUsagePhysicalValue(
            HidP_Input,
            HID_USAGE_PAGE_GENERIC,
            info.link,
            HID_USAGE_GENERIC_X,
            dev.preparsedData.get(),
            rawData,
            sizeHid);

        LONG y = GetHidUsagePhysicalValue(
            HidP_Input,
            HID_USAGE_PAGE_GENERIC,
            info.link,
            HID_USAGE_GENERIC_Y,
            dev.preparsedData.get(),
            rawData,
            sizeHid);

        if (x != -1 || y != -1)
            contacts.push_back({ info, id, { x, y } });
    }

    return contacts;
}

// Sets key state
void SetKeyState(WORD vkCode, bool down)
{
    INPUT input = { 0 };
    if (down) {
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = vkCode;
        SendInput(1, &input, sizeof(INPUT));
    }
    else {
        input.type = INPUT_KEYBOARD;
        input.ki.dwFlags = KEYEVENTF_KEYUP;
        input.ki.wVk = vkCode;
        SendInput(1, &input, sizeof(INPUT));
    }
}

// Updates calibration in registry
void UpdateCalibration() {
    HKEY hKey;

    RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\TouchpadKeypad\\", NULL, KEY_ALL_ACCESS, &hKey);
    debugf("Updating calibration");
    if (!hKey) {
        debugf("Creating registry key");
        RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\TouchpadKeypad\\", NULL, NULL, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &hKey, NULL);
    }
    RegSetValueEx(hKey, L"maxx", NULL, REG_DWORD, (const BYTE*)&maxx, sizeof(maxx));
    RegSetValueEx(hKey, L"maxy", NULL, REG_DWORD, (const BYTE*)&maxy, sizeof(maxy));
    RegSetValueEx(hKey, L"minx", NULL, REG_DWORD, (const BYTE*)&minx, sizeof(minx));
    RegSetValueEx(hKey, L"miny", NULL, REG_DWORD, (const BYTE*)&miny, sizeof(miny));
    RegCloseKey(hKey);
}
// Reads calibration from registry.
void ReadCalibration() {
    HKEY hKey;
    DWORD dwBufferSize = sizeof(DWORD);

    RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\TouchpadKeypad\\", NULL, KEY_ALL_ACCESS, &hKey);
    if (!hKey) {
        return;
    }
    RegQueryValueExW(hKey, L"maxx", 0, NULL, (LPBYTE)&maxx, &dwBufferSize);
    RegQueryValueExW(hKey, L"maxy", 0, NULL, (LPBYTE)&maxy, &dwBufferSize);
    RegQueryValueExW(hKey, L"minx", 0, NULL, (LPBYTE)&minx, &dwBufferSize);
    RegQueryValueExW(hKey, L"miny", 0, NULL, (LPBYTE)&miny, &dwBufferSize);
    RegCloseKey(hKey);

    debugf("Read calibrate from registry: %d, %d, %d, %d", minx, miny, maxx, maxy);
}
// Handles a WM_INPUT event. May update wParam/lParam to be delivered
// to the real WndProc. Returns true if the event is handled entirely
// at the hook layer and should not be delivered to the real WndProc.
// Returns false if the real WndProc should be called.
static bool HandleRawInput(WPARAM* wParam, LPARAM* lParam)
{
    HRAWINPUT hInput = (HRAWINPUT)*lParam;
    RAWINPUTHEADER hdr = GetRawInputHeader(hInput);
    if (hdr.dwType != RIM_TYPEHID) {
        debugf("Got raw input for device %p with event type != HID: %u", hdr.hDevice, hdr.dwType);

        // Suppress mouse input events to prevent it from getting
        // mixed in with our absolute movement events. Unfortunately
        // this has the side effect of disabling all non-touchpad
        // input. One solution might be to determine the device that
        // sent the event and check if it's also a touchpad, and only
        // filter out events from such devices.
        if (hdr.dwType == RIM_TYPEMOUSE) {
            return true;
        }
        return false;
    }

    debugf("Got HID raw input event for device %p", hdr.hDevice);

    device_info& dev = GetDeviceInfo(hdr.hDevice);
    malloc_ptr<RAWINPUT> input = GetRawInput(hInput, hdr);
    std::vector<contact> contacts = GetContacts(dev, input.get());
    if (contacts.empty()) {
        debugf("Found no contacts in input event");
    }

    INPUT kinput;
    kinput.type = INPUT_KEYBOARD;
    kinput.ki.time = 0;
    kinput.ki.dwExtraInfo = 0;
 
    // Key press states
    bool zp = false;
    bool xp = false;

    for (const contact& contact : contacts) {
        if (contact.point.x > maxx) {
            maxx = contact.point.x;
            UpdateCalibration();
        }
        if (contact.point.y > maxy) {
            maxy = contact.point.y;
            UpdateCalibration();
        }
        if ((contact.point.x < minx || minx == 0) && contact.point.x != -1) {
            minx = contact.point.x;
            UpdateCalibration();
        }
        if ((contact.point.y < miny || miny == 0) && contact.point.y != -1) {
            miny = contact.point.y;
            UpdateCalibration();
        }

        if (split) {
            if (contact.point.x < minx + ((maxx - minx) / 2))
                zp = true;
            else
                xp = true;
        }
        else {
            if (contact.point.y < miny + ((maxy - miny) / 2))
                zp = true;
            else
                xp = true;
        }
    }

    if (zp && !zkp) {
        SetKeyState(83, true);
        debugf("1 up");
        zkp = true;
    }
    else if (!zp && zkp) {
        SetKeyState(83, false);
        debugf("1 down");
        zkp = false;
    }
    if (xp & !xkp) {
        SetKeyState(68, true);
        debugf("2 up");
        xkp = true;
    }
    else if (!xp && xkp) {
        SetKeyState(68, false);
        debugf("2 down");
        xkp = false;
    }
    return false;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // TODO: Place code here.

    // Initialize global strings
    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_TOUCHPADKEYPAD, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    // Perform application initialization:
    if (!InitInstance (hInstance, nCmdShow))
    {
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_TOUCHPADKEYPAD));

    MSG msg;

    // Main message loop:
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    return (int) msg.wParam;
}

// Creates a console and redirects input/output streams to it.
// Used to display debug output in non-console applications.
// If DEBUG_FILE is set, opens the log file and points g_debugFile
// to it.
static void
StartDebugMode()
{
#if DEBUG_MODE
    FreeConsole();
    AllocConsole();
#pragma warning(push)
#pragma warning(disable:4996)
    freopen("CONIN$", "r", stdin);
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
#pragma warning(pop)
#endif
}


//
//  FUNCTION: MyRegisterClass()
//
//  PURPOSE: Registers the window class.
//
ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style          = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = WndProc;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = hInstance;
    wcex.hIcon          = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_TOUCHPADKEYPAD));
    wcex.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground  = (HBRUSH)(COLOR_WINDOW+1);
    wcex.lpszMenuName   = MAKEINTRESOURCEW(IDC_TOUCHPADKEYPAD);
    wcex.lpszClassName  = szWindowClass;
    wcex.hIconSm        = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}

//
//   FUNCTION: InitInstance(HINSTANCE, int)
//
//   PURPOSE: Saves instance handle and creates main window
//
//   COMMENTS:
//
//        In this function, we save the instance handle in a global variable and
//        create and display the main program window.
//
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    hInst = hInstance; // Store instance handle in our global variable

    HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr);

    if (!hWnd)
    {
        return FALSE;
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    StartDebugMode();
    ReadCalibration();
    RegisterTouchpadInput(hWnd);
    return TRUE;
}

//
//  FUNCTION: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  PURPOSE: Processes messages for the main window.
//
//  WM_COMMAND  - process the application menu
//  WM_PAINT    - Paint the main window
//  WM_DESTROY  - post a quit message and return
//
//
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_INPUT:
        {
            HandleRawInput(&wParam, &lParam);
        }
        break;
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            // TODO: Add any drawing code that uses hdc here...
            EndPaint(hWnd, &ps);
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}
