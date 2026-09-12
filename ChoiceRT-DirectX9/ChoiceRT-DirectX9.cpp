//set these for different viewport resolution, for example
//#define HEIGHT 480
//#define WIDTH  640

#define HEIGHT GetSystemMetrics(SM_CYSCREEN)
#define WIDTH  GetSystemMetrics(SM_CXSCREEN)

// no need to change anything below this point

#define NOMINMAX
#include <d3d9.h>
#include <mmsystem.h> //timeBeginPeriod/timeEndPeriod
#include <atomic>
#include <thread>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cwchar> // swprintf_s
#include <conio.h> // _getch()
#include <ctime>
#include <limits>
#include <iostream>
#include <iomanip> // std::setprecision

//if linker can't find d3d9.lib, point it at your SDK/Windows Kit's lib directory here instead
#pragma comment (lib, "winmm.lib")

static const D3DCOLOR COLOR_BLACK = D3DCOLOR_XRGB(0, 0, 0);

constexpr int MAX_STIMULI = 8;

// name/wname hold the same text in both charsets: console output needs narrow, the on-screen
// GDI legend needs wide, and storing both avoids a runtime conversion for static data that never
// changes
struct StimulusDef {
	D3DCOLOR color;
	const char* name;
	const wchar_t* wname;
	WORD vkey;
	char keyChar;
};

// home-row keys, split across both hands: A S D F / J K L ;
static const StimulusDef ALL_STIMULI[MAX_STIMULI] = {
	{ D3DCOLOR_XRGB(255,   0,   0), "RED",     L"RED",     'A',      'A' },
	{ D3DCOLOR_XRGB(  0, 255,   0), "GREEN",   L"GREEN",   'S',      'S' },
	{ D3DCOLOR_XRGB(  0,   0, 255), "BLUE",    L"BLUE",    'D',      'D' },
	{ D3DCOLOR_XRGB(255, 255,   0), "YELLOW",  L"YELLOW",  'F',      'F' },
	{ D3DCOLOR_XRGB(  0, 255, 255), "CYAN",    L"CYAN",    'J',      'J' },
	{ D3DCOLOR_XRGB(255,   0, 255), "MAGENTA", L"MAGENTA", 'K',      'K' },
	{ D3DCOLOR_XRGB(255, 140,   0), "ORANGE",  L"ORANGE",  'L',      'L' },
	{ D3DCOLOR_XRGB(255, 255, 255), "WHITE",   L"WHITE",   VK_OEM_1, ';' },
};

int activeStimulusCount = 3; //set once from console input in WinMain, before any thread reads it

static D3DCOLOR ColorForStimulus(int idx) { return idx >= 0 ? ALL_STIMULI[idx].color : COLOR_BLACK; }
static const char* NameForStimulus(int idx) { return idx >= 0 ? ALL_STIMULI[idx].name : "NONE"; }

static void SplitRGB(D3DCOLOR c, int& r, int& g, int& b) {
	r = (c >> 16) & 0xFF;
	g = (c >> 8) & 0xFF;
	b = c & 0xFF;
}

static bool FindStimulusForKey(WORD vkey, int& outIndex) {
	for (int i = 0; i < activeStimulusCount; i++) {
		if (ALL_STIMULI[i].vkey == vkey) { outIndex = i; return true; }
	}
	return false;
}

// Cross-thread-shared atomics get their own cache line each: the input thread writes these
// while the render thread is reading (and sometimes writing) them at very high frequency in
// its own spin loop, and vice versa. Without padding, two of these could land on the same
// 64-byte line and every write from one core would force a coherency invalidation on the
// other core's cached copy ("false sharing") even though the variables are logically
// unrelated. Padding each one out to a full line means a write from one thread can never
// disturb the other thread's cache line for a different variable.
template <typename T>
struct alignas(64) HotAtomic {
	static_assert(sizeof(std::atomic<T>) < 64, "HotAtomic pad would underflow");
	std::atomic<T> value;
	char pad[64 - sizeof(std::atomic<T>)];
	HotAtomic() = default;
	explicit HotAtomic(T v) : value(v) {}
	T load(std::memory_order order = std::memory_order_relaxed) const { return value.load(order); }
	void store(T v, std::memory_order order = std::memory_order_relaxed) { value.store(v, order); }
};

