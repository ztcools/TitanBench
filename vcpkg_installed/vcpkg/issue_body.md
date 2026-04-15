Package: wolfssl:x64-linux@5.8.4

**Host Environment**

- Host: x64-linux
- Compiler: GNU 11.4.0
- CMake Version: 4.3.1
-    vcpkg-tool version: 2025-11-19-da1f056dc0775ac651bea7e3fbbf4066146a55f3
    vcpkg-scripts version: 5bf0c55239 2025-11-28 (5 months ago)

**To Reproduce**

`vcpkg install `

**Failure logs**

```
Downloading https://github.com/wolfssl/wolfssl/archive/v5.8.4-stable.tar.gz -> wolfssl-wolfssl-v5.8.4-stable.tar.gz
Successfully downloaded wolfssl-wolfssl-v5.8.4-stable.tar.gz
-- Extracting source /home/zt/tools/vcpkg/downloads/wolfssl-wolfssl-v5.8.4-stable.tar.gz
-- Using source at /home/zt/tools/vcpkg/buildtrees/wolfssl/src/8.4-stable-00f9bb5448.clean
-- Getting CMake variables for x64-linux
-- Loading CMake variables from /home/zt/tools/vcpkg/buildtrees/wolfssl/cmake-get-vars_C_CXX-x64-linux.cmake.log
-- Configuring x64-linux
-- Building x64-linux-dbg
-- Building x64-linux-rel
-- Fixing pkgconfig file: /home/zt/tools/vcpkg/packages/wolfssl_x64-linux/lib/pkgconfig/wolfssl.pc
CMake Error at scripts/cmake/vcpkg_find_acquire_program.cmake:179 (message):
  Could not find pkg-config.  Please install it via your package manager:

      sudo apt-get install pkg-config
Call Stack (most recent call first):
  scripts/cmake/vcpkg_fixup_pkgconfig.cmake:193 (vcpkg_find_acquire_program)
  ports/wolfssl/portfile.cmake:76 (vcpkg_fixup_pkgconfig)
  scripts/ports.cmake:206 (include)



```

**Additional context**

<details><summary>vcpkg.json</summary>

```
{
  "name": "titanbench",
  "version-string": "0.1.0",
  "dependencies": [
    "wolfssl"
  ]
}

```
</details>
