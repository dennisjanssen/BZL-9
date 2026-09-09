@echo off
setlocal

rem BZL-9 notifier for Claude Code hooks.
rem
rem   bzl9.cmd working     Claude has started a turn
rem   bzl9.cmd waiting     Claude has finished and is waiting for you
rem   bzl9.cmd attention   Claude is blocked on your approval
rem   bzl9.cmd test        one-off connectivity check (prints the reply)
rem
rem Deliberately a script rather than a curl call inlined into
rem settings.json. Two reasons:
rem   1. It ALWAYS exits 0. `Stop` is a blockable hook event -- a hook
rem      exiting with code 2 stops Claude's turn -- and curl uses exit 2
rem      for "failed to initialize". A desk ornament being unplugged must
rem      never be able to hang the assistant.
rem   2. It removes the nested JSON-inside-JSON quoting that inlining
rem      needs, and it does not care which shell Claude Code invokes.

set HOST=bzl9.local
set CURL=curl -s --max-time 2 -H "Content-Type: application/json"

if "%~1"=="working"   goto :working
if "%~1"=="waiting"   goto :waiting
if "%~1"=="attention" goto :attention
if "%~1"=="test"      goto :test
goto :done

:working
%CURL% -X POST "http://%HOST%/api/mood" -d "{\"mood\":\"focused\"}" >nul 2>&1
goto :done

:waiting
%CURL% -X POST "http://%HOST%/api/mood" -d "{\"mood\":\"auto\"}" >nul 2>&1
goto :done

:attention
%CURL% -X POST "http://%HOST%/api/express" -d "{\"expression\":\"wave\"}" >nul 2>&1
goto :done

:test
echo Contacting %HOST% ...
curl -s --max-time 5 "http://%HOST%/api/status"
echo.
if errorlevel 1 (
  echo.
  echo FAILED. Check that the device is powered, on the same network, and
  echo running firmware with mDNS. Try its IP instead of %HOST%.
) else (
  echo.
  echo OK - if you saw JSON above, the hooks will work.
)
goto :done

:done
rem Never propagate a failure: see note at the top.
exit /b 0
