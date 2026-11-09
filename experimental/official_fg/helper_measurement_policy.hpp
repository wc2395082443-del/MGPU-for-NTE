#pragma once

namespace mgpu::official_fg
{
#ifdef MGPU_HELPER_NO_TRACE
inline constexpr bool helper_measurements = false;
#else
inline constexpr bool helper_measurements = true;
#endif
}
