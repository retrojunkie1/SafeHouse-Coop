@echo off
rem Sly 2 co-op over Steam (SafeHouse-Coop): needs Steam running and steam_api64.dll beside Sly2-Coop.exe.
rem Hosting starts on its own; F8 in game opens Steam's invite dialog.
cd /d "%~dp0"
start "" "%~dp0Sly2-Coop.exe" --data-dir "%~dp0..\data\installations\sly2" --user-dir "%~dp0..\data\user\sly2" --coop-steam
