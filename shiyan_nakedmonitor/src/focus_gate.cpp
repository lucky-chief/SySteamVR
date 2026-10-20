//============ Shiyan Naked Monitor OpenVR driver ============
#include "focus_gate.h"

#include "driverlog.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

static const char *kFocusSection = "shiyan_nakedmonitor_focus";

namespace
{

struct FocusState
{
	bool initialized = false;
	bool enabled = true;

	// Lowercase process base names whose windows may drive the simulator.
	std::vector< std::string > processes;

	bool allow_output_window = true;
	bool allow_fullscreen = true;

	std::chrono::steady_clock::time_point last_check{};
	bool active = false;
	std::string last_reason = "?";
};

FocusState g_focus;

std::string ToLowerAscii( std::string value )
{
	std::transform( value.begin(), value.end(), value.begin(),
		[]( unsigned char c ) { return static_cast< char >( std::tolower( c ) ); } );
	return value;
}

std::string GetProcessBaseName( DWORD pid )
{
	HANDLE process = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid );
	if ( process == nullptr )
	{
		return std::string();
	}

	wchar_t path[ MAX_PATH * 2 ] = {};
	DWORD size = ARRAYSIZE( path );
	std::string name;
	if ( QueryFullProcessImageNameW( process, 0, path, &size ) )
	{
		const std::wstring full( path, size );
		const size_t slash = full.find_last_of( L"\\/" );
		const std::wstring base = ( slash == std::wstring::npos ) ? full : full.substr( slash + 1 );
		const size_t dot = base.find_last_of( L'.' );
		const std::wstring stem = ( dot == std::wstring::npos ) ? base : base.substr( 0, dot );
		for ( wchar_t c : stem )
		{
			name.push_back( static_cast< char >( ( c >= L'A' && c <= L'Z' ) ? ( c - L'A' + L'a' ) : c ) );
		}
	}
	CloseHandle( process );
	return name;
}

std::string GetWindowClass( HWND window )
{
	wchar_t cls[ 128 ] = {};
	GetClassNameW( window, cls, ARRAYSIZE( cls ) );
	std::string out;
	for ( wchar_t c : std::wstring( cls ) )
	{
		out.push_back( static_cast< char >( ( c < 0x80 ) ? c : '?' ) );
	}
	return out;
}

// Desktop shell windows (Progman/WorkerW/taskbar) cover a whole monitor but
// must never count as "a fullscreen application".
bool IsShellWindow( HWND window )
{
	const std::string cls = GetWindowClass( window );
	return cls == "Progman" || cls == "WorkerW" || cls == "Shell_TrayWnd"
		|| cls == "Shell_SecondaryTrayWnd" || cls == "Windows.UI.Core.CoreWindow";
}

bool CoversWholeMonitor( HWND window )
{
	// A maximized normal window slightly exceeds the monitor rect (invisible
	// borders), so a coverage test alone would flag every maximized editor or
	// browser as "fullscreen". Real fullscreen windows are borderless.
	const LONG_PTR style = GetWindowLongPtrW( window, GWL_STYLE );
	if ( ( style & ( WS_CAPTION | WS_THICKFRAME | WS_DLGFRAME | WS_BORDER ) ) != 0 )
	{
		return false;
	}

	RECT rect = {};
	if ( !GetWindowRect( window, &rect ) )
	{
		return false;
	}
	const HMONITOR monitor = MonitorFromWindow( window, MONITOR_DEFAULTTONEAREST );
	MONITORINFO info = {};
	info.cbSize = sizeof( info );
	if ( !GetMonitorInfoW( monitor, &info ) )
	{
		return false;
	}
	const double width = static_cast< double >( rect.right - rect.left );
	const double height = static_cast< double >( rect.bottom - rect.top );
	const double monitorWidth = static_cast< double >( info.rcMonitor.right - info.rcMonitor.left );
	const double monitorHeight = static_cast< double >( info.rcMonitor.bottom - info.rcMonitor.top );
	if ( monitorWidth <= 0.0 || monitorHeight <= 0.0 )
	{
		return false;
	}
	return width >= monitorWidth * 0.99 && height >= monitorHeight * 0.99;
}

void GetStringSetting( const char *pchKey, const char *pchDefault, std::string &out )
{
	char buffer[ 1024 ] = {};
	vr::EVRSettingsError error = vr::VRSettingsError_None;
	vr::VRSettings()->GetString( kFocusSection, pchKey, buffer, sizeof( buffer ), &error );
	out = ( error == vr::VRSettingsError_None ) ? buffer : pchDefault;
}

