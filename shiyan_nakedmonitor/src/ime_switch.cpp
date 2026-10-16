//============ Shiyan Naked Monitor OpenVR driver ============
#include "ime_switch.h"

#include "driverlog.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

static const char *kImeSection = "shiyan_nakedmonitor_ime";

namespace
{

struct ImeState
{
	bool initialized = false;
	bool enabled = true;

	// Lowercase process base names whose windows get switched to English.
	std::vector< std::string > processes;

	// Target keyboard layout, as a KLID string ("00000409" = US English).
	std::string layout_klid = "00000409";
	HKL target_hkl = nullptr;

	std::chrono::steady_clock::time_point last_check{};
	HWND last_window = nullptr;
	std::chrono::steady_clock::time_point last_log{};
};

ImeState g_ime;

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

		// ASCII lowercase by hand (process names are ASCII).
		for ( wchar_t c : stem )
		{
			name.push_back( static_cast< char >( ( c >= L'A' && c <= L'Z' ) ? ( c - L'A' + L'a' ) : c ) );
		}
	}
	CloseHandle( process );
	return name;
}

void SplitProcessList( const std::string &list )
{
	g_ime.processes.clear();
	size_t start = 0;
	while ( start <= list.size() )
	{
		const size_t comma = list.find( ',', start );
		std::string item = ( comma == std::string::npos ) ? list.substr( start )
														  : list.substr( start, comma - start );
		// trim
		const size_t first = item.find_first_not_of( " \t" );
		const size_t last = item.find_last_not_of( " \t" );
		item = ( first == std::string::npos ) ? std::string() : item.substr( first, last - first + 1 );
		if ( !item.empty() )
		{
			g_ime.processes.push_back( ToLowerAscii( item ) );
		}
		if ( comma == std::string::npos )
		{
			break;
		}
		start = comma + 1;
	}
}

void GetStringSetting( const char *pchKey, const char *pchDefault, std::string &out )
{
	char buffer[ 1024 ] = {};
	vr::EVRSettingsError error = vr::VRSettingsError_None;
	vr::VRSettings()->GetString( kImeSection, pchKey, buffer, sizeof( buffer ), &error );
	out = ( error == vr::VRSettingsError_None ) ? buffer : pchDefault;
}

} // namespace

namespace ImeSwitch
{

void Init()
{
	if ( g_ime.initialized )
	{
		return;
	}
	g_ime.initialized = true;

	vr::EVRSettingsError error = vr::VRSettingsError_None;
	g_ime.enabled = vr::VRSettings()->GetBool( kImeSection, "enabled", &error );
	if ( error != vr::VRSettingsError_None )
	{
		g_ime.enabled = true;
	}

	std::string processes;
	GetStringSetting( "processes",
		"vrcompositor,vrmonitor,vrdashboard,vrwebhelper,vrserver,steamwebhelper,steamtours",
		processes );
	SplitProcessList( processes );

	GetStringSetting( "keyboard_layout", "00000409", g_ime.layout_klid );

	// Load the layout so we have a valid HKL to post; the low word is the
	// language ID, which is what the target thread will activate.
	const std::wstring klid( g_ime.layout_klid.begin(), g_ime.layout_klid.end() );
	g_ime.target_hkl = LoadKeyboardLayoutW( klid.c_str(), KLF_NOTELLSHELL );
	if ( g_ime.target_hkl == nullptr && g_ime.layout_klid.size() >= 8 )
	{
		const long langId = std::strtol( g_ime.layout_klid.substr( 4, 4 ).c_str(), nullptr, 16 );
		if ( langId > 0 )
		{
			g_ime.target_hkl = reinterpret_cast< HKL >( ( static_cast< uintptr_t >( langId ) << 16 )
														| static_cast< uintptr_t >( langId ) );
		}
	}

	DriverLog( "shiyan_nakedmonitor: ime switch (%s; layout %s hkl=0x%p; %zu processes).",
		g_ime.enabled ? "enabled" : "disabled",
		g_ime.layout_klid.c_str(), reinterpret_cast< void * >( g_ime.target_hkl ),
		g_ime.processes.size() );
}

void Update()
{
	if ( !g_ime.initialized )
	{
		Init();
	}
	if ( !g_ime.enabled || g_ime.target_hkl == nullptr )
	{
		return;
	}

	const auto now = std::chrono::steady_clock::now();
	if ( g_ime.last_check != std::chrono::steady_clock::time_point{}
		 && now - g_ime.last_check < std::chrono::milliseconds( 500 ) )
	{
		return;
	}
	g_ime.last_check = now;

	HWND window = GetForegroundWindow();
	if ( window == nullptr )
	{
		return;
	}

	DWORD pid = 0;
	const DWORD tid = GetWindowThreadProcessId( window, &pid );
	if ( pid == 0 || pid == GetCurrentProcessId() )
	{
		return;
	}

	const std::string process = GetProcessBaseName( pid );
	if ( process.empty()
		 || std::find( g_ime.processes.begin(), g_ime.processes.end(), process ) == g_ime.processes.end() )
	{
		return;
	}

	// The low word of an HKL is its language ID: 0x0804 = zh-CN, 0x0409 = en-US.
	const uintptr_t currentLang = reinterpret_cast< uintptr_t >( GetKeyboardLayout( tid ) ) & 0xFFFF;
	const uintptr_t targetLang = reinterpret_cast< uintptr_t >( g_ime.target_hkl ) & 0xFFFF;
	if ( currentLang == targetLang )
	{
		return; // already English
	}

	PostMessageW( window, WM_INPUTLANGCHANGEREQUEST, 0, reinterpret_cast< LPARAM >( g_ime.target_hkl ) );

	if ( window != g_ime.last_window
		 || g_ime.last_log == std::chrono::steady_clock::time_point{}
		 || now - g_ime.last_log >= std::chrono::seconds( 5 ) )
	{
		g_ime.last_window = window;
		g_ime.last_log = now;
		DriverLog( "shiyan_nakedmonitor: input language -> %s for '%s' (hwnd 0x%p)",
			g_ime.layout_klid.c_str(), process.c_str(), reinterpret_cast< void * >( window ) );
	}
}

} // namespace ImeSwitch
