//============ Shiyan Naked Monitor OpenVR driver ============
#pragma once

#include <algorithm>
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

// Rotation whose -Z axis points along `dir` (shortest arc, up stays level).
inline void FromForward( const float dir[ 3 ], float outQuat[ 4 ] )
{
	const float len = std::sqrt( dir[ 0 ] * dir[ 0 ] + dir[ 1 ] * dir[ 1 ] + dir[ 2 ] * dir[ 2 ] );
	if ( len < 1e-6f )
	{
		outQuat[ 0 ] = 0.0f;
		outQuat[ 1 ] = 0.0f;
		outQuat[ 2 ] = 0.0f;
		outQuat[ 3 ] = 1.0f;
		return;
	}

	const float fx = dir[ 0 ] / len;
	const float fy = dir[ 1 ] / len;
	const float fz = dir[ 2 ] / len;

	// from = -Z; dot = -fz; axis = from x dir.
	const float dot = std::max( -1.0f, std::min( 1.0f, -fz ) );
	float ax = fy;
	float ay = -fx;
	float az = 0.0f;
	const float axisLen = std::sqrt( ax * ax + ay * ay + az * az );
	if ( axisLen < 1e-6f )
	{
		if ( dot > 0.0f )
		{
			outQuat[ 0 ] = 0.0f;
			outQuat[ 1 ] = 0.0f;
			outQuat[ 2 ] = 0.0f;
			outQuat[ 3 ] = 1.0f;
		}
		else
		{
			outQuat[ 0 ] = 0.0f;
			outQuat[ 1 ] = 1.0f;
			outQuat[ 2 ] = 0.0f;
			outQuat[ 3 ] = 0.0f; // 180 degrees around +Y
		}
		return;
	}

	const float invAxisLen = 1.0f / axisLen;
	ax *= invAxisLen;
	ay *= invAxisLen;
	az *= invAxisLen;
	const float halfAngle = std::acos( dot ) * 0.5f;
	const float s = std::sin( halfAngle );
	outQuat[ 0 ] = ax * s;
	outQuat[ 1 ] = ay * s;
	outQuat[ 2 ] = az * s;
	outQuat[ 3 ] = std::cos( halfAngle );
}

// out = a * b (Hamilton product, all quaternions are (x, y, z, w)).
inline void Multiply( const float a[ 4 ], const float b[ 4 ], float out[ 4 ] )
{
	const float ax = a[ 0 ], ay = a[ 1 ], az = a[ 2 ], aw = a[ 3 ];
	const float bx = b[ 0 ], by = b[ 1 ], bz = b[ 2 ], bw = b[ 3 ];
	out[ 0 ] = aw * bx + ax * bw + ay * bz - az * by;
	out[ 1 ] = aw * by - ax * bz + ay * bw + az * bx;
	out[ 2 ] = aw * bz + ax * by - ay * bx + az * bw;
	out[ 3 ] = aw * bw - ax * bx - ay * by - az * bz;
}

// Inverse of a unit quaternion = conjugate.
inline void Conjugate( const float q[ 4 ], float out[ 4 ] )
{
	out[ 0 ] = -q[ 0 ];
	out[ 1 ] = -q[ 1 ];
	out[ 2 ] = -q[ 2 ];
	out[ 3 ] = q[ 3 ];
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
