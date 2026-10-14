//============ Shiyan Naked Monitor OpenVR driver ============
#include "device_provider.h"
#include "openvr_driver.h"

#include <cstring>

#if defined( _WIN32 )
#define HMD_DLL_EXPORT extern "C" __declspec( dllexport )
#elif defined( __GNUC__ ) || defined( __APPLE__ )
#define HMD_DLL_EXPORT extern "C" __attribute__( ( visibility( "default" ) ) )
#else
#error "Unsupported platform."
#endif

NakedDeviceProvider device_provider;

//-----------------------------------------------------------------------------
// Entry point called by vrserver when loading the driver.
//-----------------------------------------------------------------------------
HMD_DLL_EXPORT void *HmdDriverFactory( const char *pInterfaceName, int *pReturnCode )
{
	if ( 0 == strcmp( vr::IServerTrackedDeviceProvider_Version, pInterfaceName ) )
	{
		return &device_provider;
	}

	if ( pReturnCode )
	{
		*pReturnCode = vr::VRInitError_Init_InterfaceNotFound;
	}

	return nullptr;
}
