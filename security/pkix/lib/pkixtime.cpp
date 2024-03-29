/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "pkix/Time.h"
#include "pkix/pkixutil.h"

#ifdef _WINDOWS
#ifdef _MSC_VER
#pragma warning(push, 3)
#endif
#include "windows.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#else
#include "sys/time.h"
#endif

namespace mozilla { namespace pkix {

Time
Now()
{
  uint64_t seconds;

#ifdef _WINDOWS
  // "Contains a 64-bit value representing the number of 100-nanosecond
  // intervals since January 1, 1601 (UTC)."
  //   - http://msdn.microsoft.com/en-us/library/windows/desktop/ms724284(v=vs.85).aspx
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  uint64_t ft64 = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) |
                  ft.dwLowDateTime;
  seconds = (DaysBeforeYear(1601) * Time::ONE_DAY_IN_SECONDS) +
            ft64 / (1000u * 1000u * 1000u / 100u);
#else
  // "The gettimeofday() function shall obtain the current time, expressed as
  // seconds and microseconds since the Epoch."
  //   - http://pubs.opengroup.org/onlinepubs/009695399/functions/gettimeofday.html
  timeval tv;
  (void) gettimeofday(&tv, nullptr);
  seconds = (DaysBeforeYear(1970) * Time::ONE_DAY_IN_SECONDS) +
            static_cast<uint64_t>(tv.tv_sec);
#endif

  return TimeFromElapsedSecondsAD(seconds);
}

Time
TimeFromEpochInSeconds(uint64_t secondsSinceEpoch)
{
  uint64_t seconds = (DaysBeforeYear(1970) * Time::ONE_DAY_IN_SECONDS) +
                     secondsSinceEpoch;
  return TimeFromElapsedSecondsAD(seconds);
}

} } // namespace mozilla::pkix
