@echo off
rem Starts the offline TF2 mod from this folder.
rem Steam must be running, with Team Fortress 2 and Source SDK Base 2013
rem Multiplayer installed. Anything you add after the file name (for example
rem -windowed or -novid) is passed on to the game. -condebug keeps a log of
rem the console in mod_tf\console.log, which helps when something goes wrong.
start "" "%~dp0mod_tf_win64.exe" -game "%~dp0mod_tf" -condebug %*
