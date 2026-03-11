#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include <string.h>
#include <stdio.h>

/* Function pointer for RtlGetVersion - use long/void* to avoid C parse issues with SDK types */
typedef long (WINAPI *RtlGetVersionPtr)(void*);

#define OVERLAY_ALPHA 153  /* 60% opacity (255 * 0.6) */
#define OVERLAY_SIZE_SMALL  0
#define OVERLAY_SIZE_MEDIUM 1
#define OVERLAY_SIZE_LARGE  2
#define OVERLAY_SIZE_XLARGE 3
#define OVERLAY_SIZE_HUGE   4

typedef struct {
	BOOL popup;
	BOOL overlay_enabled;
	int overlay_size;  /* OVERLAY_SIZE_SMALL to HUGE */
} Settings;

#define WM_TRAYICON (WM_USER + 1)
#define WM_UPDATE_TRAY_ICON (WM_APP + 1)
#define ID_TRAY_EXIT 1001
#define ID_TRAY_OVERLAY 1002
#define ID_TRAY_SIZE_SMALL 1003
#define ID_TRAY_SIZE_MED   1004
#define ID_TRAY_SIZE_LARGE 1005
#define ID_TRAY_SIZE_XLARGE 1006
#define ID_TRAY_SIZE_HUGE   1007
#define ID_TRAY_RESTART 1008
#define ID_TRAY_RUNATSTARTUP 1009
#define ID_TRAY_OPEN_TASKSCHED 1010
#define IDI_APPICON 1
#define IDI_APPICON_DISABLED 2

void ShowError(LPCSTR message);
DWORD GetOSVersion();
void PressKey(int keyCode);
void ReleaseKey(int keyCode);
void ToggleCapsLockState();
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
BOOL AddTrayIcon(HWND hwnd, HINSTANCE hInstance, BOOL enabled);
void UpdateTrayIcon(HWND hwnd, HINSTANCE hInstance, BOOL enabled);
void RemoveTrayIcon(HWND hwnd);
void LoadOverlaySettings(void);
void SaveOverlaySettings(void);
void RestartApplication(void);
BOOL IsRunAtStartup(void);
BOOL SetRunAtStartup(BOOL enable);
void GetOverlayDimensions(int sizeIndex, int* width, int* height);
HWND CreateOverlayWindow(HINSTANCE hInstance);
void OverlaySetVisible(BOOL show);
void OverlayUpdateSize(void);

static const char* TRAY_WINDOW_CLASS = "SwitchyTrayWindow";
static const char* OVERLAY_WINDOW_CLASS = "SwitchyOverlay";
HHOOK hHook;
BOOL enabled = TRUE;
BOOL keystrokeCapsProcessed = FALSE;
BOOL keystrokeShiftProcessed = FALSE;
BOOL capsUsedForToggle = FALSE;
BOOL letCapsKeyThrough = FALSE;  /* when Alt/Ctrl+Caps with Caps ON: let both down+up through */
HWND g_hwndTray = NULL;
HWND g_hwndOverlay = NULL;
HANDLE g_hMutex = NULL;

Settings settings = {
	.popup = FALSE,
	.overlay_enabled = TRUE,
	.overlay_size = OVERLAY_SIZE_MEDIUM
};


