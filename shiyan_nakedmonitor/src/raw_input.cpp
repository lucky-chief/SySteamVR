//============ Shiyan Naked Monitor OpenVR driver ============
#include "raw_input.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace
{

std::atomic<bool> g_running{ false };
HANDLE g_thread = nullptr;
HANDLE g_readyEvent = nullptr;
DWORD g_threadId = 0;
std::mutex g_stateMutex;
RawInput::State g_state;

const wchar_t *kWindowClass = L"ShiyanNakedMonitorRawInput";

void ProcessRawInput( HRAWINPUT hRawInput )
{
	// RAWINPUT for mouse/keyboard is small; an aligned fixed buffer is enough.
	alignas( 8 ) BYTE buffer[ 64 ] = {};
	UINT bytes = sizeof( buffer );
	bytes = GetRawInputData( hRawInput, RID_INPUT, buffer, &bytes, sizeof( RAWINPUTHEADER ) );
	if ( bytes == static_cast< UINT >( -1 ) || bytes < sizeof( RAWINPUTHEADER ) )
	{
		return;
	}

	const RAWINPUT *raw = reinterpret_cast<const RAWINPUT *>( buffer );
	std::lock_guard< std::mutex > lock( g_stateMutex );

	if ( raw->header.dwType == RIM_TYPEMOUSE )
	{
		const RAWMOUSE &mouse = raw->data.mouse;
		g_state.dx += mouse.lLastX;
		g_state.dy += mouse.lLastY;

		const USHORT flags = mouse.usButtonFlags;
		if ( ( flags & RI_MOUSE_LEFT_BUTTON_DOWN ) != 0 )
		{
			g_state.left = true;
		}
		if ( ( flags & RI_MOUSE_LEFT_BUTTON_UP ) != 0 )
		{
			g_state.left = false;
		}
		if ( ( flags & RI_MOUSE_RIGHT_BUTTON_DOWN ) != 0 )
		{
			g_state.right = true;
		}
		if ( ( flags & RI_MOUSE_RIGHT_BUTTON_UP ) != 0 )
		{
			g_state.right = false;
		}
		if ( ( flags & RI_MOUSE_MIDDLE_BUTTON_DOWN ) != 0 )
		{
			g_state.middle = true;
		}
		if ( ( flags & RI_MOUSE_MIDDLE_BUTTON_UP ) != 0 )
		{
			g_state.middle = false;
		}
		if ( ( flags & RI_MOUSE_WHEEL ) != 0 )
		{
			g_state.wheel += static_cast< short >( mouse.usButtonData ) / WHEEL_DELTA;
		}
	}
	else if ( raw->header.dwType == RIM_TYPEKEYBOARD )
	{
		const RAWKEYBOARD &key = raw->data.keyboard;
		const USHORT vkey = key.VKey;
		if ( vkey > 0 && vkey < 256 )
		{
			g_state.keys[ vkey ] = ( key.Flags & RI_KEY_BREAK ) == 0;
		}
	}
}

LRESULT CALLBACK WndProc( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
	switch ( msg )
	{
	case WM_INPUT:
		ProcessRawInput( reinterpret_cast<HRAWINPUT>( lParam ) );
		return 0;
	case WM_DESTROY:
		return 0;
	default:
		return DefWindowProcW( hwnd, msg, wParam, lParam );
	}
}

DWORD WINAPI CaptureThread( LPVOID )
{
	HINSTANCE instance = GetModuleHandleW( nullptr );

	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof( wc );
	wc.lpfnWndProc = WndProc;
	wc.hInstance = instance;
	wc.lpszClassName = kWindowClass;
	RegisterClassExW( &wc );

	// Message-only window: receives WM_INPUT without ever appearing on screen.
	HWND hwnd = CreateWindowExW( 0, kWindowClass, L"", 0, 0, 0, 0, 0,
		HWND_MESSAGE, nullptr, instance, nullptr );
	if ( hwnd == nullptr )
	{
		SetEvent( g_readyEvent );
		return 1;
	}

	// Mouse (page 1 / usage 2) and keyboard (page 1 / usage 6), background
	// delivery (RIDEV_INPUTSINK) so input keeps arriving while SteamVR or a VR
	// app has focus.
	RAWINPUTDEVICE devices[ 2 ] = {};
	devices[ 0 ].usUsagePage = 0x01;
	devices[ 0 ].usUsage = 0x02; // mouse
	devices[ 0 ].dwFlags = RIDEV_INPUTSINK;
	devices[ 0 ].hwndTarget = hwnd;
	devices[ 1 ].usUsagePage = 0x01;
	devices[ 1 ].usUsage = 0x06; // keyboard
	devices[ 1 ].dwFlags = RIDEV_INPUTSINK;
	devices[ 1 ].hwndTarget = hwnd;
	RegisterRawInputDevices( devices, 2, sizeof( devices[ 0 ] ) );

	g_threadId = GetCurrentThreadId();
	g_running = true;
	SetEvent( g_readyEvent ); // main thread may now post to this queue

	MSG msg;
	while ( GetMessageW( &msg, nullptr, 0, 0 ) > 0 )
	{
		TranslateMessage( &msg );
		DispatchMessageW( &msg );
	}

	// Detach raw input from this window before tearing it down.
	RAWINPUTDEVICE remove[ 2 ] = {};
	remove[ 0 ] = devices[ 0 ];
	remove[ 0 ].dwFlags = RIDEV_REMOVE;
	remove[ 0 ].hwndTarget = nullptr;
	remove[ 1 ] = devices[ 1 ];
	remove[ 1 ].dwFlags = RIDEV_REMOVE;
	remove[ 1 ].hwndTarget = nullptr;
	RegisterRawInputDevices( remove, 2, sizeof( remove[ 0 ] ) );

	g_running = false;
	DestroyWindow( hwnd );
	UnregisterClassW( kWindowClass, instance );
	return 0;
}

} // namespace

namespace RawInput
{

bool Start()
{
	if ( g_thread != nullptr )
	{
		return true;
	}

	g_readyEvent = CreateEventW( nullptr, TRUE, FALSE, nullptr );
	if ( g_readyEvent == nullptr )
	{
		return false;
	}

	g_thread = CreateThread( nullptr, 0, CaptureThread, nullptr, 0, nullptr );
	if ( g_thread == nullptr )
	{
		CloseHandle( g_readyEvent );
		g_readyEvent = nullptr;
		return false;
	}

	// Wait (bounded) until the window exists and raw input is registered.
	WaitForSingleObject( g_readyEvent, 2000 );
	return g_running.load();
}

void Shutdown()
{
	if ( g_thread == nullptr )
	{
		return;
	}

	if ( g_running.load() && g_threadId != 0 )
	{
		PostThreadMessageW( g_threadId, WM_QUIT, 0, 0 );
	}
	WaitForSingleObject( g_thread, 3000 );
	CloseHandle( g_thread );
	g_thread = nullptr;
	g_threadId = 0;

	if ( g_readyEvent != nullptr )
	{
		CloseHandle( g_readyEvent );
		g_readyEvent = nullptr;
	}
}

State Consume()
{
	std::lock_guard< std::mutex > lock( g_stateMutex );
	State snapshot = g_state;
	g_state.dx = 0;
	g_state.dy = 0;
	g_state.wheel = 0;
	return snapshot;
}

} // namespace RawInput
