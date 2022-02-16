// TouchpadTest.cpp : Defines the entry point for the application.
//

#include "framework.h"
#include "TouchpadTest.h"
#include <windows.h>    // included for Windows Touch
#include <windowsx.h>   // included for point conversion
#include <hidsdi.h>
#include <hidpi.h>
#include "strsafe.h"
#include <list>

// block the size of the touchpad. This is discoverable via HID but for simplicity its hard coded here
POINT touchpadSize{ 2000, 1200 };

// touch points data
#define MAXPOINTS 10
int points[MAXPOINTS][2];
// color 
int idLookup[MAXPOINTS];

static int radius = 50;
static int mouseradius = 20;
static BOOL TouchpadExists = FALSE;

// There should be at least as many colors
// as there can be touch points so that you
// can have different colors for each point
COLORREF colors[] = { RGB(153,255,51),
                      RGB(153,0,0),
                      RGB(0,153,0),
                      RGB(255,255,0),
                      RGB(255,51,204),
                      RGB(0,0,0),
                      RGB(0,153,0),
                      RGB(153, 255, 255),
                      RGB(153,153,255),
                      RGB(0,51,153)
};

// Global Variables:
#define MAX_LOADSTRING 100
HINSTANCE hInst;                                // current instance
WCHAR szTitle[MAX_LOADSTRING];                  // The title bar text
WCHAR szWindowClass[MAX_LOADSTRING];            // the main window class name

// Forward declarations of functions included in this code module:
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);
int wmId;

UINT cInputs;
PTOUCHINPUT pInputs;
POINT ptInput;

// For double buffering
static HDC memDC = 0;
static HBITMAP hMemBmp = 0;
HBITMAP hOldBmp = 0;

// For drawing / fills
PAINTSTRUCT ps;
HDC hdc;

// For tracking dwId to points
int index;


struct TouchpadContact
{
public:
    int ContactId;
    int X;
    int Y;
    TouchpadContact(int contactId, int x, int y)
        : ContactId(contactId)
        , X(x)
        , Y(y)
    {
    }

    bool operator==(TouchpadContact rhs)
    {
        return (ContactId == rhs.ContactId && X == rhs.X && Y == rhs.Y);
    }
};

std::list<TouchpadContact> contacts;


struct TouchpadContactCreator
{
public:
    TouchpadContactCreator()
        : ContactId(-1)
        , X(-1)
        , Y(-1)
    {
    }

    int ContactId;
    int X;
    int Y;

    bool TryCreate(TouchpadContact* contact)
    {
        if (ContactId > -1 && X > -1 && Y > -1)
        {
            contact->ContactId = ContactId;
            contact->X = X;
            contact->Y = Y;
            return true;
        }
        contact = NULL;
        return false;
    }

    void Clear()
    {
        ContactId = -1;
        X = -1;
        Y = -1;
    }
};

struct OrderLinkCollection 
{
    _NODISCARD constexpr bool operator()(const HIDP_VALUE_CAPS& _Left, const HIDP_VALUE_CAPS& _Right) const 
    {
        return _Left.LinkCollection < _Right.LinkCollection;
    }
};

bool Exists()
{
    UINT deviceListCount = 0;
    UINT rawInputDeviceListSize = (UINT)sizeof(RAWINPUTDEVICELIST);

    if (GetRawInputDeviceList(
        NULL,
        &deviceListCount,
        rawInputDeviceListSize) != 0)
    {
        return false;
    }

    auto devices = new RAWINPUTDEVICELIST[deviceListCount];

    if (GetRawInputDeviceList(
        devices,
        &deviceListCount,
        rawInputDeviceListSize) != deviceListCount)
    {
        return false;
    }

    for (UINT count = 0; count < deviceListCount; count++)
    {
        auto device = devices[count];
        if (device.dwType != RIM_TYPEHID)
        {
            continue;
        }

        UINT deviceInfoSize = 0;

        if (GetRawInputDeviceInfo(
            device.hDevice,
            RIDI_DEVICEINFO,
            NULL,
            &deviceInfoSize) != 0)
        {
            continue;
        }

        RID_DEVICE_INFO deviceInfo{ 0 };
        deviceInfo.cbSize = deviceInfoSize;

        if (GetRawInputDeviceInfo(
            device.hDevice,
            RIDI_DEVICEINFO,
            &deviceInfo,
            &deviceInfoSize) == -1)
        {
            continue;
        }

        if ((deviceInfo.hid.usUsagePage == 0x000D) &&
            (deviceInfo.hid.usUsage == 0x0005))
        {
            return true;
        }
    }
    return false;
}