int main(int argc, char** argv)
{
	if (argc > 1 && strcmp(argv[1], "popup") == 0 && GetOSVersion() >= 10)
	{
		settings.popup = TRUE;
	}
#if _DEBUG
	printf("Pop-up is %s\n", settings.popup ? "enabled" : "disabled");
#endif
	LoadOverlaySettings();

	g_hMutex = CreateMutex(0, 0, "Switchy");
	if (GetLastError() == ERROR_ALREADY_EXISTS)
	{
		ShowError("Another instance of Switchy is already running!");
		return 1;
	}

	hHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, 0, 0);
	if (hHook == NULL)
	{
		ShowError("Error calling \"SetWindowsHookEx(...)\"");
		return 1;
	}

	/* Create hidden window for system tray icon */
	WNDCLASSEXA wc = { 0 };
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = TrayWndProc;
	wc.hInstance = GetModuleHandle(NULL);
	wc.lpszClassName = TRAY_WINDOW_CLASS;
	if (!RegisterClassExA(&wc))
	{
		ShowError("Error registering tray window class");
		UnhookWindowsHookEx(hHook);
		return 1;
	}
	g_hwndTray = CreateWindowExA(0, TRAY_WINDOW_CLASS, "Switchy", 0, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
	if (!g_hwndTray)
	{
		ShowError("Error creating tray window");
		UnhookWindowsHookEx(hHook);
		return 1;
	}
	if (!AddTrayIcon(g_hwndTray, wc.hInstance, 1))
	{
		ShowError("Error adding tray icon");
		DestroyWindow(g_hwndTray);
		UnhookWindowsHookEx(hHook);
		return 1;
	}
	g_hwndOverlay = CreateOverlayWindow(wc.hInstance);
	if (!g_hwndOverlay)
	{
		/* non-fatal: continue without overlay */
	}

	MSG messages;
	while (GetMessage(&messages, NULL, 0, 0))
	{
		TranslateMessage(&messages);
		DispatchMessage(&messages);
	}

	RemoveTrayIcon(g_hwndTray);
	if (g_hwndOverlay)
		DestroyWindow(g_hwndOverlay);
	DestroyWindow(g_hwndTray);
	UnhookWindowsHookEx(hHook);
	if (g_hMutex)
		CloseHandle(g_hMutex);

	return 0;
}


void ShowError(LPCSTR message)
{
	MessageBox(NULL, message, "Error", MB_OK | MB_ICONERROR);
}


LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_TRAYICON)
	{
		if (lParam == WM_RBUTTONUP)
		{
			POINT pt;
			GetCursorPos(&pt);
			HMENU menu = CreatePopupMenu();
			AppendMenuA(menu, settings.overlay_enabled ? MF_CHECKED : MF_UNCHECKED, ID_TRAY_OVERLAY, "Overlay when disabled");
			AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
			HMENU sizeMenu = CreatePopupMenu();
			AppendMenuA(sizeMenu, settings.overlay_size == OVERLAY_SIZE_SMALL  ? MF_CHECKED : MF_UNCHECKED, ID_TRAY_SIZE_SMALL, "Small");
			AppendMenuA(sizeMenu, settings.overlay_size == OVERLAY_SIZE_MEDIUM ? MF_CHECKED : MF_UNCHECKED, ID_TRAY_SIZE_MED, "Medium");
			AppendMenuA(sizeMenu, settings.overlay_size == OVERLAY_SIZE_LARGE  ? MF_CHECKED : MF_UNCHECKED, ID_TRAY_SIZE_LARGE, "Large");
			AppendMenuA(sizeMenu, settings.overlay_size == OVERLAY_SIZE_XLARGE ? MF_CHECKED : MF_UNCHECKED, ID_TRAY_SIZE_XLARGE, "X-Large");
			AppendMenuA(sizeMenu, settings.overlay_size == OVERLAY_SIZE_HUGE   ? MF_CHECKED : MF_UNCHECKED, ID_TRAY_SIZE_HUGE, "Huge");
			AppendMenuA(menu, MF_POPUP, (UINT_PTR)sizeMenu, "Overlay size");
			AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
			AppendMenuA(menu, MF_STRING, ID_TRAY_RESTART, "Restart");
			AppendMenuA(menu, IsRunAtStartup() ? MF_CHECKED : MF_UNCHECKED, ID_TRAY_RUNATSTARTUP, "Run at startup");
			AppendMenuA(menu, MF_STRING, ID_TRAY_OPEN_TASKSCHED, "Open Task Scheduler");
			AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
			AppendMenuA(menu, MF_STRING, ID_TRAY_EXIT, "Exit");
			SetForegroundWindow(hwnd);
			TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
			DestroyMenu(menu);
			return 0;
		}
	}
	if (msg == WM_COMMAND)
	{
		switch (LOWORD(wParam))
		{
		case ID_TRAY_EXIT:
			PostQuitMessage(0);
			return 0;
		case ID_TRAY_OVERLAY:
			settings.overlay_enabled = !settings.overlay_enabled;
			SaveOverlaySettings();
			if (!enabled)
				OverlaySetVisible(settings.overlay_enabled);
			return 0;
		case ID_TRAY_SIZE_SMALL:
			settings.overlay_size = OVERLAY_SIZE_SMALL;
			SaveOverlaySettings();
			OverlayUpdateSize();
			return 0;
		case ID_TRAY_SIZE_MED:
			settings.overlay_size = OVERLAY_SIZE_MEDIUM;
			SaveOverlaySettings();
			OverlayUpdateSize();
			return 0;
		case ID_TRAY_SIZE_LARGE:
			settings.overlay_size = OVERLAY_SIZE_LARGE;
			SaveOverlaySettings();
			OverlayUpdateSize();
			return 0;
		case ID_TRAY_SIZE_XLARGE:
			settings.overlay_size = OVERLAY_SIZE_XLARGE;
			SaveOverlaySettings();
			OverlayUpdateSize();
			return 0;
		case ID_TRAY_SIZE_HUGE:
			settings.overlay_size = OVERLAY_SIZE_HUGE;
			SaveOverlaySettings();
			OverlayUpdateSize();
			return 0;
		case ID_TRAY_RESTART:
			RestartApplication();
			return 0;
		case ID_TRAY_RUNATSTARTUP:
			SetRunAtStartup(!IsRunAtStartup());
			return 0;
		case ID_TRAY_OPEN_TASKSCHED:
			ShellExecuteA(NULL, "open", "taskschd.msc", NULL, NULL, SW_SHOWNORMAL);
			return 0;
		}
	}
	if (msg == WM_UPDATE_TRAY_ICON)
	{
		UpdateTrayIcon(hwnd, GetModuleHandle(NULL), (BOOL)wParam);
		if (g_hwndOverlay)
		{
			if (!(BOOL)wParam && settings.overlay_enabled)
				OverlaySetVisible(1);
			else
				OverlaySetVisible(0);
		}
		return 0;
	}
	if (msg == WM_DESTROY)
	{
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcA(hwnd, msg, wParam, lParam);
}


