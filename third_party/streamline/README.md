# Streamline SDK

`include/` holds the NVIDIA Streamline SDK headers (MIT, see `STREAMLINE_LICENSE.txt`) that the proxy
compiles against. `SDK_VERSION` pins the SDK release the build scripts download: line 1 is the
version, line 2 the SHA-256 of `streamline-sdk-v<version>.zip` from
https://github.com/NVIDIA-RTX/Streamline/releases.

The runtime DLLs are not committed. `src/fetch_deps.sh` (CI, MSYS2) and `src/build.ps1` (local)
download the pinned zip, verify the hash, and copy the plugins the mod needs into the package:

```
sl.interposer.dll  sl.common.dll  sl.dlss.dll  sl.dlss_g.dll  sl.pcl.dll  sl.reflex.dll
nvngx_dlss.dll     nvngx_dlssg.dll            (DLSS / DLSS-G models, NVIDIA RTX SDKs license)
```

To move to a newer Streamline: replace `include/` with the new SDK's headers, update `SDK_VERSION`
(version and hash), rebuild, and re-test. Note the DLSS model DLLs (`nvngx_*`) come from the same zip,
so one pin covers both Streamline and DLSS.
