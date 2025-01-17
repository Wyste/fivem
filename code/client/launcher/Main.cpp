/*
 * This file is part of the CitizenFX project - http://citizen.re/
 *
 * See LICENSE and MENTIONS in the root of the source tree for information
 * regarding licensing.
 */

#include "StdInc.h"
#include "CitizenGame.h"

#include <io.h>
#include <fcntl.h>

#include <CfxState.h>
#include <HostSharedData.h>

#include <array>

#include <shellscalingapi.h>

#include <shobjidl.h>

extern "C" BOOL WINAPI _CRT_INIT(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved);

void InitializeDummies();
std::optional<int> EnsureGamePath();

bool InitializeExceptionHandler();

std::map<std::string, std::string> UpdateGameCache();

#pragma comment(lib, "version.lib")

std::map<std::string, std::string> g_redirectionData;

extern "C" int wmainCRTStartup();

void DoPreLaunchTasks();
void NVSP_DisableOnStartup();
bool ExecutablePreload_Init();
void InitLogging();

HANDLE g_uiDoneEvent;
HANDLE g_uiExitEvent;

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow)
{
	bool toolMode = false;

	if (getenv("CitizenFX_ToolMode"))
	{
		toolMode = true;
	}

	if (!toolMode)
	{
		// bootstrap the game
		if (Bootstrap_RunInit())
		{
			return 0;
		}
	}

#if 0
	// TEST
	auto tenner = UI_InitTen();

	UI_DoCreation();

	while (!UI_IsCanceled())
	{
		HANDLE h = GetCurrentThread();
		MsgWaitForMultipleObjects(1, &h, FALSE, 50, QS_ALLEVENTS);
		
		MSG msg;
		while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}

		UI_UpdateText(0, va(L"%d", GetTickCount()));
		UI_UpdateText(1, va(L"%x", GetTickCount()));

		UI_UpdateProgress(50.0);
	}

	UI_DoDestruction();

	tenner = {};

	ExitProcess(0);
#endif

	// delete any old .exe.new file
	_unlink("CitizenFX.exe.new");

	// path environment appending of our primary directories
	static wchar_t pathBuf[32768];
	GetEnvironmentVariable(L"PATH", pathBuf, std::size(pathBuf));

	std::wstring newPath = MakeRelativeCitPath(L"bin") + L";" + MakeRelativeCitPath(L"") + L";" + std::wstring(pathBuf);

	SetEnvironmentVariable(L"PATH", newPath.c_str());

	SetDllDirectory(MakeRelativeCitPath(L"bin").c_str()); // to prevent a) current directory DLL search being disabled and b) xlive.dll being taken from system if not overridden

	if (!toolMode)
	{
		SetCurrentDirectory(MakeRelativeCitPath(L"").c_str());
	}

	auto addDllDirectory = (decltype(&AddDllDirectory))GetProcAddress(GetModuleHandle(L"kernel32.dll"), "AddDllDirectory");
	auto setDefaultDllDirectories = (decltype(&SetDefaultDllDirectories))GetProcAddress(GetModuleHandle(L"kernel32.dll"), "SetDefaultDllDirectories");

	// don't set DLL directories for ros:legit under Windows 7
	// see https://connect.microsoft.com/VisualStudio/feedback/details/2281687/setdefaultdlldirectories-results-in-exception-during-opening-a-winform-on-win7
	if (!IsWindows8OrGreater() && toolMode && wcsstr(GetCommandLineW(), L"ros:legit"))
	{
		setDefaultDllDirectories = nullptr;
	}

	if (addDllDirectory && setDefaultDllDirectories)
	{
		setDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
		addDllDirectory(MakeRelativeCitPath(L"").c_str());
		addDllDirectory(MakeRelativeCitPath(L"bin").c_str());
	}

	// determine dev mode and do updating
	wchar_t exeName[512];
	GetModuleFileName(GetModuleHandle(NULL), exeName, sizeof(exeName) / 2);

	wchar_t* exeBaseName = wcsrchr(exeName, L'\\');
	exeBaseName[0] = L'\0';
	exeBaseName++;

	bool devMode = toolMode;

	if (GetFileAttributes(va(L"%s.formaldev", exeBaseName)) != INVALID_FILE_ATTRIBUTES)
	{
		devMode = true;
	}

	// don't allow running a subprocess executable directly
	if (MakeRelativeCitPath(L"").find(L"cache\\subprocess") != std::string::npos)
	{
		return 0;
	}

	// store the last run directory for assistance purposes
	{
		auto regPath = MakeRelativeCitPath(L"");

		RegSetKeyValueW(HKEY_CURRENT_USER, L"SOFTWARE\\CitizenFX\\FiveM", L"Last Run Location", REG_SZ, regPath.c_str(), (regPath.size() + 1) * 2);
	}

	SetCurrentProcessExplicitAppUserModelID(L"CitizenFX.FiveM.Client");

	static HostSharedData<CfxState> initState("CfxInitState");