HotAtomic<D3DCOLOR> currentColor{ COLOR_BLACK };
HotAtomic<int> currentStimulusIndex{ -1 };
HotAtomic<int> appPhase{ 0 };  // 0 = ready screen (legend, waiting for SPACE); 1 = running trials
HotAtomic<int> state{ 0 };     // 0 = black, counting down to next stimulus; 1 = stimulus shown, awaiting a key

// time_points aren't atomic, so the black->stimulus deadline and the stimulus-onset timestamp
// are shared between threads as raw tick counts of high_resolution_clock's own duration.
HotAtomic<long long> stimulusDeadlineTicks{ 0 }; // render thread: when to flip black -> stimulus
HotAtomic<long long> stimulusStartTicks{ 0 };    // input thread: reads this once state==1 to compute RT
__forceinline std::chrono::high_resolution_clock::time_point loadTimePoint(const HotAtomic<long long>& ticks) {
	return std::chrono::high_resolution_clock::time_point(std::chrono::high_resolution_clock::duration(ticks.load()));
}
__forceinline void storeTimePoint(HotAtomic<long long>& ticks, std::chrono::high_resolution_clock::time_point tp) {
	ticks.store(tp.time_since_epoch().count());
}

// arms a fresh random black-screen interval (2-6s) before the next stimulus appears
__forceinline void ArmNextStimulusDeadline(std::chrono::high_resolution_clock::time_point now) {
	int rnd = rand() % 4000 + 2000; //random delay, between 2 and 6 seconds
	storeTimePoint(stimulusDeadlineTicks, now + std::chrono::milliseconds(rnd));
}

struct Trial {
	int stimulus;
	int64_t rtMicros;
};

std::vector<Trial> correctTrials; //input-thread-only
int64_t wrongKeyErrors = 0;       //input-thread-only: pressed a choice key that didn't match the stimulus shown
int64_t earlyErrors = 0;          //input-thread-only: pressed a choice key before any stimulus appeared
int64_t trialCount = 0;           //input-thread-only: stimuli that received a response (correct or wrong key)

HotAtomic<bool> stop{ false };

// posted by the render thread, once it has released the D3D9 device, to ask the thread that
// owns the window to actually destroy it (Win32 requires DestroyWindow to run on that thread)
#define WM_APP_TEARDOWN (WM_APP + 1)

constexpr UINT RAW_BUF_CAPACITY = 800; //comfortably covers a single keyboard RAWINPUT record
alignas(RAWINPUT) static BYTE raw_buf_storage[RAW_BUF_CAPACITY];
RAWINPUT* raw_buf = reinterpret_cast<RAWINPUT*>(raw_buf_storage);

