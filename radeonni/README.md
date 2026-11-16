# radeonni/: the userland Firebrand speaks

Firebrand's accelerator offers the same user clients as the RadeonNI driver
([RadeonNI-OSX](https://github.com/kouta-kun/RadeonNI-OSX)), so RadeonNI's userland
runs on top of it unchanged. This directory has the parts a Firebrand installation uses,
from RadeonNI-OSX with this project's changes (radeonsi on a big-endian host, Polaris):

| Path | What | Built by |
|---|---|---|
| `gld/` | `RadeonNIGLDriver.bundle`, the OpenGL driver: Apple's GL driver interface on the outside, Mesa inside | `build-gl.sh` (cross, GCC 15) |
| `mesa/patches/` | the changes to Mesa 26.2.4; 0030-0047 run radeonsi (GCN cards) on a big-endian host (0047: buffer textures, and with them OpenGL 4.6 / 4.5) | `build-gl.sh` |
| `mesa/winsys/`, `mesa/frontend/`, `mesa/target/` | the gallium winsys over the kext's user client (no DRM), the frontend, the bundle's target | `build-gl.sh` |
| `mesa/darwin8/` | what Mesa needs and Mac OS X 10.5 lacks (`clock_gettime`, `posix_memalign`, TLS helpers, ...) | `build-gl.sh` |
| `ga/` | `RadeonNIGA.plugin`, the window server's 2D plug-in | `scripts/build-mac.sh` (Xcode, on the Mac) |
| `video/` | the `'rdnv'` client library: H.264 through the card's UVD decoder, for programs (FrostByte compiles it in) | compiled into the program that uses it |
| `hw/` | the user-client protocol (`rdn_user.h`, `rdn_video.h`, `rdn_uvd_h264.h`) and the memory allocator the bundle uses | with the above, and with the kext |

The names stay RadeonNI's on purpose: the system and existing programs find the bundle and
the plug-in by those names (`IOGLBundleName`, `IOCFPlugInTypes`), and the same OpenGL bundle
serves RadeonNI's cards (SI/CIK) as well; Firebrand itself now drives SI and CIK cards too.

Licence: MIT, [LICENSE](LICENSE) (RadeonNI-OSX's). The Mesa patches are under the licence of
the Mesa files they change (MIT).