#ifdef IS_LAUNCHER
	initState->isReverseGame = true;
#endif

	// check if the master process still lives
	{
		HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, initState->GetInitialPid());

		if (hProcess == nullptr)
		{
			initState->SetInitialPid(GetCurrentProcessId());
		}
		else
		{
			DWORD exitCode = STILL_ACTIVE;
			GetExitCodeProcess(hProcess, &exitCode);

			if (exitCode != STILL_ACTIVE)
			{
				initState->SetInitialPid(GetCurrentProcessId());
			}

			CloseHandle(hProcess);
		}
	}

	// if not the master process, force devmode
	if (!devMode)
	{
		devMode = !initState->IsMasterProcess();
	}

	// init tenUI
	std::unique_ptr<TenUIBase> tui;

	if (initState->IsMasterProcess())
	{
		tui = UI_InitTen();
	}

	if (!devMode)
	{
		if (!Bootstrap_DoBootstrap())
		{
			return 0;
		}
	}

	if (InitializeExceptionHandler())
	{
		return 0;
	}

	InitLogging();

	// load global dinput8.dll over any that might exist in the game directory
	{
		wchar_t systemPath[512];
		GetSystemDirectory(systemPath, _countof(systemPath));

		wcscat_s(systemPath, L"\\dinput8.dll");

		LoadLibrary(systemPath);
	}

	LoadLibrary(MakeRelativeCitPath(L"dinput8.dll").c_str());
	LoadLibrary(MakeRelativeCitPath(L"steam_api64.dll").c_str());

	// laod V8 DLLs in case end users have these in a 'weird' directory
	LoadLibrary(MakeRelativeCitPath(L"v8_libplatform.dll").c_str());
	LoadLibrary(MakeRelativeCitPath(L"v8_libbase.dll").c_str());
	LoadLibrary(MakeRelativeCitPath(L"v8.dll").c_str());

	// assign us to a job object
	if (initState->IsMasterProcess())
	{
		// set DPI-aware
		HMODULE shCore = LoadLibrary(L"shcore.dll");

		if (shCore)
		{
			auto SetProcessDpiAwareness = (decltype(&::SetProcessDpiAwareness))GetProcAddress(shCore, "SetProcessDpiAwareness");

			if (SetProcessDpiAwareness)
			{
				SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
			}
		}

		// delete crashometry
		_wunlink(MakeRelativeCitPath(L"cache\\crashometry").c_str());

		if (GetFileAttributesW(MakeRelativeCitPath(L"permalauncher").c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			// create job
			HANDLE hJob = CreateJobObject(nullptr, nullptr);

			if (hJob)
			{
				if (AssignProcessToJobObject(hJob, GetCurrentProcess()))
				{
					JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
					info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
					if (SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &info, sizeof(info)))
					{
						initState->inJobObject = true;
					}
				}
			}
		}
	}

	// exit if not in a job object
	if (initState->inJobObject)
	{
		BOOL result;
		IsProcessInJob(GetCurrentProcess(), nullptr, &result);

		if (!result)
		{
			// and if this isn't a subprocess
			wchar_t fxApplicationName[MAX_PATH];
			GetModuleFileName(GetModuleHandle(nullptr), fxApplicationName, _countof(fxApplicationName));

			if (wcsstr(fxApplicationName, L"subprocess") == nullptr)
			{
				// and not a fivem:// protocol handler
				if (wcsstr(GetCommandLineW(), L"fivem://") == nullptr)
				{
					return 0;
				}
			}
		}
	}

	if (initState->IsMasterProcess())
	{
		DoPreLaunchTasks();
	}

	// make sure the game path exists
	if (auto gamePathExit = EnsureGamePath(); gamePathExit)
	{
		return *gamePathExit;
	}

	if (addDllDirectory)
	{
		addDllDirectory(MakeRelativeGamePath(L"").c_str());
	}

	if (!toolMode)
	{
		if (OpenMutex(SYNCHRONIZE, FALSE, L"CitizenFX_LogMutex") == nullptr)
		{
			// create the mutex
			CreateMutex(nullptr, TRUE, L"CitizenFX_LogMutex");

			// rotate any CitizenFX.log files cleanly
			const int MaxLogs = 10;

			auto makeLogName = [] (int idx)
			{
				return MakeRelativeCitPath(va(L"CitizenFX.log%s", (idx == 0) ? L"" : va(L".%d", idx)));
			};

			for (int i = (MaxLogs - 1); i >= 0; i--)
			{
				std::wstring logPath = makeLogName(i);
				std::wstring newLogPath = makeLogName(i + 1);

				if ((i + 1) == MaxLogs)
				{
					_wunlink(logPath.c_str());
				}
				else
				{
					_wrename(logPath.c_str(), newLogPath.c_str());
				}
			}

			// also do checks here to complain at BAD USERS
			if (!GetProcAddress(GetModuleHandle(L"kernel32.dll"), "SetThreadDescription")) // kernel32 forwarder only got this export in 1703, kernelbase.dll got this in 1607.
			{
				std::wstring fpath = MakeRelativeCitPath(L"CitizenFX.ini");

				bool showOSWarning = true;

				if (GetFileAttributes(fpath.c_str()) != INVALID_FILE_ATTRIBUTES)
				{
					showOSWarning = (GetPrivateProfileInt(L"Game", L"DisableOSVersionCheck", 0, fpath.c_str()) != 1);
				}

				if (showOSWarning)
				{
					MessageBox(nullptr, L"You are currently using an outdated version of Windows. This may lead to issues using the FiveM client. Please update to Windows 10 version 1703 (\"Creators Update\") or higher in case you are experiencing "
						L"any issues. The game will continue to start now.", L"FiveM", MB_OK | MB_ICONWARNING);
				}
			}

#ifndef _DEBUG
			{
				HANDLE hToken;

				if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
				{
					TOKEN_ELEVATION_TYPE elevationData;
					DWORD size;

					if (GetTokenInformation(hToken, TokenElevationType, &elevationData, sizeof(elevationData), &size))
					{
						if (elevationData == TokenElevationTypeFull)
						{
							const wchar_t* elevationComplaint = L"FiveM does not support running under elevated privileges. Please change your Windows settings to not run FiveM as administrator.\nThat won't fix anything. The game will exit now.";

							auto result = MessageBox(nullptr, elevationComplaint, L"FiveM", MB_ABORTRETRYIGNORE | MB_ICONERROR);

							if (result == IDIGNORE)
							{
								MessageBox(nullptr, L"No, you can't ignore this. The game will exit now.", L"FiveM", MB_OK | MB_ICONINFORMATION);
							}
							else if (result == IDRETRY)
							{
								MessageBox(nullptr, elevationComplaint, L"FiveM", MB_OK | MB_ICONWARNING);
							}

							return 0;
						}
					}

					CloseHandle(hToken);
				}
			}
#endif

			{
				HANDLE hFile = CreateFile(MakeRelativeCitPath(L"writable_test").c_str(), GENERIC_WRITE, FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);

				if (hFile == INVALID_HANDLE_VALUE)
				{
					if (GetLastError() == ERROR_ACCESS_DENIED)
					{
						MessageBox(nullptr, L"FiveM could not create a file in the folder it is placed in. Please move your installation out of Program Files or another protected folder.", L"Error", MB_OK | MB_ICONSTOP);
						return 0;
					}
				}
				else
				{
					CloseHandle(hFile);
				}
			}
		}
	}

	NVSP_DisableOnStartup();

	// readd the game path into the PATH
	newPath = MakeRelativeCitPath(L"bin\\crt") + L";" + MakeRelativeCitPath(L"bin") + L";" + MakeRelativeCitPath(L"") + L";" + MakeRelativeGamePath(L"") + L"; " + std::wstring(pathBuf);

	SetEnvironmentVariable(L"PATH", newPath.c_str());

	if (!toolMode)
	{
		SetCurrentDirectory(MakeRelativeGamePath(L"").c_str());
	}