BOOL AddTrayIcon(HWND hwnd, HINSTANCE hInstance, BOOL enabled)
{
	NOTIFYICONDATAA nid = { 0 };
	nid.cbSize = sizeof(nid);
	nid.hWnd = hwnd;
	nid.uID = 1;
	nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	nid.uCallbackMessage = WM_TRAYICON;
	nid.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(enabled ? IDI_APPICON : IDI_APPICON_DISABLED));
	if (!nid.hIcon)
		nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
	strncpy_s(nid.szTip, sizeof(nid.szTip),
		enabled ? "Switchy - Caps Lock to switch layout" : "Switchy Disabled - Alt+Caps or Ctrl+Caps to enable",
		_TRUNCATE);
	return Shell_NotifyIconA(NIM_ADD, &nid) != 0;
}

void UpdateTrayIcon(HWND hwnd, HINSTANCE hInstance, BOOL enabled)
{
	NOTIFYICONDATAA nid = { 0 };
	nid.cbSize = sizeof(nid);
	nid.hWnd = hwnd;
	nid.uID = 1;
	nid.uFlags = NIF_ICON | NIF_TIP;
	nid.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(enabled ? IDI_APPICON : IDI_APPICON_DISABLED));
	if (!nid.hIcon)
		nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
	strncpy_s(nid.szTip, sizeof(nid.szTip),
		enabled ? "Switchy - Caps Lock to switch layout" : "Switchy Disabled - Alt+Caps or Ctrl+Caps to enable",
		_TRUNCATE);
	Shell_NotifyIconA(NIM_MODIFY, &nid);
}


void RemoveTrayIcon(HWND hwnd)
{
	NOTIFYICONDATAA nid = { 0 };
	nid.cbSize = sizeof(nid);
	nid.hWnd = hwnd;
	nid.uID = 1;
	Shell_NotifyIconA(NIM_DELETE, &nid);
}


#define OVERLAY_REG_KEY "Software\\Switchy"
void LoadOverlaySettings(void)
{
	HKEY hKey;
	if (RegOpenKeyExA(HKEY_CURRENT_USER, OVERLAY_REG_KEY, 0, KEY_READ, &hKey) != 0)
		return;
	{
		DWORD val = 0, size = sizeof(DWORD);
		if (RegQueryValueExA(hKey, "OverlayEnabled", NULL, NULL, (LPBYTE)&val, &size) == 0)
			settings.overlay_enabled = (val != 0);
		val = (DWORD)settings.overlay_size;
		size = sizeof(DWORD);
		if (RegQueryValueExA(hKey, "OverlaySize", NULL, NULL, (LPBYTE)&val, &size) == 0 && val <= OVERLAY_SIZE_HUGE)
			settings.overlay_size = (int)val;
		RegCloseKey(hKey);
	}
}