static void PrintStats() {
	std::cout << std::fixed << std::setprecision(2);

	size_t correct = correctTrials.size();

	if (correct > 0) {
		std::vector<int64_t> rts;
		rts.reserve(correct);
		for (const Trial& t : correctTrials) rts.push_back(t.rtMicros);
		std::sort(rts.begin(), rts.end());

		int64_t sum = 0;
		for (int64_t rt : rts) sum += rt;
		int64_t average = sum / (int64_t)rts.size();

		double variance = 0.0;
		for (int64_t rt : rts) variance += pow((double)(rt - average), 2);
		double stdev = rts.size() > 1 ? sqrt(variance / (rts.size() - 1)) : 0.0;

		std::cout << "\nOverall (correct responses only):\n";
		std::cout << "Max:   " << rts.back() / 1000.0 << "ms\n";
		std::cout << "Avg:   " << average / 1000.0 << "ms\n";
		std::cout << "Min:   " << rts.front() / 1000.0 << "ms\n";
		std::cout << "STDEV: " << stdev / 1000.0 << "\n";
	}
	else {
		std::cout << "\nNo correct responses recorded.\n";
	}

	std::cout << "\nPer-Stimulus Breakdown:\n";
	for (int idx = 0; idx < activeStimulusCount; idx++) {
		std::vector<int64_t> rts;
		for (const Trial& t : correctTrials) if (t.stimulus == idx) rts.push_back(t.rtMicros);

		std::cout << ALL_STIMULI[idx].name << " (" << ALL_STIMULI[idx].keyChar << "): ";
		if (!rts.empty()) {
			std::sort(rts.begin(), rts.end());
			int64_t sum = 0;
			for (int64_t rt : rts) sum += rt;
			double avg = (double)sum / rts.size() / 1000.0;
			std::cout << rts.size() << " correct, avg " << avg << "ms, min " << rts.front() / 1000.0
				<< "ms, max " << rts.back() / 1000.0 << "ms\n";
		}
		else {
			std::cout << "no correct responses\n";
		}
	}

	std::cout << "\nTotals:\n";
	std::cout << "Total Stimuli Shown:  " << trialCount << "\n";
	std::cout << "Correct Responses:    " << correct << "\n";
	std::cout << "Wrong-Key Responses:  " << wrongKeyErrors << "\n";
	std::cout << "Anticipation Errors:  " << earlyErrors << " (key pressed before a stimulus appeared)\n";
	if (trialCount > 0)
		std::cout << "Accuracy:             " << (100.0 * correct / trialCount) << "%\n";
}

LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_INPUT: {
		//buffer is pre-sized big enough for a keyboard RAWINPUT, so a single call fetches
		//the data directly instead of round-tripping once to ask for the required size first
		UINT cb_size = RAW_BUF_CAPACITY;
		GetRawInputData((HRAWINPUT)lParam, RID_INPUT, raw_buf, &cb_size, sizeof(RAWINPUTHEADER));

		if (raw_buf->header.dwType == RIM_TYPEKEYBOARD &&
			(raw_buf->data.keyboard.Message == WM_KEYDOWN || raw_buf->data.keyboard.Message == WM_SYSKEYDOWN)) {

			WORD vkey = raw_buf->data.keyboard.VKey;
			auto now = std::chrono::high_resolution_clock::now();

			if (vkey == VK_ESCAPE) {
				PrintStats();
				//don't tear the window down here: the render thread still owns the D3D9
				//device (fullscreen exclusive) and must release it first. Just flag stop;
				//the render thread will ask us to destroy the window once it's done.
				stop.store(true, std::memory_order_release);
			}
			else if (appPhase.load() == 0) {
				if (vkey == VK_SPACE) {
					ArmNextStimulusDeadline(now);
					state.store(0);
					appPhase.store(1);
				}
			}
			else {
				int pressedIndex;
				if (FindStimulusForKey(vkey, pressedIndex)) {
					int curState = state.load();

					if (curState == 0) { //pressed before any stimulus appeared
						earlyErrors++;
						ArmNextStimulusDeadline(now); //re-roll the wait so rhythmic guessing can't pay off
					}
					else if (curState == 1) {
						int stim = currentStimulusIndex.load();
						trialCount++;
						state.store(0);
						currentColor.store(COLOR_BLACK);
						currentStimulusIndex.store(-1);
						ArmNextStimulusDeadline(now);

						if (pressedIndex == stim) {
							auto timeDiff = now - loadTimePoint(stimulusStartTicks);
							int64_t rtMicros = std::chrono::duration_cast<std::chrono::microseconds>(timeDiff).count();
							correctTrials.push_back({ stim, rtMicros });
							std::cout << "#" << trialCount << " " << NameForStimulus(stim) << ": "
								<< rtMicros / 1000.0 << "ms\n";
						}
						else {
							wrongKeyErrors++;
							std::cout << "#" << trialCount << " " << NameForStimulus(stim) << ": WRONG (pressed "
								<< ALL_STIMULI[pressedIndex].keyChar << ")\n";
						}
					}
				}
			}
		}

		if (GET_RAWINPUT_CODE_WPARAM(wParam) == RIM_INPUT)
			DefWindowProc(hWnd, msg, wParam, lParam); //The application must call DefWindowProc so the system can perform cleanup.
		break;
	}
	case WM_CLOSE:
		//same reasoning as the ESC handler above: let the render thread drive teardown order
		stop.store(true, std::memory_order_release);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	case WM_APP_TEARDOWN:
		DestroyWindow(hWnd);
		return 0;
	default:
		return DefWindowProc(hWnd, msg, wParam, lParam);
	}
	return 0;
}

