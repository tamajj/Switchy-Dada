#include <Windows.h>
#if _DEBUG
#include <stdio.h>
#endif // _DEBUG

typedef NTSTATUS(WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);

typedef struct {
	BOOL popup;
} Settings;

void ShowError(LPCSTR message);
DWORD GetOSVersion();
void PressKey(int keyCode);
void ReleaseKey(int keyCode);
void ToggleCapsLockState();
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);


HHOOK hHook;
BOOL enabled = TRUE;
BOOL keystrokeCapsProcessed = FALSE;
BOOL keystrokeShiftProcessed = FALSE;
BOOL capsUsedForToggle = FALSE;
BOOL pendingCapsReset = FALSE;

Settings settings = {
	.popup = FALSE
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

	HANDLE hMutex = CreateMutex(0, 0, "Switchy");
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

	MSG messages;
	while (GetMessage(&messages, NULL, 0, 0))
	{
		TranslateMessage(&messages);
		DispatchMessage(&messages);
	}

	UnhookWindowsHookEx(hHook);

	return 0;
}


void ShowError(LPCSTR message)
{
	MessageBox(NULL, message, "Error", MB_OK | MB_ICONERROR);
}


DWORD GetOSVersion()
{
	HMODULE hMod = GetModuleHandleW(L"ntdll.dll");
	RTL_OSVERSIONINFOW osvi = { 0 };

	if (hMod)
	{
		RtlGetVersionPtr p = (RtlGetVersionPtr)GetProcAddress(hMod, "RtlGetVersion");

		if (p)
		{
			osvi.dwOSVersionInfoSize = sizeof(osvi);
			p(&osvi);
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
			BOOL isAltHeld = (wParam == WM_SYSKEYDOWN);
			BOOL isCtrlHeld = (wParam == WM_KEYDOWN && (GetAsyncKeyState(VK_LCONTROL) & 0x8000));

			// Alt+Caps or LCtrl+Caps: toggle Switchy enable/disable
			if ((isAltHeld || isCtrlHeld) && !keystrokeCapsProcessed)
			{
				keystrokeCapsProcessed = TRUE;
				capsUsedForToggle = TRUE;
				enabled = !enabled;

				// Reset state on re-enable, defer CapsLock reset to keyup
				if (enabled)
				{
					keystrokeShiftProcessed = FALSE;
					pendingCapsReset = TRUE;
				}
#if _DEBUG
				printf("Switchy has been %s\n", enabled ? "enabled" : "disabled");
#endif // _DEBUG
				return 1;
			}

			// Caps released
			if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP)
			{
				keystrokeCapsProcessed = FALSE;

				if (capsUsedForToggle)
				{
					// Don't switch layout after enable/disable toggle
					capsUsedForToggle = FALSE;

					// Force CapsLock OFF on re-enable (deferred from keydown)
					if (pendingCapsReset)
					{
						pendingCapsReset = FALSE;
						if (GetKeyState(VK_CAPITAL) & 1)
						{
							ToggleCapsLockState();
						}
					}
				}
				else if (enabled)
				{
					if (!keystrokeShiftProcessed)
					{
						// Caps alone: switch layout
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

			// When disabled, pass through CapsLock normally
			if (!enabled)
			{
				return CallNextHookEx(hHook, nCode, wParam, lParam);
			}

			// Caps pressed (normal, no Alt/Ctrl)
			if (wParam == WM_KEYDOWN && !keystrokeCapsProcessed)
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
