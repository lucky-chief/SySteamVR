//============ Shiyan Naked Monitor OpenVR driver ============
#include "gamepad_input.h"

#include "driverlog.h"
#include "openvr_driver.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <XInput.h>

static const char *kSection = "shiyan_nakedmonitor_gamepad";

namespace
{

struct Settings
{
	bool enabled = true;
	int slot = -1; // -1 = first connected pad
	float deadzone = 0.18f;
	bool rumble = true;
	float rumble_gain = 1.0f;
};

Settings g_settings;
bool g_initialized = false;

bool g_connected = false;
int g_slot = -1;
std::chrono::steady_clock::time_point g_next_scan{};

bool g_rumble_active = false;
std::chrono::steady_clock::time_point g_rumble_until{};

float GetFloatSetting( const char *pchKey, float flDefault )
{
	vr::EVRSettingsError error = vr::VRSettingsError_None;
	const float value = vr::VRSettings()->GetFloat( kSection, pchKey, &error );
	return ( error == vr::VRSettingsError_None ) ? value : flDefault;
}

bool GetBoolSetting( const char *pchKey, bool bDefault )
{
	vr::EVRSettingsError error = vr::VRSettingsError_None;
	const bool value = vr::VRSettings()->GetBool( kSection, pchKey, &error );
	return ( error == vr::VRSettingsError_None ) ? value : bDefault;
}

int32_t GetIntSetting( const char *pchKey, int32_t nDefault )
{
	vr::EVRSettingsError error = vr::VRSettingsError_None;
	const int32_t value = vr::VRSettings()->GetInt32( kSection, pchKey, &error );
	return ( error == vr::VRSettingsError_None ) ? value : nDefault;
}

void StopRumble()
{
	if ( !g_rumble_active || g_slot < 0 )
	{
		g_rumble_active = false;
		return;
	}

	XINPUT_VIBRATION vibration = {};
	XInputSetState( static_cast< DWORD >( g_slot ), &vibration );
	g_rumble_active = false;
}

// Radial deadzone with rescaling so the usable range is still 0 .. 1.
void ApplyDeadzone( SHORT rawX, SHORT rawY, float deadzone, float &outX, float &outY )
{
	float x = static_cast< float >( rawX ) / 32767.0f;
	float y = static_cast< float >( rawY ) / 32767.0f;

	float magnitude = std::sqrt( x * x + y * y );
	if ( magnitude > 1.0f )
	{
		x /= magnitude;
		y /= magnitude;
		magnitude = 1.0f;
	}

	if ( magnitude <= deadzone || magnitude <= 0.0f )
	{
		outX = 0.0f;
		outY = 0.0f;
		return;
	}

	const float scale = ( magnitude - deadzone ) / ( 1.0f - deadzone ) / magnitude;
	outX = x * scale;
	outY = y * scale;
}

float NormalizeTrigger( BYTE value )
{
	// ~12% deadzone so a resting trigger reads exactly 0.
	const float normalized = static_cast< float >( value ) / 255.0f;
	return std::max( 0.0f, ( normalized - 0.12f ) / 0.88f );
}

} // namespace