#ifdef GTA_NY
	// initialize TLS variable so we get a TLS directory
	InitializeDummies();
#endif

	// check stuff regarding the game executable
	std::wstring gameExecutable = MakeRelativeGamePath(GAME_EXECUTABLE);

#ifndef IS_LAUNCHER
	if (GetFileAttributes(gameExecutable.c_str()) == INVALID_FILE_ATTRIBUTES)
	{
		MessageBox(nullptr, L"Could not find the game executable (" GAME_EXECUTABLE L") at the configured path. Please check your CitizenFX.ini file.", PRODUCT_NAME, MB_OK | MB_ICONERROR);
		return 0;
	}
#endif

#ifdef GTA_FIVE
	if (!ExecutablePreload_Init())
	{
		return 0;
	}

	// ensure game cache is up-to-date, and obtain redirection metadata from the game cache
	std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>, wchar_t> converter;
	auto redirectionData = UpdateGameCache();

	if (redirectionData.empty())
	{
		return 0;
	}

	g_redirectionData = redirectionData;

	gameExecutable = converter.from_bytes(redirectionData["GTA5.exe"]);

	{
		DWORD versionInfoSize = GetFileVersionInfoSize(gameExecutable.c_str(), nullptr);

		if (versionInfoSize)
		{
			std::vector<uint8_t> versionInfo(versionInfoSize);

			if (GetFileVersionInfo(gameExecutable.c_str(), 0, versionInfo.size(), &versionInfo[0]))
			{
				void* fixedInfoBuffer;
				UINT fixedInfoSize;

				VerQueryValue(&versionInfo[0], L"\\", &fixedInfoBuffer, &fixedInfoSize);

				VS_FIXEDFILEINFO* fixedInfo = reinterpret_cast<VS_FIXEDFILEINFO*>(fixedInfoBuffer);
				
				if ((fixedInfo->dwFileVersionLS >> 16) != 1604)
				{
					MessageBox(nullptr, va(L"The found GTA executable (%s) has version %d.%d.%d.%d, but only 1.0.1604.0 is currently supported. Please obtain this version, and try again.",
										   gameExecutable.c_str(),
										   (fixedInfo->dwFileVersionMS >> 16),
										   (fixedInfo->dwFileVersionMS & 0xFFFF),
										   (fixedInfo->dwFileVersionLS >> 16),
										   (fixedInfo->dwFileVersionLS & 0xFFFF)), PRODUCT_NAME, MB_OK | MB_ICONERROR);

					return 0;
				}
			}
		}
	}