void SaveOverlaySettings(void)
{
	HKEY hKey;
	if (RegCreateKeyExA(HKEY_CURRENT_USER, OVERLAY_REG_KEY, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) != 0)
		return;
	{
		DWORD val = settings.overlay_enabled ? 1 : 0;
		RegSetValueExA(hKey, "OverlayEnabled", 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
		val = (DWORD)settings.overlay_size;
		RegSetValueExA(hKey, "OverlaySize", 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
		RegCloseKey(hKey);
	}
}

#define TASK_NAME "Switchy"
BOOL IsRunAtStartup(void)
{
	/* Query Task Scheduler directly via schtasks /query */
	char params[256];
	sprintf_s(params, sizeof(params),
		"/query /tn \"" TASK_NAME "\" /fo LIST");

	STARTUPINFOA si = { sizeof(si) };
	PROCESS_INFORMATION pi = { 0 };
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;

	char cmdLine[512];
	sprintf_s(cmdLine, sizeof(cmdLine), "schtasks.exe %s", params);

	if (!CreateProcessA(NULL, cmdLine, NULL, NULL, FALSE,
		CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
		return FALSE;

	WaitForSingleObject(pi.hProcess, 5000);
	DWORD exitCode = 1;
	GetExitCodeProcess(pi.hProcess, &exitCode);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);

	return (exitCode == 0);
}

BOOL SetRunAtStartup(BOOL enable)
{
	char params[MAX_PATH + 384];
	if (enable)
	{
		char exePath[MAX_PATH];
		char username[256];
		DWORD unameLen = (DWORD)(sizeof(username) / sizeof(username[0]));
		GetModuleFileNameA(NULL, exePath, MAX_PATH);
		if (!GetUserNameA(username, &unameLen))
			username[0] = '\0';
		/* /ru = run as this user at logon, /rl limited = run without elevation
		   /f = overwrite if exists */
		if (username[0])
			sprintf_s(params, sizeof(params),
				"/create /tn \"" TASK_NAME "\" /tr \"\\\"%s\\\"\" /sc onlogon /ru \"%s\" /rl limited /f",
				exePath, username);
		else
			sprintf_s(params, sizeof(params),
				"/create /tn \"" TASK_NAME "\" /tr \"\\\"%s\\\"\" /sc onlogon /rl limited /f",
				exePath);
	}
	else
	{
		sprintf_s(params, sizeof(params),
			"/delete /tn \"" TASK_NAME "\" /f");
	}
	/* "runas" verb elevates schtasks — one UAC prompt to set up, never again */
	SHELLEXECUTEINFOA sei = { sizeof(sei) };
	sei.lpVerb = "runas";
	sei.lpFile = "schtasks.exe";
	sei.lpParameters = params;
	sei.nShow = SW_HIDE;
	sei.fMask = SEE_MASK_NOCLOSEPROCESS;
	if (!ShellExecuteExA(&sei) || !sei.hProcess)
		return FALSE;

	WaitForSingleObject(sei.hProcess, 10000);
	DWORD exitCode = 1;
	GetExitCodeProcess(sei.hProcess, &exitCode);
	CloseHandle(sei.hProcess);

	return (exitCode == 0);
}

void RestartApplication(void)
{
	char exePath[MAX_PATH];
	if (GetModuleFileNameA(NULL, exePath, MAX_PATH) == 0)
		return;
	if (g_hMutex)
	{
		CloseHandle(g_hMutex);
		g_hMutex = NULL;
	}
	{
		STARTUPINFOA si = { sizeof(si) };
		PROCESS_INFORMATION pi = { 0 };
		if (CreateProcessA(exePath, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
		{
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
			PostQuitMessage(0);
		}
	}
}

void GetOverlayDimensions(int sizeIndex, int* width, int* height)
{
	static const int sizes[5][2] = { { 140, 44 }, { 180, 56 }, { 220, 68 }, { 280, 80 }, { 360, 100 } };
	if (sizeIndex < 0) sizeIndex = 0;
	if (sizeIndex > 4) sizeIndex = 4;
	*width = sizes[sizeIndex][0];
	*height = sizes[sizeIndex][1];
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_PAINT)
	{
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);
		RECT rc;
		GetClientRect(hwnd, &rc);
		HBRUSH bg = CreateSolidBrush(RGB(80, 80, 90));
		HGDIOBJ old = SelectObject(hdc, bg);
		RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 12, 12);
		SelectObject(hdc, old);
		DeleteObject(bg);
		SetBkMode(hdc, TRANSPARENT);
		SetTextColor(hdc, RGB(230, 230, 230));
		{
			static const int fontSizes[] = { 10, 12, 14, 18, 24 };
			int pt = fontSizes[settings.overlay_size >= 0 && settings.overlay_size <= 4 ? settings.overlay_size : 1];
			int logy = GetDeviceCaps(hdc, LOGPIXELSY);
			HFONT font = CreateFontA(-MulDiv(pt, logy, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, "Segoe UI");
			HGDIOBJ oldFont = SelectObject(hdc, font);
			DrawTextA(hdc, "Switchy Disabled", -1, &rc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
			SelectObject(hdc, oldFont);
			DeleteObject(font);
		}
		EndPaint(hwnd, &ps);
		return 0;
	}
	return DefWindowProcA(hwnd, msg, wParam, lParam);
}

HWND CreateOverlayWindow(HINSTANCE hInstance)
{
	WNDCLASSEXA wc = { 0 };
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = OverlayWndProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = OVERLAY_WINDOW_CLASS;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	if (!RegisterClassExA(&wc))
		return NULL;
	int w = 0, h = 0;
	GetOverlayDimensions(settings.overlay_size, &w, &h);
	RECT work = { 0 };
	SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
	int x = work.right - w - 16;
	int y = work.bottom - h - 16;
	HWND hwnd = CreateWindowExA(
		WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_TRANSPARENT,
		OVERLAY_WINDOW_CLASS, "Switchy Overlay",
		WS_POPUP,
		x, y, w, h,
		NULL, NULL, hInstance, NULL);
	if (!hwnd)
		return NULL;
	SetLayeredWindowAttributes(hwnd, 0, OVERLAY_ALPHA, LWA_ALPHA);
	return hwnd;
}

void OverlaySetVisible(BOOL show)
{
	if (!g_hwndOverlay) return;
	if (show)
	{
		OverlayUpdateSize();
		ShowWindow(g_hwndOverlay, SW_SHOWNA);
	}
	else
		ShowWindow(g_hwndOverlay, SW_HIDE);
}

void OverlayUpdateSize(void)
{
	if (!g_hwndOverlay) return;
	int w = 0, h = 0;
	GetOverlayDimensions(settings.overlay_size, &w, &h);
	RECT work = { 0 };
	SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
	int x = work.right - w - 16;
	int y = work.bottom - h - 16;
	SetWindowPos(g_hwndOverlay, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
	InvalidateRect(g_hwndOverlay, NULL, 1);
}


DWORD GetOSVersion()
{
	HMODULE hMod = GetModuleHandleW(L"ntdll.dll");
	RTL_OSVERSIONINFOW osvi = { 0 };

	if (hMod)
	{
		RtlGetVersionPtr p = (RtlGetVersionPtr)(void*)GetProcAddress(hMod, "RtlGetVersion");

		if (p)
		{
			osvi.dwOSVersionInfoSize = sizeof(osvi);
			p((void*)&osvi);
		}
	}

	return osvi.dwMajorVersion;
}


void PressKey(int keyCode)
{
	keybd_event(keyCode, 0, 0, 0);
}


void ReleaseKey(int keyCode)
{
	keybd_event(keyCode, 0, KEYEVENTF_KEYUP, 0);
}


void ToggleCapsLockState()
{
	PressKey(VK_CAPITAL);
	ReleaseKey(VK_CAPITAL);
#if _DEBUG
	printf("Caps Lock state has been toggled\n");
#endif // _DEBUG
}


LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	KBDLLHOOKSTRUCT* key = (KBDLLHOOKSTRUCT*)lParam;
	if (nCode == HC_ACTION && !(key->flags & LLKHF_INJECTED))
	{
#if _DEBUG
		const char* keyStatus = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) ? "pressed" : "released";
		printf("Key %d has been %s\n", key->vkCode, keyStatus);
#endif // _DEBUG
		if (key->vkCode == VK_CAPITAL)
		{
			BOOL isAltHeld = (wParam == WM_SYSKEYDOWN && (key->flags & LLKHF_ALTDOWN));
			BOOL isCtrlHeld = (wParam == WM_KEYDOWN && (GetAsyncKeyState(VK_LCONTROL) & 0x8000));

			/* Alt+Caps or LCtrl+Caps: toggle Switchy enable/disable */
			if ((isAltHeld || isCtrlHeld) && !keystrokeCapsProcessed)
			{
				keystrokeCapsProcessed = TRUE;
				capsUsedForToggle = TRUE;
				enabled = !enabled;

				if (g_hwndTray)
					PostMessage(g_hwndTray, WM_UPDATE_TRAY_ICON, (WPARAM)enabled, 0);

				/* Release modifier so apps don't see ghost press */
				if (isAltHeld)
					keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
				if (isCtrlHeld)
					keybd_event(VK_LCONTROL, 0, KEYEVENTF_KEYUP, 0);

				/* Reset state on re-enable */
				if (enabled)
					keystrokeShiftProcessed = FALSE;

#if _DEBUG
				printf("Switchy has been %s\n", enabled ? "enabled" : "disabled");
#endif // _DEBUG
				/* Ensure Caps Lock is OFF after toggle.
				   If ON -> let key through -> OS toggles it OFF.
				   If OFF -> swallow -> stays OFF. */
				if (GetKeyState(VK_CAPITAL) & 0x0001)
				{
					letCapsKeyThrough = TRUE;
					return CallNextHookEx(hHook, nCode, wParam, lParam);
				}
				return 1;
			}

			/* Caps released */
			if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP)
			{
				keystrokeCapsProcessed = FALSE;

				if (letCapsKeyThrough)
				{
					letCapsKeyThrough = FALSE;
					capsUsedForToggle = FALSE;
					return CallNextHookEx(hHook, nCode, wParam, lParam);
				}

				if (capsUsedForToggle)
				{
					/* Don't switch layout after enable/disable toggle */
					capsUsedForToggle = FALSE;
				}
				else if (enabled)
				{
					if (!keystrokeShiftProcessed)
					{
						/* Caps alone: switch layout */
						if (settings.popup)
						{
							PressKey(VK_LWIN);
							PressKey(VK_SPACE);
							ReleaseKey(VK_SPACE);
							ReleaseKey(VK_LWIN);
						}
						else
						{
							PressKey(VK_MENU);
							PressKey(VK_LSHIFT);
							ReleaseKey(VK_MENU);
							ReleaseKey(VK_LSHIFT);
						}
					}
				}

				keystrokeShiftProcessed = FALSE;
			}

			/* When disabled, pass through CapsLock normally */
			if (!enabled)
			{
				return CallNextHookEx(hHook, nCode, wParam, lParam);
			}

			/* Caps pressed (normal, no Alt/Ctrl).
			   Also handle SYSKEYDOWN without Alt (CapsLock ON edge case). */
			if ((wParam == WM_KEYDOWN || (wParam == WM_SYSKEYDOWN && !(key->flags & LLKHF_ALTDOWN))) && !keystrokeCapsProcessed)
			{
				keystrokeCapsProcessed = TRUE;

				if (keystrokeShiftProcessed)
				{
					ToggleCapsLockState();
				}
			}

			return 1;
		}

		else if (key->vkCode == VK_LSHIFT || key->vkCode == VK_RSHIFT)
		{
			if ((wParam == WM_KEYUP || wParam == WM_SYSKEYUP) && !keystrokeCapsProcessed)
			{
				keystrokeShiftProcessed = FALSE;
			}

			if (!enabled)
			{
				return CallNextHookEx(hHook, nCode, wParam, lParam);
			}

			if (wParam == WM_KEYDOWN && !keystrokeShiftProcessed)
			{
				keystrokeShiftProcessed = TRUE;

				if (keystrokeCapsProcessed)
				{
					ToggleCapsLockState();
				}
			}

			return 0;
		}
	}

	return CallNextHookEx(hHook, nCode, wParam, lParam);
}