void SplitList( const std::string &list )
{
	g_focus.processes.clear();
	size_t start = 0;
	while ( start <= list.size() )
	{
		const size_t comma = list.find( ',', start );
		std::string item = ( comma == std::string::npos ) ? list.substr( start )
														  : list.substr( start, comma - start );
		const size_t first = item.find_first_not_of( " \t" );
		const size_t last = item.find_last_not_of( " \t" );
		item = ( first == std::string::npos ) ? std::string() : item.substr( first, last - first + 1 );
		if ( !item.empty() )
		{
			g_focus.processes.push_back( ToLowerAscii( item ) );
		}
		if ( comma == std::string::npos )
		{
			break;
		}
		start = comma + 1;
	}
}

} // namespace

namespace FocusGate
{

void Init()
{
	if ( g_focus.initialized )
	{
		return;
	}
	g_focus.initialized = true;

	vr::EVRSettingsError error = vr::VRSettingsError_None;
	g_focus.enabled = vr::VRSettings()->GetBool( kFocusSection, "enabled", &error );
	if ( error != vr::VRSettingsError_None )
	{
		g_focus.enabled = true;
	}

	std::string processes;
	GetStringSetting( "processes",
		"vrserver,vrcompositor,vrmonitor,vrdashboard,vrwebhelper,steamwebhelper,steamtours,steam",
		processes );
	SplitList( processes );

	auto getBool = []( const char *key, bool fallback ) -> bool {
		vr::EVRSettingsError err = vr::VRSettingsError_None;
		const bool value = vr::VRSettings()->GetBool( kFocusSection, key, &err );
		return ( err == vr::VRSettingsError_None ) ? value : fallback;
	};
	g_focus.allow_output_window = getBool( "allow_output_window", true );
	g_focus.allow_fullscreen = getBool( "allow_fullscreen", true );

	DriverLog( "shiyan_nakedmonitor: input focus gate (%s; %zu process(es); output window %s; fullscreen %s).",
		g_focus.enabled ? "enabled" : "disabled", g_focus.processes.size(),
		g_focus.allow_output_window ? "allowed" : "ignored",
		g_focus.allow_fullscreen ? "allowed" : "ignored" );
}

bool IsInputActive()
{
	if ( !g_focus.initialized )
	{
		Init();
	}
	if ( !g_focus.enabled )
	{
		return true;
	}

	const auto now = std::chrono::steady_clock::now();
	if ( g_focus.last_check != std::chrono::steady_clock::time_point{}
		 && now - g_focus.last_check < std::chrono::milliseconds( 100 ) )
	{
		return g_focus.active;
	}
	g_focus.last_check = now;

	const HWND foreground = GetForegroundWindow();
	bool active = false;
	std::string reason = "none";

	if ( foreground != nullptr )
	{
		const std::string cls = GetWindowClass( foreground );
		if ( g_focus.allow_output_window && cls == "ShiyanNakedMonitorOutput" )
		{
			active = true;
			reason = "VR output window";
		}
		else
		{
			DWORD pid = 0;
			GetWindowThreadProcessId( foreground, &pid );
			const std::string process = ( pid != 0 && pid != GetCurrentProcessId() )
				? GetProcessBaseName( pid )
				: std::string();
			if ( !process.empty()
				 && std::find( g_focus.processes.begin(), g_focus.processes.end(), process )
						!= g_focus.processes.end() )
			{
				active = true;
				reason = process;
			}
			else if ( g_focus.allow_fullscreen && !IsShellWindow( foreground )
					  && CoversWholeMonitor( foreground ) )
			{
				active = true;
				reason = process.empty() ? "fullscreen window" : ( process + " (fullscreen)" );
			}
			else
			{
				reason = process.empty() ? ( cls.empty() ? "unknown" : cls ) : process;
			}
		}
	}

	if ( active != g_focus.active || reason != g_focus.last_reason )
	{
		g_focus.active = active;
		g_focus.last_reason = reason;
		DriverLog( "shiyan_nakedmonitor: input focus %s (%s).",
			active ? "active" : "blocked", reason.c_str() );
	}
	g_focus.active = active;
	return active;
}

} // namespace FocusGate