namespace GamepadInput
{

void Init()
{
	if ( g_initialized )
	{
		return;
	}
	g_initialized = true;

	g_settings.enabled = GetBoolSetting( "enabled", g_settings.enabled );
	g_settings.slot = GetIntSetting( "slot", g_settings.slot );
	g_settings.deadzone = GetFloatSetting( "deadzone", g_settings.deadzone );
	g_settings.deadzone = std::max( 0.0f, std::min( 0.6f, g_settings.deadzone ) );
	g_settings.rumble = GetBoolSetting( "rumble", g_settings.rumble );
	g_settings.rumble_gain = GetFloatSetting( "rumble_gain", g_settings.rumble_gain );
	g_settings.rumble_gain = std::max( 0.0f, std::min( 2.0f, g_settings.rumble_gain ) );

	if ( g_settings.slot < 0 || g_settings.slot >= static_cast< int >( XUSER_MAX_COUNT ) )
	{
		g_settings.slot = -1;
	}

	const std::string slotText = ( g_settings.slot < 0 ) ? "auto" : std::to_string( g_settings.slot );
	DriverLog( "shiyan_nakedmonitor: gamepad input %s (slot %s, deadzone %.2f, rumble %s).",
		g_settings.enabled ? "enabled" : "disabled",
		slotText.c_str(),
		g_settings.deadzone, g_settings.rumble ? "on" : "off" );
}

State Poll()
{
	State out;

	if ( !g_initialized )
	{
		Init();
	}
	if ( !g_settings.enabled )
	{
		return out;
	}

	const auto now = std::chrono::steady_clock::now();

	if ( g_rumble_active && now >= g_rumble_until )
	{
		StopRumble();
	}

	// Scan only when the configured slot is invalid or the pad disappeared.
	if ( g_settings.slot >= 0 )
	{
		g_slot = g_settings.slot;
	}
	else if ( g_slot < 0 && now < g_next_scan )
	{
		return out;
	}

	XINPUT_STATE xinputState = {};
	DWORD result = ERROR_DEVICE_NOT_CONNECTED;

	if ( g_slot >= 0 )
	{
		result = XInputGetState( static_cast< DWORD >( g_slot ), &xinputState );
	}

	if ( result != ERROR_SUCCESS && g_settings.slot < 0 )
	{
		// Auto mode: try every slot once per second.
		for ( DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot )
		{
			if ( XInputGetState( slot, &xinputState ) == ERROR_SUCCESS )
			{
				g_slot = static_cast< int >( slot );
				result = ERROR_SUCCESS;
				break;
			}
		}
	}

	if ( result != ERROR_SUCCESS )
	{
		if ( g_connected )
		{
			g_connected = false;
			StopRumble();
			DriverLog( "shiyan_nakedmonitor: gamepad disconnected." );
		}
		if ( g_settings.slot < 0 )
		{
			g_slot = -1;
			g_next_scan = now + std::chrono::seconds( 1 );
		}
		return out;
	}

	if ( !g_connected )
	{
		g_connected = true;
		DriverLog( "shiyan_nakedmonitor: gamepad connected (slot %d).", g_slot );
	}

	out.connected = true;

	const XINPUT_GAMEPAD &pad = xinputState.Gamepad;
	const WORD buttons = pad.wButtons;

	out.a = ( buttons & XINPUT_GAMEPAD_A ) != 0;
	out.b = ( buttons & XINPUT_GAMEPAD_B ) != 0;
	out.x = ( buttons & XINPUT_GAMEPAD_X ) != 0;
	out.y = ( buttons & XINPUT_GAMEPAD_Y ) != 0;
	out.back = ( buttons & XINPUT_GAMEPAD_BACK ) != 0;
	out.start = ( buttons & XINPUT_GAMEPAD_START ) != 0;
	out.shoulderLeft = ( buttons & XINPUT_GAMEPAD_LEFT_SHOULDER ) != 0;
	out.shoulderRight = ( buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER ) != 0;
	out.leftStickClick = ( buttons & XINPUT_GAMEPAD_LEFT_THUMB ) != 0;
	out.rightStickClick = ( buttons & XINPUT_GAMEPAD_RIGHT_THUMB ) != 0;
	out.dpadUp = ( buttons & XINPUT_GAMEPAD_DPAD_UP ) != 0;
	out.dpadDown = ( buttons & XINPUT_GAMEPAD_DPAD_DOWN ) != 0;
	out.dpadLeft = ( buttons & XINPUT_GAMEPAD_DPAD_LEFT ) != 0;
	out.dpadRight = ( buttons & XINPUT_GAMEPAD_DPAD_RIGHT ) != 0;

	ApplyDeadzone( pad.sThumbLX, pad.sThumbLY, g_settings.deadzone, out.leftX, out.leftY );
	ApplyDeadzone( pad.sThumbRX, pad.sThumbRY, g_settings.deadzone, out.rightX, out.rightY );

	out.leftTrigger = NormalizeTrigger( pad.bLeftTrigger );
	out.rightTrigger = NormalizeTrigger( pad.bRightTrigger );

	return out;
}

void Rumble( float amplitude, float seconds )
{
	if ( !g_settings.enabled || !g_settings.rumble || !g_connected || g_slot < 0 )
	{
		return;
	}

	if ( amplitude <= 0.0f )
	{
		StopRumble();
		return;
	}

	const float clamped = std::min( 1.0f, amplitude * g_settings.rumble_gain );
	const WORD speed = static_cast< WORD >( std::min( 1.0f, clamped ) * 65535.0f );

	XINPUT_VIBRATION vibration = { speed, speed };
	XInputSetState( static_cast< DWORD >( g_slot ), &vibration );

	const auto duration = std::chrono::milliseconds(
		static_cast< long long >( std::max( 0.02f, std::min( 2.0f, seconds ) ) * 1000.0f ) );
	g_rumble_active = true;
	g_rumble_until = std::chrono::steady_clock::now() + duration;
}

} // namespace GamepadInput
