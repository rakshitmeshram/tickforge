@echo off
rem Runs zig ranlib from the project .venv (pip install ziglang) so CMake can use it as a compiler.
"%~dp0..\..\.venv\Scripts\python.exe" -m ziglang ranlib %*
