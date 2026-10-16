//============ Shiyan Naked Monitor OpenVR driver ============
#pragma once

#include <cmath>

//-----------------------------------------------------------------------------
// Minimal quaternion helpers shared by the HMD pose and the mouse ray.
// Convention: tracking space, -Z forward, +Y up; quaternions are (x, y, z, w),
// the same order OpenVR's HmdQuaternion_t uses.
//-----------------------------------------------------------------------------
namespace Quat
{

// Ry(yaw) * Rx(pitch): positive yaw turns left, positive pitch looks up.
inline void FromYawPitch( float yaw, float pitch, float outQuat[ 4 ] )
{
	const float halfYaw = yaw * 0.5f;
	const float halfPitch = pitch * 0.5f;
	const float cy = std::cos( halfYaw );
	const float sy = std::sin( halfYaw );
	const float cp = std::cos( halfPitch );
	const float sp = std::sin( halfPitch );
	outQuat[ 0 ] = cy * sp;  // x
	outQuat[ 1 ] = cp * sy;  // y
	outQuat[ 2 ] = -sy * sp; // z
	outQuat[ 3 ] = cy * cp;  // w
}

// v' = q * v * q^-1 (via the t = 2*(q_vec x v) form).
inline void RotateVector( const float q[ 4 ], const float v[ 3 ], float out[ 3 ] )
{
	const float x = q[ 0 ];
	const float y = q[ 1 ];
	const float z = q[ 2 ];
	const float w = q[ 3 ];

	const float tx = 2.0f * ( y * v[ 2 ] - z * v[ 1 ] );
	const float ty = 2.0f * ( z * v[ 0 ] - x * v[ 2 ] );
	const float tz = 2.0f * ( x * v[ 1 ] - y * v[ 0 ] );

	out[ 0 ] = v[ 0 ] + w * tx + ( y * tz - z * ty );
	out[ 1 ] = v[ 1 ] + w * ty + ( z * tx - x * tz );
	out[ 2 ] = v[ 2 ] + w * tz + ( x * ty - y * tx );
}

} // namespace Quat
