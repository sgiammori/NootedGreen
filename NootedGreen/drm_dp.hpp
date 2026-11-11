// The DPCD addresses this driver names, spelled as the kernel's include/drm/display/drm_dp.h spells them:
// https://github.com/torvalds/linux/blob/65ba8f3/include/drm/display/drm_dp.h
#ifndef drm_dp_hpp
#define drm_dp_hpp

#define DP_SUPPORTED_LINK_RATES		    0x010 /* eDP 1.4 */
# define DP_MAX_SUPPORTED_RATES		     8	    /* 16-bit little-endian */

#define DP_LINK_BW_SET		            0x100
#define DP_LANE_COUNT_SET	            0x101
#define DP_EDP_CONFIGURATION_SET            0x10a   /* XXX 1.2? */
#define DP_LINK_RATE_SET		    0x115   /* eDP 1.4 */

#define DP_SOURCE_OUI			    0x300

#define DP_SET_POWER                        0x600
# define DP_SET_POWER_D0                    0x1
# define DP_SET_POWER_D3                    0x2

#endif