// Console subsystem entry point, so results printed via std::cout are visible; forwards to WinMain.
int main()
{
	int ret = WinMain(GetModuleHandle(NULL), NULL, NULL, SW_SHOWNORMAL);
	return ret;
}

// Windows 10's power throttling ("EcoQoS") can transparently downclock a thread or, on hybrid
// P-core/E-core CPUs, schedule it onto a slow efficiency core if it looks like background work.
// That would silently wreck latency on exactly the threads we've gone to the trouble of pinning
// and boosting, so explicitly opt them (and the process) out of it.
static void OptOutOfPowerThrottling(HANDLE thread) {
	THREAD_POWER_THROTTLING_STATE state{};
	state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
	state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
	state.StateMask = 0; //0 = never throttle this thread
	SetThreadInformation(thread, ThreadPowerThrottling, &state, sizeof(state));
}

// draws the color/key legend onto the current back buffer via GetDC, so it doesn't need a D3DX
// font or any vertex/text pipeline of its own - just GDI, released before Present() touches the
// surface again
static void DrawReadyOverlay(LPDIRECT3DDEVICE9 d3ddev) {
	LPDIRECT3DSURFACE9 backBuffer = nullptr;
	if (FAILED(d3ddev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer) return;

	HDC hdc;
	if (SUCCEEDED(backBuffer->GetDC(&hdc))) {
		HFONT font = CreateFontW(-MulDiv(28, GetDeviceCaps(hdc, LOGPIXELSY), 72), 0, 0, 0,
			FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
			CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
		HFONT oldFont = (HFONT)SelectObject(hdc, font);
		SetBkMode(hdc, TRANSPARENT);

		int lineHeight = 48;
		int totalHeight = (activeStimulusCount + 2) * lineHeight;
		int top = ((int)HEIGHT - totalHeight) / 2;

		SetTextColor(hdc, RGB(255, 255, 255));
		RECT titleRect{ 0, top, (int)WIDTH, top + lineHeight };
		DrawTextW(hdc, L"ChoiceRT-DirectX9", -1, &titleRect, DT_CENTER | DT_SINGLELINE);

		for (int i = 0; i < activeStimulusCount; i++) {
			int r, g, b;
			SplitRGB(ALL_STIMULI[i].color, r, g, b);
			SetTextColor(hdc, RGB(r, g, b));

			wchar_t line[64];
			swprintf_s(line, L"%s  =  %c", ALL_STIMULI[i].wname, (wchar_t)ALL_STIMULI[i].keyChar);

			RECT lineRect{ 0, top + (i + 1) * lineHeight, (int)WIDTH, top + (i + 2) * lineHeight };
			DrawTextW(hdc, line, -1, &lineRect, DT_CENTER | DT_SINGLELINE);
		}

		SetTextColor(hdc, RGB(160, 160, 160));
		RECT footer{ 0, top + (activeStimulusCount + 1) * lineHeight, (int)WIDTH, top + (activeStimulusCount + 2) * lineHeight };
		DrawTextW(hdc, L"Press SPACE to begin, ESC to quit", -1, &footer, DT_CENTER | DT_SINGLELINE);

		SelectObject(hdc, oldFont);
		DeleteObject(font);
		backBuffer->ReleaseDC(hdc);
	}
	backBuffer->Release();
}

static void HandlePresent(LPDIRECT3DDEVICE9 d3ddev, D3DPRESENT_PARAMETERS& d3dpp) {
	HRESULT hr = d3ddev->Present(NULL, NULL, NULL, NULL);

	// Exclusive fullscreen devices get lost whenever something steals focus - alt-tab, a
	// UAC prompt, a Windows notification toast, a screen lock, an RDP disconnect. Without
	// handling this, Present() would just fail silently forever and the screen would go
	// dead/frozen until the process is killed - one of the most common real stutter/freeze
	// causes for exclusive-fullscreen D3D9 apps.
	if (hr == D3DERR_DEVICELOST) {
		while (!stop.load(std::memory_order_acquire)) {
			HRESULT cooperativeLevel = d3ddev->TestCooperativeLevel();
			if (cooperativeLevel == D3D_OK) break;
			if (cooperativeLevel == D3DERR_DEVICENOTRESET) {
				if (SUCCEEDED(d3ddev->Reset(&d3dpp))) break;
			}
			Sleep(1); //nothing useful to render while the device isn't ours to draw to; a
			          //short sleep here doesn't touch the steady-state input/render latency
		}
	}
}

// runs on the calling (main) thread until stop is signalled, then releases the D3D9 device and
// asks the input thread (the one that actually owns hWnd) to destroy the window.
void RunRenderLoop(LPDIRECT3D9 d3d, LPDIRECT3DDEVICE9 d3ddev, HWND hWnd, D3DPRESENT_PARAMETERS d3dpp) {
	while (!stop.load(std::memory_order_acquire)) {
		if (appPhase.load() == 0) {
			d3ddev->Clear(0, NULL, D3DCLEAR_TARGET, COLOR_BLACK, 0.0f, 0);
			DrawReadyOverlay(d3ddev);
			HandlePresent(d3ddev, d3dpp);
		}
		else {
			if (state.load() == 0) {
				auto now = std::chrono::high_resolution_clock::now();
				if (now >= loadTimePoint(stimulusDeadlineTicks)) {
					int idx = rand() % activeStimulusCount;
					currentStimulusIndex.store(idx);
					currentColor.store(ColorForStimulus(idx));
					storeTimePoint(stimulusStartTicks, now); //keep actual stimulus-onset timer
					state.store(1);
				}
			}

			d3ddev->Clear(0, NULL, D3DCLEAR_TARGET, currentColor.load(), 0.0f, 0);
			HandlePresent(d3ddev, d3dpp);
		}
	}

	d3ddev->Release();
	d3d->Release();
	PostMessage(hWnd, WM_APP_TEARDOWN, 0, 0);
}

struct InputThreadContext {
	HINSTANCE hInstance;
	HWND hWnd = nullptr;
};

// owns the window and its message queue for the lifetime of the app: Win32 ties a window's
// message queue to the thread that created it, so this is the only thread that can ever see
// WM_INPUT. It does nothing else - no D3D calls, no waiting on Present - and never sleeps: it
// busy-polls PeekMessage instead of blocking on GetMessage, trading a fully-pinned CPU core for
// removing the OS sleep/wake scheduling transition (and its jitter) from the input path
// entirely. Combined with TIME_CRITICAL priority, power-throttling opt-out, and a dedicated
// physical core, this is effectively the ceiling of what user-mode Windows allows.
void InputThreadMain(InputThreadContext* ctx, std::atomic<bool>* windowReady, DWORD_PTR affinityMask) {
	if (affinityMask) SetThreadAffinityMask(GetCurrentThread(), affinityMask);
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
	OptOutOfPowerThrottling(GetCurrentThread());

	WNDCLASSEX wc;
	ZeroMemory(&wc, sizeof(WNDCLASSEX));

	wc.cbSize = sizeof(WNDCLASSEX);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = WindowProc;
	wc.hInstance = ctx->hInstance;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.lpszClassName = L"ChoiceRTWindowClass";

	RegisterClassEx(&wc);

	HWND hWnd = CreateWindowEx(0, L"ChoiceRTWindowClass", L"ChoiceRT-DirectX9",
		WS_EX_TOPMOST | WS_POPUP,
		0, 0, (UINT)GetSystemMetrics(SM_CXSCREEN), (UINT)GetSystemMetrics(SM_CYSCREEN),
		NULL, NULL, ctx->hInstance, NULL);

	if (!hWnd) {
		windowReady->store(true, std::memory_order_release);
		return;
	}

	RAWINPUTDEVICE Keyboard;
	Keyboard.usUsagePage = 0x01;
	Keyboard.usUsage = 0x06; //keyboard
	Keyboard.dwFlags = RIDEV_NOLEGACY;
	Keyboard.hwndTarget = hWnd;

	if (!RegisterRawInputDevices(&Keyboard, 1, sizeof(RAWINPUTDEVICE))) {
		DestroyWindow(hWnd);
		windowReady->store(true, std::memory_order_release);
		return;
	}

	ShowWindow(hWnd, SW_SHOWNORMAL);
	ShowCursor(FALSE);
	SetCursor(NULL);

	ctx->hWnd = hWnd;
	windowReady->store(true, std::memory_order_release);

	//busy-poll instead of GetMessage's blocking wait: PeekMessage doesn't have GetMessage's
	//"returns 0 on WM_QUIT" contract, so WM_QUIT has to be checked for explicitly.
	MSG msg;
	for (;;) {
		if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) break;
			DispatchMessage(&msg);
		}
		else {
			YieldProcessor(); //PAUSE hint: doesn't add meaningful latency (the next poll still sees a
			                   //queued message immediately) but is dramatically kinder to power/heat
			                   //and to a hyperthread sibling than a naked spin
		}
	}
}

