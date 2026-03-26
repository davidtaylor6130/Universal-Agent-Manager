@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B build -DUAM_FETCH_DEPS=ON -DUAM_BUILD_TESTS=OFF -DUAM_WINDOWS_GUI_SUBSYSTEM=ON
