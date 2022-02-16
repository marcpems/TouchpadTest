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

#define MAXPOINTS 100

POINT touchpadSize{ 2000, 1200 };

// You will use this array to track touch points
int points[MAXPOINTS][2];

// You will use this array to switch the color / track ids
int idLookup[MAXPOINTS];


// You can make the touch points larger
// by changing this radius value
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

#define MAX_LOADSTRING 100

// Global Variables:
HINSTANCE hInst;                                // current instance
WCHAR szTitle[MAX_LOADSTRING];                  // The title bar text
WCHAR szWindowClass[MAX_LOADSTRING];            // the main window class name

// Forward declarations of functions included in this code module:
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);

int wmId, wmEvent, i, x, y;

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

class TouchpadHelper
{
public: 
    static bool Exists()
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

        for(int count = 0; count < deviceListCount; count++)
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

    static bool RegisterInput(HWND windowHandle)
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

    static void ParseInput(LPARAM lParam)
    {
        contacts.clear();

        //TCHAR msg[1024];

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

        //__try
        //{
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
        //}
        //__finally
        //{
        //  delete[] rawInputPointer;
        //}

        // Parse RAWINPUT.
        rawHidRawDataPointer = new BYTE[length];
        memcpy(rawHidRawDataPointer, rawHidRawData, length);

        //        __try
        //        {
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

        //wsprintf(msg, L"Contact count: %d ", contacts.size());
        //OutputDebugString(msg);
        //for (auto contact = contacts.begin(); contact != contacts.end(); contact++)
        //{
        //    Ellipse(memDC, (*contact).X - mouseradius, (*contact).Y - mouseradius, (*contact).X + mouseradius, (*contact).Y + mouseradius);
        //    wsprintf(msg, L" (%d,%d),", (*contact).X, (*contact).Y);
        //    OutputDebugString(msg);
        //}
        //OutputDebugString(L"\r\n");
    error:

        delete[] rawHidRawDataPointer; rawHidRawDataPointer = 0;
        delete[] preparsedDataPointer; preparsedDataPointer = 0;
        delete[] rawInputPointer; rawInputPointer = 0;
        delete[] rawInputData; rawInputData = 0;
        delete[] rawHidRawData; rawHidRawData = 0;
        delete[] valueCaps; valueCaps = 0;
    }
};

void GetList()
{
    UINT nDevices, nStored, i;
    RAWINPUTDEVICELIST* pRawInputDeviceList = 0;
    
    if (GetRawInputDeviceList(NULL, &nDevices, sizeof(RAWINPUTDEVICELIST)) != 0) 
    { 
        return; 
    }

    // The list of devices can change between calls to GetRawInputDeviceList,
    // so call it again if the function returns ERROR_INSUFFICIENT_BUFFER
    do
    {
        if ((pRawInputDeviceList = (PRAWINPUTDEVICELIST)realloc(pRawInputDeviceList, sizeof(RAWINPUTDEVICELIST) * nDevices)) == NULL)
        { 
            return;
        }

        nStored = GetRawInputDeviceList(pRawInputDeviceList, &nDevices, sizeof(RAWINPUTDEVICELIST));
    } 
    while (nStored == (UINT)-1 && GetLastError() == ERROR_INSUFFICIENT_BUFFER);

    if (nStored == (UINT)-1) 
    { 
        return;
    }

    for (i = 0; i < nStored; ++i)
    {
        // do the job with each pRawInputDeviceList[i] element...
        switch (pRawInputDeviceList[i].dwType)
        {
        case 0:
//            OutputDebugString(L"Type 0\r\n");
            break;
        case 1:
//            OutputDebugString(L"Type 1\r\n");
            break;
        case 2:
//            OutputDebugString(L"Type 2\r\n");
            break;
        default:
//            OutputDebugString(L"No Type \r\n");
            break;
        }
    }

    // after the job, free the RAWINPUTDEVICELIST
    free(pRawInputDeviceList);
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

    GetList();

    //

    //Rid[0].usUsagePage = 0x01;          // HID_USAGE_PAGE_GENERIC
    //Rid[0].usUsage = 0x02;              // HID_USAGE_GENERIC_MOUSE
    //Rid[0].dwFlags = RIDEV_NOLEGACY;    // adds mouse and also ignores legacy mouse messages
    //Rid[0].hwndTarget = 0;

    ////Rid[0].usUsagePage = 0x01;          // HID_USAGE_PAGE_GENERIC
    ////Rid[0].usUsage = 0x05;              // HID_USAGE_GENERIC_GAMEPAD
    ////Rid[0].dwFlags = 0;                 // adds game pad
    ////Rid[0].hwndTarget = 0;

    //if (RegisterRawInputDevices(Rid, 1, sizeof(Rid[0])) == FALSE)
    //{
    //    //registration failed. Call GetLastError for the cause of the error.
    //}

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

   TouchpadExists = TouchpadHelper::Exists();

   if (TouchpadExists)
   {
       TouchpadHelper::RegisterInput(hWnd);
   }

   ShowWindow(hWnd, nCmdShow);
   UpdateWindow(hWnd);

   return TRUE;
}