bool RegisterInput(HWND windowHandle)
{
    // Precision Touchpad (PTP) in HID Clients Supported in Windows
    // https://docs.microsoft.com/en-us/windows-hardware/drivers/hid/hid-architecture#hid-clients-supported-in-windows
    RAWINPUTDEVICE device
    {
        0x000D,
        0x0005,
        0,
        windowHandle
    };

    return RegisterRawInputDevices(&device, 1, sizeof(RAWINPUTDEVICE));
}

void ParseInput(LPARAM lParam)
{
    contacts.clear();

    // Get RAWINPUT.
    UINT rawInputSize = 0;
    UINT rawInputHeaderSize = sizeof(RAWINPUTHEADER);
    RAWINPUT* rawInput = 0;
    BYTE* rawHidRawData = 0;
    UINT length = 0;
    BYTE* rawInputData = 0;
    BYTE* rawHidRawDataPointer = 0;
    int rawInputOffset = 0;

    void* rawInputPointer = 0;

    BYTE* preparsedDataPointer = 0;
    HIDP_CAPS caps{};
    UINT preparsedDataSize = 0;
    USHORT valueCapsLength = 0;
    HIDP_VALUE_CAPS* valueCaps = 0;

    UINT scanTime = 0;
    UINT contactCount = 0;
    TouchpadContactCreator creator;

    std::list<HIDP_VALUE_CAPS> orderedCaps;

    if (GetRawInputData(
        (HRAWINPUT)lParam,
        RID_INPUT,
        0,
        &rawInputSize,
        rawInputHeaderSize) != 0)
    {
        goto error;
    }

    rawInputPointer = new BYTE[rawInputSize];

    if (GetRawInputData(
        (HRAWINPUT)lParam,
        RID_INPUT,
        rawInputPointer,
        &rawInputSize,
        rawInputHeaderSize) != rawInputSize)
    {
        goto error;
    }

    rawInput = (RAWINPUT*)rawInputPointer;

    rawInputData = new BYTE[rawInputSize];
    memcpy(rawInputData, rawInputPointer, rawInputSize);

    length = rawInput->data.hid.dwSizeHid * rawInput->data.hid.dwCount;
    rawHidRawData = new BYTE[length];
    rawInputOffset = (int)rawInputSize - length;

    memcpy(rawHidRawData, rawInputData + rawInputOffset, length);

    // Parse RAWINPUT.
    rawHidRawDataPointer = new BYTE[length];
    memcpy(rawHidRawDataPointer, rawHidRawData, length);

    if (GetRawInputDeviceInfo(
        rawInput->header.hDevice,
        RIDI_PREPARSEDDATA,
        NULL,
        &preparsedDataSize) != 0)
    {
        goto error;
    }

    preparsedDataPointer = new BYTE[preparsedDataSize];

    if (GetRawInputDeviceInfo(
        rawInput->header.hDevice,
        RIDI_PREPARSEDDATA,
        preparsedDataPointer,
        &preparsedDataSize) != preparsedDataSize)
    {
        goto error;
    }

    if (HidP_GetCaps(
        (PHIDP_PREPARSED_DATA)preparsedDataPointer,
        &caps) != HIDP_STATUS_SUCCESS)
    {
        goto error;
    }

    valueCapsLength = caps.NumberInputValueCaps;
    valueCaps = new HIDP_VALUE_CAPS[valueCapsLength];

    if (HidP_GetValueCaps(
        HIDP_REPORT_TYPE::HidP_Input,
        valueCaps,
        &valueCapsLength,
        (PHIDP_PREPARSED_DATA)preparsedDataPointer) != HIDP_STATUS_SUCCESS)
    {
        goto error;
    }

    for (int count = 0; count < valueCapsLength; count++)
    {
        orderedCaps.push_back(valueCaps[count]);
    }

    orderedCaps.sort(OrderLinkCollection());

    for (auto itter = orderedCaps.begin(); itter != orderedCaps.end(); itter++)
    {
        ULONG value = 0;
        if (HidP_GetUsageValue(
            HIDP_REPORT_TYPE::HidP_Input,
            (*itter).UsagePage,
            (*itter).LinkCollection,
            (*itter).NotRange.Usage,
            &value,
            (PHIDP_PREPARSED_DATA)preparsedDataPointer,
            (PCHAR)rawHidRawDataPointer,
            length) != HIDP_STATUS_SUCCESS)
        {
            continue;
        }

        // Usage Page and ID in Windows Precision Touchpad input reports
        // https://docs.microsoft.com/en-us/windows-hardware/design/component-guidelines/windows-precision-touchpad-required-hid-top-level-collections#windows-precision-touchpad-input-reports
        switch ((*itter).LinkCollection)
        {
        case 0:
            if (0x0D == (*itter).UsagePage)
            {
                switch ((*itter).NotRange.Usage)
                {
                case 0x56: // Scan Time
                    scanTime = value;
                    break;

                case 0x54: // Contact Count
                    contactCount = value;
                    break;
                }
            }
            break;

        default:
            switch ((*itter).UsagePage)
            {
            case 0x0D:
                if ((*itter).NotRange.Usage == 0x51) // Contact ID
                {
                    creator.ContactId = (int)value;
                }
                break;

            case 0x01:
                if ((*itter).NotRange.Usage == 0x30) // X
                {
                    creator.X = (int)value;
                }
                else if ((*itter).NotRange.Usage == 0x31) // Y
                {
                    creator.Y = (int)value;
                }
                break;
            }
            break;
        }

        TouchpadContact contact(0, 0, 0);
        if (creator.TryCreate(&contact))
        {
            contacts.push_back(contact);
            if (contacts.size() >= contactCount)
                break;

            creator.Clear();
        }
    }

error:

    delete[] rawHidRawDataPointer; rawHidRawDataPointer = 0;
    delete[] preparsedDataPointer; preparsedDataPointer = 0;
    delete[] rawInputPointer; rawInputPointer = 0;
    delete[] rawInputData; rawInputData = 0;
    delete[] rawHidRawData; rawHidRawData = 0;
    delete[] valueCaps; valueCaps = 0;
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
    LoadStringW(hInstance, IDC_TOUCHPADTEST, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    // Perform application initialization:
    if (!InitInstance (hInstance, nCmdShow))
    {
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_TOUCHPADTEST));

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
    wcex.hIcon          = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_TOUCHPADTEST));
    wcex.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground  = (HBRUSH)(COLOR_WINDOW+1);
    wcex.lpszMenuName   = MAKEINTRESOURCEW(IDC_TOUCHPADTEST);
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

   RegisterTouchWindow(hWnd, 0);

   // the following code initializes the points
   for (int i = 0; i < MAXPOINTS; i++) 
   {
       points[i][0] = -1;
       points[i][1] = -1;
       idLookup[i] = -1;
   }
   EnableMouseInPointer(TRUE);

   TouchpadExists = Exists();

   if (TouchpadExists)
   {
       RegisterInput(hWnd);
   }

   ShowWindow(hWnd, nCmdShow);
   UpdateWindow(hWnd);

   return TRUE;
}

