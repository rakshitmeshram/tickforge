@echo off
rem Runs zig ar from the project .venv (pip install ziglang) so CMake can use it as a compiler.
"%~dp0..\..\.venv\Scripts\python.exe" -m ziglang ar %*