// Enumerate physical cores and hand back the affinity masks of two DIFFERENT physical cores -
// one for input, one for render - so the two hot threads can never end up as hyperthread
// siblings of each other (which would have them fight over the same core's execution ports and
// caches even while "on separate logical processors"). Also prefers to skip whichever core owns
// logical processor 0, since Windows tends to route more interrupts/DPCs to it. Falls back to
// the simple "first two schedulable logical processors" approach if the topology query fails or
// the machine doesn't expose at least two physical cores to this process.
static bool PickCoreAffinities(DWORD_PTR processAffinityMask, DWORD_PTR& inputMask, DWORD_PTR& renderMask) {
	DWORD len = 0;
	GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
	if (len == 0) return false;

	std::vector<BYTE> buffer(len);
	auto* infoBase = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
	if (!GetLogicalProcessorInformationEx(RelationProcessorCore, infoBase, &len)) return false;

	std::vector<DWORD_PTR> coreMasks;
	BYTE* ptr = buffer.data();
	BYTE* end = buffer.data() + len;
	while (ptr < end) {
		auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(ptr);
		if (info->Relationship == RelationProcessorCore) {
			DWORD_PTR mask = (DWORD_PTR)info->Processor.GroupMask[0].Mask & processAffinityMask;
			if (mask) coreMasks.push_back(mask);
		}
		ptr += info->Size;
	}

	if (coreMasks.size() < 2) return false;

	//cores that don't include logical processor 0 sort first
	std::sort(coreMasks.begin(), coreMasks.end(), [](DWORD_PTR a, DWORD_PTR b) {
		return (a & 1) < (b & 1);
		});

	inputMask = coreMasks[0];
	renderMask = coreMasks[1];
	return true;
}