LRESULT OnTouch(HWND hWnd, WPARAM wParam, LPARAM lParam) 
{
    BOOL bHandled = FALSE;
    UINT cInputs = LOWORD(wParam);
    PTOUCHINPUT pInputs = new TOUCHINPUT[cInputs];
    if (pInputs) 
    {
        if (GetTouchInputInfo((HTOUCHINPUT)lParam, cInputs, pInputs, sizeof(TOUCHINPUT))) 
        {
            for (UINT i = 0; i < cInputs; i++) {
                TOUCHINPUT ti = pInputs[i];
                //do something with each touch input entry
            }
            bHandled = TRUE;
        }
        else 
        {
            /* handle the error here */
        }
        delete[] pInputs;
    }
    else 
    {
        /* handle the error here, probably out of memory */
    }
    if (bHandled) 
    {
        // if you handled the message, close the touch input handle and return
        CloseTouchInputHandle((HTOUCHINPUT)lParam);
        return 0;
    }
    else 
    {
        // if you didn't handle the message, let DefWindowProc handle it
        return DefWindowProc(hWnd, WM_TOUCH, wParam, lParam);
    }
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
        TouchpadHelper::ParseInput(lParam);
        RedrawWindow(hWnd, NULL, NULL, RDW_INTERNALPAINT | RDW_INVALIDATE);

        //UINT dwSize = 0;
        //HRESULT hResult; 
        //WCHAR szTempOutput[20480];

        //GetRawInputData((HRAWINPUT)lParam, RID_INPUT, NULL, &dwSize, sizeof(RAWINPUTHEADER));
        //LPBYTE lpb = new BYTE[dwSize];
        //if (lpb == NULL)
        //{
        //    return 0;
        //}

        //if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, lpb, &dwSize, sizeof(RAWINPUTHEADER)) != dwSize)
        //    OutputDebugString(TEXT("GetRawInputData does not return correct size !\n"));

        //RAWINPUT* raw = (RAWINPUT*)lpb;

        //if (raw->header.dwType == RIM_TYPEMOUSE)
        //{
        //    WCHAR szDeviceName[40960] = {'\0'};
        //    UINT dwSize = 40960;
        //    hResult = GetRawInputDeviceInfoW(raw->header.hDevice, RIDI_DEVICENAME, szDeviceName, &dwSize);

        //    if (raw->data.mouse.usFlags & MOUSE_ATTRIBUTES_CHANGED)
        //    { 
        //        mousePoints[0][0] = -1;
        //        mousePoints[0][1] = -1;
        //    }
        //    else
        //    {
        //        if (raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)
        //        {
        //            bool isVirtualDesktop = (raw->data.mouse.usFlags & MOUSE_VIRTUAL_DESKTOP) == MOUSE_VIRTUAL_DESKTOP;

        //            int width = GetSystemMetrics(isVirtualDesktop ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
        //            int height = GetSystemMetrics(isVirtualDesktop ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);

        //            int absoluteX = int((raw->data.mouse.lLastX / 65535.0f) * width);
        //            int absoluteY = int((raw->data.mouse.lLastY / 65535.0f) * height);

        //            mousePoints[0][0] = absoluteX;
        //            mousePoints[0][1] = absoluteY;
        //        }
        //        else
        //        {
        //            float width = (float)GetSystemMetrics(SM_CXSCREEN)*100;
        //            float height = (float)GetSystemMetrics(SM_CYSCREEN)*100;

        //            auto absoluteX = ((float)raw->data.mouse.lLastX / 65535.0f) * width;
        //            auto absoluteY = ((float)raw->data.mouse.lLastY / 65535.0f) * height;

        //            mousePoints[0][0] += absoluteX;
        //            mousePoints[0][1] += absoluteY;
        //        }
        //    }

        //    hResult = StringCchPrintf(szTempOutput, 20480,
        //        TEXT("Mouse: usFlags=%04x ulButtons=%04x usButtonFlags=%04x usButtonData=%04x ulRawButtons=%04x lLastX=%04x lLastY=%04x Name=%s ulExtraInformation=%04x\r\n"),
        //        raw->data.mouse.usFlags,
        //        raw->data.mouse.ulButtons,
        //        raw->data.mouse.usButtonFlags,
        //        raw->data.mouse.usButtonData,
        //        raw->data.mouse.ulRawButtons,
        //        raw->data.mouse.lLastX,
        //        raw->data.mouse.lLastY,
        //        szDeviceName,
        //        raw->data.mouse.ulExtraInformation);

        //    if (FAILED(hResult))
        //    {
        //        // TODO: write error handler
        //    }
        //    OutputDebugString(szTempOutput);
        //    RedrawWindow(hWnd, NULL, NULL, RDW_INTERNALPAINT | RDW_INVALIDATE);
        //}

        //delete[] lpb;
        //return 0;
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

        POINTER_INPUT_TYPE ptrTypeInfo = { 0 };
        GetPointerType(id, &ptrTypeInfo);

        OutputDebugString(L"Pointer Update ");
        switch (ptrTypeInfo)
        {
        case PT_TOUCHPAD:
            OutputDebugString(L"Touchpad");
            break;
        case PT_TOUCH:
            OutputDebugString(L"Touch");
            break;
        case PT_MOUSE:
            OutputDebugString(L"Mouse");
            break;
        case PT_PEN:
            OutputDebugString(L"Pen");
            break;
        case PT_POINTER:
            OutputDebugString(L"Pointer");
            break;
        }

        OutputDebugString(L"\r\n");

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
//        OnTouch(hWnd, wParam, lParam);
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
            for (i = 0; i < MAXPOINTS; i++) 
            {
                auto colour = CreateSolidBrush(colors[i]);
                SelectObject(memDC, colour);
                x = points[i][0];
                y = points[i][1];
                if (x > 0 && y > 0) 
                {
                    Ellipse(memDC, x - radius, y - radius, x + radius, y + radius);
                }

                RECT rect; 
                // Get the window size to adjust the touchpad points to fit the window.
                GetWindowRect(hWnd, &rect);

                // x and y are in the range 0 to touchpadSize
                // output coordinates are in the range 0 to Window size
                for (auto contact = contacts.begin(); contact != contacts.end(); contact++)
                {
                    POINT drawcenter
                    { 
                        (((float)(*contact).X) / touchpadSize.x) * (rect.right - rect.left),
                        (((float)(*contact).Y) / touchpadSize.y) * (rect.bottom - rect.top) };

                    Ellipse(memDC, drawcenter.x - mouseradius, drawcenter.y - mouseradius, drawcenter.x + mouseradius, drawcenter.y + mouseradius);
                }
                //x = (int)mousePoints[i][0];
                //y = (int)mousePoints[i][1];
                //if (x > 0 && y > 0)
                //{
                //    Ellipse(memDC, x - mouseradius, y - mouseradius, x + mouseradius, y + mouseradius);
                //}

                DeleteObject(colour);
            }

            RECT rect;
            // Get the window size to adjust the touchpad points to fit the window.
            GetWindowRect(hWnd, &rect);
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
                    (((float)(*contact).X) / touchpadSize.x) * (rect.right - rect.left),
                    (((float)(*contact).Y) / touchpadSize.y) * (rect.bottom - rect.top) };

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