// This function is used to return an index given an ID
int GetContactIndex(int dwID) 
{
    for (int i = 0; i < MAXPOINTS; i++) 
    {
        if (idLookup[i] == -1) 
        {
            idLookup[i] = dwID;
            return i;
        }
        else 
        {
            if (idLookup[i] == dwID) 
            {
                return i;
            }
        }
    }

    // Out of contacts
    return -1;
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
        ParseInput(lParam);
        RedrawWindow(hWnd, NULL, NULL, RDW_INTERNALPAINT | RDW_INVALIDATE);
    }
    break;

    case WM_POINTERUPDATE:
    {
        auto id = GET_POINTERID_WPARAM(wParam);
        auto isNew = IS_POINTER_NEW_WPARAM(wParam);
        auto isInRange = IS_POINTER_INRANGE_WPARAM(wParam);
        auto isInContact = IS_POINTER_INCONTACT_WPARAM(wParam);
        auto x = GET_X_LPARAM(lParam);
        auto y = GET_Y_LPARAM(lParam);
        auto index = GetContactIndex(id);
        if (index < MAXPOINTS)
        {
            ptInput.x = x;
            ptInput.y = y;
            ScreenToClient(hWnd, &ptInput);

            points[index][0] = ptInput.x;
            points[index][1] = ptInput.y;
        }

        RedrawWindow(hWnd, NULL, NULL, RDW_INTERNALPAINT | RDW_INVALIDATE);
    }
    break;

        // pass touch messages to the touch handler 
    case WM_TOUCH:
        cInputs = LOWORD(wParam);
        pInputs = new TOUCHINPUT[cInputs];
        if (pInputs) 
        {
            if (GetTouchInputInfo((HTOUCHINPUT)lParam, cInputs, pInputs, sizeof(TOUCHINPUT))) 
            {
                for (int i = 0; i < static_cast<INT>(cInputs); i++) 
                {
                    TOUCHINPUT ti = pInputs[i];
                    index = GetContactIndex(ti.dwID);

                    if (ti.dwID != 0 && index < MAXPOINTS) 
                    {
                        // Do something with your touch input handle
                        ptInput.x = TOUCH_COORD_TO_PIXEL(ti.x);
                        ptInput.y = TOUCH_COORD_TO_PIXEL(ti.y);
                        ScreenToClient(hWnd, &ptInput);

                        if (ti.dwFlags & TOUCHEVENTF_UP) 
                        {
                            points[index][0] = -1;
                            points[index][1] = -1;
                        }
                        else 
                        {
                            points[index][0] = ptInput.x;
                            points[index][1] = ptInput.y;
                        }
                    }
                }
            }

            // If you handled the message and don't want anything else done with it, you can close it
            CloseTouchInputHandle((HTOUCHINPUT)lParam);
            delete[] pInputs;
        }
        else 
        {
            // Handle the error here 
        }

        RedrawWindow(hWnd, NULL, NULL, RDW_INTERNALPAINT | RDW_INVALIDATE);
        break;

    case WM_COMMAND:
        {
            int wmId = LOWORD(wParam);
            // Parse the menu selections:
            switch (wmId)
            {
            case IDM_ABOUT:
                DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
                break;
            case IDM_EXIT:
                DestroyWindow(hWnd);
                break;
            default:
                return DefWindowProc(hWnd, message, wParam, lParam);
            }
        }
        break;

    case WM_PAINT:
        {
            hdc = BeginPaint(hWnd, &ps);
            RECT client;
            int x, y;
            GetClientRect(hWnd, &client);

            // start double buffering
            if (!memDC) 
            {
                memDC = CreateCompatibleDC(hdc);
            }
            hMemBmp = CreateCompatibleBitmap(hdc, client.right, client.bottom);
            hOldBmp = (HBITMAP)SelectObject(memDC, hMemBmp);
            auto brush = CreateSolidBrush(RGB(255, 255, 255));
            FillRect(memDC, &client, brush);

            //Draw Touched Points                
            for (int i = 0; i < MAXPOINTS; i++) 
            {
                auto colour = CreateSolidBrush(colors[i]);
                SelectObject(memDC, colour);
                x = points[i][0];
                y = points[i][1];
                if (x > 0 && y > 0) 
                {
                    Ellipse(memDC, x - radius, y - radius, x + radius, y + radius);
                }

                DeleteObject(colour);
            }

            int i = 0;
            // Draw the touchpad
            for (auto contact = contacts.begin(); contact != contacts.end(); contact++)
            {
                auto colour = CreateSolidBrush(colors[i++]);
                SelectObject(memDC, colour);

                // x and y are in the range 0 to touchpadSize
                // output coordinates are in the range 0 to Window size
                POINT drawcenter
                {
                    (LONG)((((float)(*contact).X) / touchpadSize.x)* (client.right - client.left)),
                    (LONG)((((float)(*contact).Y) / touchpadSize.y)* (client.bottom - client.top))
                };

                Ellipse(memDC, drawcenter.x - mouseradius, drawcenter.y - mouseradius, drawcenter.x + mouseradius, drawcenter.y + mouseradius);

                DeleteObject(colour);
            }

            BitBlt(hdc, 0, 0, client.right, client.bottom, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, hOldBmp);

            EndPaint(hWnd, &ps);

            DeleteDC(memDC);
            DeleteObject(hMemBmp);
            hMemBmp = 0;
            memDC = 0;
            DeleteObject(brush);
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

// Message handler for about box.
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}