// conhost/Windows Terminal both render 24-bit ANSI color codes once VT processing is turned on,
// so the console legend can show each color's real RGB instead of just naming it in plain text
static void EnableAnsiColors() {
	HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
	DWORD mode = 0;
	if (hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &mode)) {
		SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
	}
}

static void PrintFullLegend() {
	std::cout << "Available colors and keys:\n";
	for (int i = 0; i < MAX_STIMULI; i++) {
		int r, g, b;
		SplitRGB(ALL_STIMULI[i].color, r, g, b);
		std::cout << "  \x1b[38;2;" << r << ";" << g << ";" << b << "m"
			<< ALL_STIMULI[i].name << " = " << ALL_STIMULI[i].keyChar << "\x1b[0m\n";
	}
	std::cout << "\n";
}

static void PromptForStimulusCount() {
	std::cout << "ChoiceRT-DirectX9\n";
	std::cout << "Press ESC at any time to stop and see your results.\n\n";

	PrintFullLegend();

	for (;;) {
		std::cout << "How many of the colors above to use, in order (2-" << MAX_STIMULI << ")? ";
		int n;
		std::cin >> n;
		bool failed = std::cin.fail();
		if (failed) std::cin.clear();
		//always discard the rest of the line, success or failure, so a leftover '\n' can't
		//satisfy a later blocking read (e.g. the "press any key" wait) instantly
		std::cin.ignore((std::numeric_limits<std::streamsize>::max)(), '\n');
		if (failed) continue;
		if (n >= 2 && n <= MAX_STIMULI) {
			activeStimulusCount = n;
			break;
		}
	}

	std::cout << "\nUsing the first " << activeStimulusCount << " colors above. "
		"This legend is also shown on screen - press SPACE there to begin.\n\n";
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	srand(static_cast<unsigned int>(time(NULL)));

	std::cout << std::fixed << std::setprecision(2);
	EnableAnsiColors();
	PromptForStimulusCount();

	timeBeginPeriod(1); //tighten the scheduler/timer tick for more consistent thread wake latency

	HANDLE process = GetCurrentProcess();
	DWORD_PTR processAffinityMask, systemAffinityMask;
	DWORD_PTR inputCoreMask = 0, renderCoreMask = 0;

	//isolate the input-pump thread and the render thread onto separate physical cores, so a busy
	//render loop can never delay this process's own handling of a WM_INPUT message
	if (GetProcessAffinityMask(process, &processAffinityMask, &systemAffinityMask)) {
		if (!PickCoreAffinities(processAffinityMask, inputCoreMask, renderCoreMask)) {
			//topology query unavailable/insufficient: fall back to the first two available logical processors
			inputCoreMask = 0; renderCoreMask = 0;
			DWORD_PTR mask = 1;
			for (int bit = 0; bit < (int)(sizeof(DWORD_PTR) * 8) && !renderCoreMask; bit++) {
				if (mask & processAffinityMask) {
					if (!inputCoreMask) inputCoreMask = mask;
					else if (!renderCoreMask) renderCoreMask = mask;
				}
				mask <<= 1;
			}
		}
	}

	//HIGH (not REALTIME) on purpose: REALTIME_PRIORITY_CLASS unlocks Windows' real-time priority
	//tier (16-31), which gets no anti-starvation boosting and can preempt system housekeeping
	//threads unconditionally - a hang in a thread up there can take the whole desktop with it.
	//HIGH_PRIORITY_CLASS stays in the scheduler-supervised range while still sitting above every
	//normal application.
	SetPriorityClass(process, HIGH_PRIORITY_CLASS);

	PROCESS_POWER_THROTTLING_STATE procPowerState{};
	procPowerState.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
	procPowerState.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
	procPowerState.StateMask = 0; //opt the whole process out of EcoQoS/efficiency-core scheduling
	SetProcessInformation(process, ProcessPowerThrottling, &procPowerState, sizeof(procPowerState));

	InputThreadContext inputCtx{ hInstance };
	std::atomic<bool> windowReady{ false };
	std::thread inputThread(InputThreadMain, &inputCtx, &windowReady, inputCoreMask);

	while (!windowReady.load(std::memory_order_acquire))
		std::this_thread::yield(); //one-shot wait for window/raw-input setup on the input thread

	if (!inputCtx.hWnd) {
		inputThread.join();
		timeEndPeriod(1);
		return -1;
	}
	HWND hWnd = inputCtx.hWnd;

	LPDIRECT3D9 d3d;
	LPDIRECT3DDEVICE9 d3ddev;
	D3DPRESENT_PARAMETERS d3dpp;

	d3d = Direct3DCreate9(D3D_SDK_VERSION);

	ZeroMemory(&d3dpp, sizeof(d3dpp));
	d3dpp.Windowed = FALSE;
	d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	d3dpp.hDeviceWindow = hWnd;
	d3dpp.BackBufferFormat = D3DFMT_X8R8G8B8;
	d3dpp.BackBufferWidth = (UINT)GetSystemMetrics(SM_CXSCREEN);
	d3dpp.BackBufferHeight = (UINT)GetSystemMetrics(SM_CYSCREEN);
	d3dpp.BackBufferCount = 1; //deliberately not raised: more buffers measured ~30-40% higher
	                           //throughput here, but each queued buffer is another frame of
	                           //possible delay between drawing something and it reaching the
	                           //screen - the opposite of what this app is for
	d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE; //disable vsync

	d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &d3dpp, &d3ddev);
	if (d3ddev == NULL) {
		stop.store(true, std::memory_order_release);
		PostMessage(hWnd, WM_APP_TEARDOWN, 0, 0);
		inputThread.join();
		d3d->Release();
		timeEndPeriod(1);
		return -1;
	}

	D3DVIEWPORT9 pViewport = { 0, 0, (DWORD)WIDTH, (DWORD)HEIGHT, 0.0, 1.0 };
	if (d3ddev->SetViewport(&pViewport) != S_OK) {
		stop.store(true, std::memory_order_release);
		d3ddev->Release();
		PostMessage(hWnd, WM_APP_TEARDOWN, 0, 0);
		inputThread.join();
		d3d->Release();
		timeEndPeriod(1);
		return -1;
	}

	d3ddev->ShowCursor(FALSE);

	//the very first Clear+Present pays a one-time cost (shader/driver JIT, fullscreen mode-set
	//settling) that measured ~17ms here versus a steady-state ~1ms - paying it now, before the
	//benchmark can ever be looking at the clock, instead of on the user's first real trial.
	for (int i = 0; i < 5; i++) {
		d3ddev->Clear(0, NULL, D3DCLEAR_TARGET, COLOR_BLACK, 0.0f, 0);
		d3ddev->Present(NULL, NULL, NULL, NULL);
	}

	if (renderCoreMask) SetThreadAffinityMask(GetCurrentThread(), renderCoreMask);
	//ABOVE_NORMAL, not HIGHEST: within HIGH_PRIORITY_CLASS, HIGHEST and TIME_CRITICAL both resolve
	//to the same absolute priority (15), which would tie with the input thread instead of staying
	//strictly below it. ABOVE_NORMAL keeps a real gap so input always wins any actual contention.
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
	OptOutOfPowerThrottling(GetCurrentThread());

	RunRenderLoop(d3d, d3ddev, hWnd, d3dpp); //blocks (on this thread) until stop is signalled; releases d3d/d3ddev internally

	inputThread.join();

	timeEndPeriod(1);
	ShowCursor(true);

	std::cout << "\nPress any key to continue...\n";
	//_getch() reads straight from the console, not the buffered C stream cin/getchar share, so
	//it can't be short-circuited by a leftover newline the way an earlier cin>>n bug did
	_getch();

	return 0;
}
