/* Native rebuild support: lib/sylib.h declares these timing buffers extern,
 * while the distributed binary runtime supplies their storage. */
#include <sys/time.h>
struct timeval _sysy_start, _sysy_end;
int _sysy_l1[1024], _sysy_l2[1024];
int _sysy_h[1024], _sysy_m[1024], _sysy_s[1024], _sysy_us[1024];
int _sysy_idx;