#endif

	tui = {};

	g_uiExitEvent = CreateEvent(NULL, FALSE, FALSE, L"CitizenFX_PreUIExit");
	g_uiDoneEvent = CreateEvent(NULL, FALSE, FALSE, L"CitizenFX_PreUIDone");

	if (initState->IsMasterProcess() && !toolMode)
	{
		std::thread([/*tui = std::move(tui)*/]() mutable
		{
			static HostSharedData<CfxState> initState("CfxInitState");

			if (!initState->isReverseGame)
			{
				//auto tuiTen = std::move(tui);
				auto tuiTen = UI_InitTen();

				// say hi
				UI_DoCreation(false);

				auto st = GetTickCount64();
				UI_UpdateText(0, L"Starting FiveM...");
				UI_UpdateText(1, L"We're getting there.");

				while (GetTickCount64() < (st + 3500))
				{
					HANDLE hs[] =
					{
						g_uiExitEvent
					};

					auto res = MsgWaitForMultipleObjects(std::size(hs), hs, FALSE, 50, QS_ALLEVENTS);

					if (res == WAIT_OBJECT_0)
					{
						break;
					}

					MSG msg;
					while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
					{
						TranslateMessage(&msg);
						DispatchMessage(&msg);
					}

					UI_UpdateProgress((GetTickCount64() - st) / 35.0);
				}

				UI_DoDestruction();
			}

			SetEvent(g_uiDoneEvent);
		}).detach();
	}

	if (!toolMode)
	{
		wchar_t fxApplicationName[MAX_PATH];
		GetModuleFileName(GetModuleHandle(nullptr), fxApplicationName, _countof(fxApplicationName));

#ifdef IS_LAUNCHER
		// is this the game runtime subprocess?
		if (wcsstr(fxApplicationName, L"GameRuntime") != nullptr)
		{
#else
		if (initState->IsMasterProcess())
		{
#endif
#ifdef _DEBUG
			//MessageBox(nullptr, va(L"Gameruntime starting (pid %d)", GetCurrentProcessId()), L"CitizenFx", MB_OK);
#endif

			// game launcher initialization
			CitizenGame::Launch(gameExecutable);
		}
#ifdef IS_LAUNCHER
		// it's not, is this the first process running?
		else if (initState->IsMasterProcess())
		{
			// run game mode
			HMODULE coreRT = LoadLibrary(MakeRelativeCitPath(L"CoreRT.dll").c_str());

			if (coreRT)
			{
				auto gameProc = (void(*)())GetProcAddress(coreRT, "GameMode_Init");

				if (gameProc)
				{
					gameProc();
				}
			}
		}
#endif
		else
		{
			// could be it's a prelauncher like Chrome
			CitizenGame::Launch(gameExecutable, true);
		}
	}
	else
	{
		HMODULE coreRT = LoadLibrary(MakeRelativeCitPath(L"CoreRT.dll").c_str());

		if (coreRT)
		{
			auto toolProc = (void(*)())GetProcAddress(coreRT, "ToolMode_Init");

			if (toolProc)
			{
				auto gameFunctionProc = (void(*)(void(*)(const wchar_t*)))GetProcAddress(coreRT, "ToolMode_SetGameFunction");

				if (gameFunctionProc)
				{
					static auto gameExecutableStr = gameExecutable;

					gameFunctionProc([] (const wchar_t* customExecutable)
					{
						if (customExecutable == nullptr)
						{
							SetCurrentDirectory(MakeRelativeGamePath(L"").c_str());

							if (OpenMutex(SYNCHRONIZE, FALSE, L"CitizenFX_GameMutex") == nullptr)
							{
								CreateMutex(nullptr, TRUE, L"CitizenFX_GameMutex");

								CitizenGame::Launch(gameExecutableStr);
							}
						}
						else
						{
							CitizenGame::Launch(customExecutable);
						}
					});
				}

                CitizenGame::SetCoreMapping();

				toolProc();
			}
			else
			{
				printf("Could not find ToolMode_Init in CoreRT.dll.\n");
			}
		}
		else
		{
			printf("Could not initialize CoreRT.dll.\n");
		}
	}

	return 0;
}

extern "C" __declspec(dllexport) DWORD NvOptimusEnablement = 1;
extern "C" __declspec(dllexport) DWORD AmdPowerXpressRequestHighPerformance = 1;
