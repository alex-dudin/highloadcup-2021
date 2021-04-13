# Solution for the "Gold Rush" contest (HighLoad Cup 2021)

Place: 2

Final score: 13186674

## How to build solution using Visual Studio 2019

1) Install the prerequisites
   - CMake 3.17+ (https://cmake.org/)
   - vcpkg (https://github.com/Microsoft/vcpkg/)

2) Generate Visual Studio solution
   ```bat
   cmake -G "Visual Studio 16 2019" -Thost=x64 -Ax64 -B build -S . -DCMAKE_TOOLCHAIN_FILE=%VCPKG_DIR%\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
   ```
 
3) Build Visual Studio solution
   ```bat
   cmake --build build --config Release
   ```

## Links
- [Official site](https://cups.mail.ru/en/contests/goldrush)
- [Official repository](https://github.com/All-Cups/highloadcup/tree/main/goldrush)
- [Final round results](https://cups.mail.ru/en/results/goldrush?period=past&roundId=599)
- [Other soultions](https://github.com/proton/highloadcup21_solutions)
