if not exist "build" mkdir "build"
cl /O2 /std:c++20 /EHsc /Zi /Fo:build\ /Fd:build\ /Fe:build\threading.exe threading.cpp user32.lib
