@echo off
setlocal enabledelayedexpansion
REM =====================================================================
REM REPAIR-T6 — the all-up verify (CAMPAIGN_REPAIR_PLAN §T6): ONE command,
REM loud exit code, run before every push.
REM
REM   1. build Release (the suites + the campaign_qc harness)
REM   2. the fast sim tier — the unit suites, known-red gated
REM      (anything red OUTSIDE scripts\verify_known_reds.txt FAILS)
REM   3. the three scenario gates (takeoff_only, landing_only,
REM      digi_full_mission)
REM   4. the stock-landing harness (F4_STOCK_WORLD=testcamp.world.json)
REM   5. a 0.3-h armed war (--war --aa-combat: the C5 verdicts)
REM
REM Exit codes: 0 = green (known reds allowed, noted); 1 = the build or
REM an unknown red; 2 = a scenario gate / stock / war verdict failed.
REM Logs land in qc\verify\.
REM =====================================================================

set "ROOT=%~dp0.."
set "BUILD=%ROOT%\Build"
set "REDS=%~dp0verify_known_reds.txt"
set "OUT=%ROOT%\qc\verify"
set "WORLD=%ROOT%\testcamp.world.json"
set "CT=%ROOT%\f4-world-convert\tests\fixtures\falcon4.ct.json"
set "SCEN=%BUILD%\scenarios"

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%WORLD%" (
    echo [verify] FATAL: the stock world %WORLD% not found
    exit /b 2
)

set FAILED=0

REM ---------------------------------------------------------------------
echo [verify] 1/5 build Release...
cmake --build "%BUILD%" --config Release -j 8 ^
  --target test_brain_component test_navigation_module test_takeoff_module ^
  test_landing_module test_air_steering test_ground_steering ^
  test_tower_atc test_ground_contact test_combat_integration ^
  test_fidelity_combat test_campaign_session test_campaign_result_sink ^
  test_digi_mission test_campaign_stock_landing campaign_qc
if errorlevel 1 (
    echo [verify] BUILD FAILED
    exit /b 1
)

REM ---------------------------------------------------------------------
echo [verify] 2/5 the fast sim tier...
call :run_gated "%BUILD%\f4-ai\tests\Release\test_brain_component.exe"
call :run_gated "%BUILD%\f4-ai\tests\Release\test_navigation_module.exe"
call :run_gated "%BUILD%\f4-ai\tests\Release\test_takeoff_module.exe"
call :run_gated "%BUILD%\f4-ai\tests\Release\test_landing_module.exe"
call :run_gated "%BUILD%\f4-ai\tests\Release\test_air_steering.exe"
call :run_gated "%BUILD%\f4-ai\tests\Release\test_ground_steering.exe"
call :run_gated "%BUILD%\f4-ai\tests\Release\test_tower_atc.exe"
call :run_gated "%BUILD%\f4-simulation\tests\Release\test_ground_contact.exe"
call :run_gated "%BUILD%\f4-simulation\tests\Release\test_combat_integration.exe"
call :run_gated "%BUILD%\f4-simulation\tests\Release\test_fidelity_combat.exe"
call :run_gated "%BUILD%\f4-simulation\tests\Release\test_campaign_session.exe"
call :run_gated "%BUILD%\f4-simulation\tests\Release\test_campaign_result_sink.exe"
call :run_gated "%BUILD%\f4-simulation\tests\Release\test_digi_mission.exe"
call :run_gated "%BUILD%\f4-simulation\tests\Release\test_campaign_war_harness.exe"
if %FAILED%==1 (
    echo [verify] VERIFY FAILED — the fast sim tier has unknown reds
    exit /b 1
)

REM ---------------------------------------------------------------------
echo [verify] 3/5 the scenario gates...
for %%S in (takeoff_only landing_only digi_full_mission) do (
    echo [verify]   scenario %%S
    "%BUILD%\f4-simulation\Release\campaign_qc.exe" --scenario ^
        "%SCEN%\%%S.json" --no-record --out-dir "%OUT%\%%S" ^
        > "%OUT%\%%S.log" 2>&1
    if errorlevel 1 (
        echo [verify]   SCENARIO %%S FAILED — see %OUT%\%%S.log
        set FAILED=1
    )
)
if %FAILED%==1 exit /b 2

REM ---------------------------------------------------------------------
echo [verify] 4/5 the stock-landing harness...
set "F4_STOCK_WORLD=%WORLD%"
"%BUILD%\f4-simulation\tests\Release\test_campaign_stock_landing.exe" ^
    > "%OUT%\stock_landing.log" 2>&1
if errorlevel 1 (
    echo [verify] STOCK LANDING FAILED — see %OUT%\stock_landing.log
    exit /b 2
)
echo [verify]   stock landing OK

REM ---------------------------------------------------------------------
echo [verify] 5/5 the 0.3-h armed war...
"%BUILD%\f4-simulation\Release\campaign_qc.exe" "%WORLD%" ^
    --class-table "%CT%" --war 0.3 --war-sample 60 --tasking-cycle 60 ^
    --aa-combat --out-dir "%OUT%\war" > "%OUT%\war.log" 2>&1
if errorlevel 1 (
    echo [verify] THE ARMED WAR FAILED — see %OUT%\war.log
    exit /b 2
)
findstr /C:"deterministic=yes" "%OUT%\war.log" >nul
if errorlevel 1 (
    echo [verify] THE ARMED WAR DID NOT CERTIFY — see %OUT%\war.log
    exit /b 2
)
echo [verify]   armed war OK (deterministic)

echo.
if "%CI_NOTE%"=="" echo [verify] VERIFY GREEN
exit /b 0

REM ---------------------------------------------------------------------
REM :run_gated <exe> — run one gtest suite from ITS OWN directory (the
REM fixtures' relative paths resolve there) and gate its failures against
REM the known-red list. A green suite prints OK; a suite whose failures
REM are ALL in verify_known_reds.txt prints the known-red notes; any
REM unknown red sets FAILED (the verify's loud failure).
:run_gated
set "SUITE_EXE=%~1"
for %%N in ("%SUITE_EXE%") do set "SUITE=%%~nN"
set "LOG=%OUT%\%SUITE%.log"
pushd "%~dp1"
"%SUITE_EXE%" > "%LOG%" 2>&1
popd
if %errorlevel%==0 (
    echo [verify]   %SUITE% OK
    goto :eof
)
set UNKNOWN=0
for /f "tokens=4" %%F in ('findstr /B /C:"[  FAILED  ]" "%LOG%"') do (
    rem skip the "[  FAILED  ] N tests, listed below:" count header
    set ISNUM=0
    echo %%F| findstr /R /C:"^[0-9][0-9]*$" >nul && set ISNUM=1
    if !ISNUM!==0 (
        findstr /X /C:"%%F" "%REDS%" >nul 2>&1
        if errorlevel 1 (
            echo [verify]   %SUITE% UNKNOWN RED: %%F
            set UNKNOWN=1
        ) else (
            echo [verify]   %SUITE% known red: %%F
        )
    )
)
if !UNKNOWN!==1 (
    echo [verify]   %SUITE% FAILED — see !LOG!
    set FAILED=1
)
goto :eof
