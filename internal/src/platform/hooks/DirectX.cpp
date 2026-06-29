#include "pch-il2cpp.h"
#include <cstdio>

#include "DirectX.h"
#include "settings.h"
#include "gui/tabs/TestTAB.h"
#include "gui/tabs/VisualsTAB.h"
#include "gui/tabs/CombatTab/CombatTAB.h"
#include "gui/tabs/PlayerTAB.h"
#include <imgui/imgui_impl_dx11.h>
#include <imgui/imgui_impl_win32.h>
#include <mutex>
#include <atomic>
#include <chrono>
#include <thread>
#include <Il2CppResolver.h>
#include "AutoAim.h"
#include "RuntimeOffsets.h"
#include "BootGate.h"
#include "DiagBridge.h"
#include "GameState.h"
#include "LocalPlayer.h"
#include "SharedMemory.h"
#include "SkinChanger.h"
#include "BagLooter.h"
#include "SpeedHack.h"
#include "FpsSetter.h"
#include "gui/tabs/CameraTAB.h"
#include "FeatureRuntime.h"
#include "ChatToast.h"
#include "HwidCapture.h"
#include "NoclipHook.h"
#include "keybinds.h"
#include "gui/tabs/WorldTAB.h"
#include "DbgFileLog.h"
#include "CrashTrace.h"
#include "AimHooks.h"

namespace {

static std::atomic<uint32_t> s_presentFrame{0};

// ImGui re-enable ladder (crash bisect). 0=off — dashboard users do not need the
// in-game overlay; stages 1+ have repeatedly crashed on first NewFrame after init.
// 0=off  1=bare D3D/imgui  2=FPS top-left (no IL2CPP)  3=FPS+camera refresh
// 4=+tab ticks  5=full menu
static constexpr uint8_t kImGuiStage = 0;
// NewFrame on f=1 hangs/crashes during menu load — defer until the game settles.
static constexpr uint32_t kImGuiMinFrame = 450;

static bool ImGuiRenderAllowed(uint32_t frame)
{
	if (kImGuiStage == 0) return false;
	if (frame < kImGuiMinFrame) return false;
	if (!GameState::GetLocalPtr()) return false;
	const int32_t maxHp = LocalPlayer::GetMaxHP();
	const int32_t hp = LocalPlayer::GetHP();
	// Reject garbage reads from stale offsets — a bogus maxHp>0 used to arm ImGui
	// mid-load and hang/crash on the first overlay frame.
	if (maxHp <= 0 || maxHp > 50000) return false;
	if (hp < 0 || hp > maxHp) return false;
	return true;
}

// Verbose enter/leave for early frames, every 500th, and post-boot heartbeats (25-frame cadence).
static bool PresentTraceVerbose(uint32_t frame)
{
	return frame <= 40u || (frame % 500u) == 0u;
}

static bool PresentTraceHeartbeat(uint32_t frame)
{
	return frame > 40u && (frame % 25u) == 0u;
}

static bool PresentShouldTrace(uint32_t frame)
{
	return PresentTraceVerbose(frame) || PresentTraceHeartbeat(frame);
}

static void PresentTraceEnter(uint32_t frame, const char* step)
{
	CrashTrace::SetPresentStep(step, frame);
	if (!PresentShouldTrace(frame)) return;
	DBG_FILE_LOG("[Present] f=" << frame << " -> " << step);
}

static void PresentTraceLeave(uint32_t frame, const char* step)
{
	if (!PresentShouldTrace(frame)) return;
	DBG_FILE_LOG("[Present] f=" << frame << " <- " << step);
}

static void PresentTraceSlow(uint32_t frame, const char* step,
	std::chrono::steady_clock::time_point t0)
{
	const double ms = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - t0).count();
	if (ms >= 80.0)
		DBG_FILE_LOG("[Present] f=" << frame << " SLOW " << step << " " << ms << "ms");
}

static void PresentTraceHeartbeatLine(uint32_t frame)
{
	if (!PresentTraceHeartbeat(frame)) return;
	DBG_FILE_LOG("[Present] f=" << frame << " heartbeat bootgate="
		<< static_cast<int>(BootGate::Current())
		<< " local=" << (GameState::GetLocalPtr() != nullptr)
		<< " world=" << (GameState::GetWorldMgr() != nullptr)
		<< " aimHooks=" << AimHooks::IsInstalled()
		<< " imguiAllowed=" << (ImGuiRenderAllowed(frame) ? 1 : 0));
}

static float s_cachedScreenW = 0.f;
static float s_cachedScreenH = 0.f;


void UpdateCachedClientSize()
{
	HWND wnd = DirectX::window;
	if (!wnd)
		return;
	RECT rc{};
	GetClientRect(wnd, &rc);
	s_cachedScreenW = static_cast<float>(rc.right - rc.left);
	s_cachedScreenH = static_cast<float>(rc.bottom - rc.top);
}

void DrawFpsOverlayTopCameraRect()
{
	ImDrawList* fg = ImGui::GetForegroundDrawList();
	if (!fg)
		return;

	if (kImGuiStage >= 3) {
		// Keep Unity pixelRect fresh even when Debug/Test overlays are idle (menu closed).
		static float s_camRectRefreshAccum = 0.f;
		s_camRectRefreshAccum += ImGui::GetIO().DeltaTime;
		if (s_camRectRefreshAccum >= 0.2f) {
			s_camRectRefreshAccum = 0.f;
			CameraTAB::ForceRefresh();
		}
	}

	if (!DirectX::window)
		return;
	const float screenW = s_cachedScreenW;
	const float screenH = s_cachedScreenH;
	if (screenW <= 0.f || screenH <= 0.f)
		return;

	float centerX;
	float textY;
	if (kImGuiStage >= 3) {
		const float prX = CameraTAB::GetPixelRectX();
		const float prY = CameraTAB::GetPixelRectY();
		const float prW = CameraTAB::GetPixelRectW();
		const float prH = CameraTAB::GetPixelRectH();
		if (prW > 16.f && prH > 16.f) {
			centerX = prX + prW * 0.5f;
			textY = screenH - (prY + prH) + 6.f;
		} else {
			centerX = screenW * 0.5f;
			textY = 6.f;
		}
	} else {
		centerX = screenW * 0.5f;
		textY = 6.f;
	}

	const float fps = ImGui::GetIO().Framerate;
	char buf[48];
	std::snprintf(buf, sizeof(buf), "%.0f FPS", fps);

	const ImVec2 ts = ImGui::CalcTextSize(buf);
	const ImVec2 pos(centerX - ts.x * 0.5f, textY);
	fg->AddText(ImVec2(pos.x + 1.f, pos.y + 1.f), IM_COL32(0, 0, 0, 200), buf);
	fg->AddText(pos, IM_COL32(220, 255, 200, 255), buf);
}
} // namespace

D3D_PRESENT_FUNCTION oPresent = nullptr;
HWND DirectX::window = nullptr;
HANDLE DirectX::hRenderSemaphore = nullptr;
ID3D11Device* DirectX::pDevice = nullptr;
ID3D11DeviceContext* DirectX::pContext = nullptr;
static ID3D11RenderTargetView* pRenderTargetView = nullptr;
static WNDPROC oWndProc = nullptr;
static std::atomic<bool> g_unloading{false};

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static MouseStateCache mouseCache;

LRESULT __stdcall dWndProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	if (g_unloading)
		return CallWindowProc(oWndProc, hWnd, uMsg, wParam, lParam);

	KeyBinds::WndProc(uMsg, wParam, lParam);

	if (uMsg == WM_SIZE) {
		UpdateCachedClientSize();
		if (pRenderTargetView) {
			pRenderTargetView->Release();
			pRenderTargetView = nullptr;
		}
	}
	return CallWindowProc(oWndProc, hWnd, uMsg, wParam, lParam);
}

HRESULT __stdcall dPresent(IDXGISwapChain* __this, UINT SyncInterval, UINT Flags) {
	if (g_unloading)
		return oPresent(__this, SyncInterval, Flags);

	const uint32_t frame = ++s_presentFrame;
	const bool trace = PresentShouldTrace(frame);
	if (trace) {
		DBG_FILE_LOG("[Present] f=" << frame << " begin bootgate="
			<< static_cast<int>(BootGate::Current()));
	}

	auto stepT0 = std::chrono::steady_clock::now();

	// DEBUG BISECT #2: SpeedHack::Tick lazily installs IL2CPP hooks via
	// Detours every frame on the render thread. Detours' transaction
	// commit suspends all other threads — if any holds the IL2CPP lock
	// when Tick fires, install hangs forever and the game freezes. The
	// rewrite from MinHook to Detours in the Bugs merge is the most
	// likely cause of "inject then freeze". Re-enable once the install
	// path is moved off the render thread (e.g. one-shot in Load(), or
	// a worker thread) — or reverted to MinHook.
	// XRebuild-style speedhack hooks install lazily once IL2CPP is ready.
	// SpeedHack::Tick();
	PresentTraceEnter(frame, "FpsSetter::Tick");
	FpsSetter::Tick();
	PresentTraceLeave(frame, "FpsSetter::Tick");

	// Present-level FPS cap (busy-wait, matches XRebuild dPresent approach).
	{
		static auto s_lastPresent = std::chrono::steady_clock::now();
		const int targetFps = FpsSetter::GetTargetFps();
		if (targetFps > 0) {
			const double targetMs = 1000.0 / static_cast<double>(targetFps);
			auto now = std::chrono::steady_clock::now();
			const double elapsedMs = std::chrono::duration<double, std::milli>(now - s_lastPresent).count();
			if (elapsedMs < targetMs) {
				const double remaining = targetMs - elapsedMs;
				if (remaining > 1.5)
					std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(remaining - 1.0)));
				while (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s_lastPresent).count() < targetMs)
					std::this_thread::yield();
			}
			s_lastPresent = std::chrono::steady_clock::now();
		}
	}

	PresentTraceEnter(frame, "RuntimeOffsets::EnsureAll");
	RuntimeOffsets::EnsureAll();
	PresentTraceSlow(frame, "RuntimeOffsets::EnsureAll", stepT0);
	PresentTraceLeave(frame, "RuntimeOffsets::EnsureAll");
	stepT0 = std::chrono::steady_clock::now();
	// #region agent log — H13/H14: Unity deltaTime snapshot before game DLL ticks
	SpeedHack::LogTimingProbe("present_post_offsets");
	// #endregion
	PresentTraceEnter(frame, "GameState::Tick");
	GameState::Tick();       // resolves AppMgr/WorldMgr/LocalPtr — must be first
	PresentTraceLeave(frame, "GameState::Tick");
	PresentTraceEnter(frame, "HwidCapture::Tick");
	HwidCapture::Tick();     // one-shot per session — calls Deca's DeviceIdHolder.GetDeviceId once IL2CPP is up, writes hwid.txt
	PresentTraceLeave(frame, "HwidCapture::Tick");
	PresentTraceEnter(frame, "LocalPlayer::Tick");
	LocalPlayer::Tick();     // reads stats from GameState::GetLocalPtr()
	PresentTraceLeave(frame, "LocalPlayer::Tick");
	PresentTraceEnter(frame, "NoclipHook::Tick");
	NoclipHook::Tick();      // lazy-install walkability hooks when noclip is enabled
	PresentTraceLeave(frame, "NoclipHook::Tick");
	PresentTraceEnter(frame, "SharedMemory::Tick");
	SharedMemory::Tick();    // shared mapping telemetry (pos + legacy bridges still using shared memory)
	PresentTraceLeave(frame, "SharedMemory::Tick");
	PresentTraceEnter(frame, "FeatureRuntime::ApplyOverrides");
	FeatureRuntime::ApplyOverrides(); // unified pipe-driven feature sync
	PresentTraceLeave(frame, "FeatureRuntime::ApplyOverrides");
	PresentTraceEnter(frame, "SkinChanger::Tick");
	SkinChanger::Tick();     // writes skin when ptr changes — uses GameState
	PresentTraceLeave(frame, "SkinChanger::Tick");
	// #region agent log
	SpeedHack::LogTimingProbe("pre_apply_timescale");
	// #endregion
	PresentTraceEnter(frame, "BootGate::Tick");
	BootGate::Tick();        // boot/audit loop — must run before feature hooks install
	PresentTraceSlow(frame, "BootGate::Tick", stepT0);
	PresentTraceLeave(frame, "BootGate::Tick");
	stepT0 = std::chrono::steady_clock::now();
	PresentTraceEnter(frame, "AutoAim::Tick");
	AutoAim::Tick();         // entity dict walk — uses GameState::GetWorldMgr()
	PresentTraceLeave(frame, "AutoAim::Tick");
	PresentTraceEnter(frame, "BagLooter::Tick");
	BagLooter::Tick();       // throttled bag scan + ext-goal routing
	PresentTraceLeave(frame, "BagLooter::Tick");
	PresentTraceEnter(frame, "DiagBridge::Tick");
	DiagBridge::Tick();      // mirror live state to %LOCALAPPDATA%\RealmEngine\diag.json
	PresentTraceLeave(frame, "DiagBridge::Tick");

	if (!DirectX::hRenderSemaphore)
		DirectX::hRenderSemaphore = CreateSemaphore(nullptr, 1, 1, nullptr);

	static std::once_flag init_flag;
	static bool s_imguiInitLogged = false;

	if (WaitForSingleObject(DirectX::hRenderSemaphore, 0) == WAIT_OBJECT_0) {
		PresentTraceEnter(frame, "ImGuiRender");
		if (!ImGuiRenderAllowed(frame)) {
			if (trace)
				DBG_FILE_LOG("[Present] f=" << frame << " ImGuiRender deferred (stage="
					<< static_cast<int>(kImGuiStage) << " minFrame=" << kImGuiMinFrame
					<< " maxHp=" << LocalPlayer::GetMaxHP() << ")");
			ReleaseSemaphore(DirectX::hRenderSemaphore, 1, nullptr);
			PresentTraceLeave(frame, "ImGuiRender");
		} else {
		static bool s_loggedImGuiAllowed = false;
		if (!s_loggedImGuiAllowed) {
			s_loggedImGuiAllowed = true;
			DBG_FILE_LOG("[Present] f=" << frame << " ImGuiRender ALLOWED (first frame)");
		}
		std::call_once(init_flag, [&]() {
			DBG_FILE_LOG("[Present] ImGui init begin (f=" << frame << ")");
			__this->GetDevice(__uuidof(ID3D11Device), (void**)&DirectX::pDevice);
			DBG_FILE_LOG("[Present] ImGui init GetDevice done (f=" << frame << ")");
			DirectX::pDevice->GetImmediateContext(&DirectX::pContext);
			DBG_FILE_LOG("[Present] ImGui init GetImmediateContext done (f=" << frame << ")");
			DXGI_SWAP_CHAIN_DESC sd; __this->GetDesc(&sd);
			DirectX::window = sd.OutputWindow;
			UpdateCachedClientSize();
			DBG_FILE_LOG("[Present] ImGui init CreateContext (f=" << frame << ")");
			ImGui::CreateContext();
			DBG_FILE_LOG("[Present] ImGui init Win32_Init (f=" << frame << ")");
			ImGui_ImplWin32_Init(DirectX::window);
			DBG_FILE_LOG("[Present] ImGui init DX11_Init (f=" << frame << ")");
			ImGui_ImplDX11_Init(DirectX::pDevice, DirectX::pContext);
			DBG_FILE_LOG("[Present] ImGui init WndProc hook (f=" << frame << ")");
			oWndProc = (WNDPROC)SetWindowLongPtr(DirectX::window, GWLP_WNDPROC, (LONG_PTR)dWndProc);
			settings.ImGuiInitialized = true;
			DBG_FILE_LOG("[Present] ImGui init done (f=" << frame << ")");
			s_imguiInitLogged = true;
		});
		if (!pRenderTargetView) {
			PresentTraceEnter(frame, "ImGui/BackBuffer");
			ID3D11Texture2D* pBackBuffer = nullptr;
			__this->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer);
			DirectX::pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &pRenderTargetView);
			pBackBuffer->Release();
			PresentTraceLeave(frame, "ImGui/BackBuffer");
		}

		PresentTraceEnter(frame, "ImGui/DX11NewFrame");
		ImGui_ImplDX11_NewFrame();
		PresentTraceLeave(frame, "ImGui/DX11NewFrame");
		PresentTraceEnter(frame, "ImGui/Win32NewFrame");
		ImGui_ImplWin32_NewFrame();
		PresentTraceLeave(frame, "ImGui/Win32NewFrame");
		PresentTraceEnter(frame, "ImGui/CoreNewFrame");
		ImGui::NewFrame();
		PresentTraceLeave(frame, "ImGui/CoreNewFrame");
		if (PresentTraceHeartbeat(frame))
			DBG_FILE_LOG("[Present] f=" << frame << " ImGui NewFrame complete");

		ImGui::GetIO().MouseDrawCursor = false;

		if (kImGuiStage >= 5) {
			if (KeyBinds::IsKeyPressed(settings.KeyBinds.Toggle_Menu))
				settings.bShowMenu = !settings.bShowMenu;
		}

		if (kImGuiStage >= 4) {
			PresentTraceEnter(frame, "ImGui/TabTicks");
			TestTAB::Tick(settings.bShowMenu);
			VisualsTAB::Tick(settings.bShowMenu);
			CombatTAB::Tick(settings.bShowMenu);
			PlayerTAB::Tick(settings.bShowMenu);
			PresentTraceLeave(frame, "ImGui/TabTicks");
		}

		if (kImGuiStage >= 2) {
			PresentTraceEnter(frame, "ImGui/FpsOverlay");
			DrawFpsOverlayTopCameraRect();
			PresentTraceLeave(frame, "ImGui/FpsOverlay");
		}

		if (kImGuiStage >= 5 && settings.bShowMenu) {
			PresentTraceEnter(frame, "ImGui/Menu");
			ImGui::SetNextWindowSize(ImVec2(1000, 36), ImGuiCond_Always);
			ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
			ImGui::Begin("##MenuBar", nullptr,
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
				ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
				ImGuiWindowFlags_NoSavedSettings);

			static int s_tab = 0;
			const char* tabs[] = { "World", "Camera", "Player", "Combat", "Visuals", "Test" };
			for (int i = 0; i < IM_ARRAYSIZE(tabs); i++) {
				if (i > 0) ImGui::SameLine();
				if (ImGui::Button(tabs[i])) s_tab = i;
			}
			ImGui::End();

			ImGui::SetNextWindowSize(ImVec2(420, 560), ImGuiCond_Always);
			ImGui::SetNextWindowPos(ImVec2(0, 36), ImGuiCond_Always);
			ImGui::Begin("##MenuContent", nullptr,
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
				ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
			switch (s_tab) {
				case 0: WorldTAB::Render();  break;
				case 1: CameraTAB::Render(); break;
				case 2: PlayerTAB::Render(); break;
				case 3: CombatTAB::Render(); break;
				case 4: VisualsTAB::Render(); break;
				case 5: TestTAB::Render();   break;
			}
			ImGui::End();
			PresentTraceLeave(frame, "ImGui/Menu");
		}

		if (kImGuiStage >= 5) {
			PresentTraceEnter(frame, "ImGui/ChatToast");
			ChatToast::Render();
			PresentTraceLeave(frame, "ImGui/ChatToast");
		}

		PresentTraceEnter(frame, "ImGui/Draw");
		ImGui::Render();
		DirectX::pContext->OMSetRenderTargets(1, &pRenderTargetView, nullptr);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		PresentTraceLeave(frame, "ImGui/Draw");
		ReleaseSemaphore(DirectX::hRenderSemaphore, 1, nullptr);
		PresentTraceLeave(frame, "ImGuiRender");
		}
	}
	if (trace) {
		DBG_FILE_LOG("[Present] f=" << frame << " end bootgate="
			<< static_cast<int>(BootGate::Current()));
	}
	PresentTraceHeartbeatLine(frame);
	return oPresent(__this, SyncInterval, Flags);
}

void DirectX::Shutdown() {
	g_unloading = true;

	if (DirectX::hRenderSemaphore) {
		WaitForSingleObject(DirectX::hRenderSemaphore, 5000);

		if (oWndProc && DirectX::window)
			SetWindowLongPtr(DirectX::window, GWLP_WNDPROC, (LONG_PTR)oWndProc);
		oWndProc = nullptr;

		if (mouseCache.hasCached)
			DirectX::ApplyMouseState(mouseCache.wasVisible, mouseCache.wasLockState);

		settings.ImGuiInitialized = false;
		ImGui_ImplDX11_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();

		if (pRenderTargetView) { pRenderTargetView->Release(); pRenderTargetView = nullptr; }
		if (DirectX::pContext)  { DirectX::pContext->Release();  DirectX::pContext = nullptr; }
		if (DirectX::pDevice)   { DirectX::pDevice->Release();   DirectX::pDevice = nullptr; }

		CloseHandle(DirectX::hRenderSemaphore);
		DirectX::hRenderSemaphore = nullptr;
	}

	DirectX::window = nullptr;
}

void DirectX::CacheCurrentMouseState()
{
	Resolver::Protection::safe_call([&]() {
		Il2CppClass* cursorClass = Resolver::FindClass("UnityEngine", "Cursor");
		if (!cursorClass) return;

		const MethodInfo* getVis = il2cpp_class_get_method_from_name(cursorClass, "get_visible", 0);
		const MethodInfo* getLock = il2cpp_class_get_method_from_name(cursorClass, "get_lockState", 0);

		if (getVis && getLock) {
			Il2CppObject* visObj = il2cpp_runtime_invoke(getVis, nullptr, nullptr, nullptr);
			Il2CppObject* lockObj = il2cpp_runtime_invoke(getLock, nullptr, nullptr, nullptr);

			if (visObj) mouseCache.wasVisible = *static_cast<bool*>(il2cpp_object_unbox(visObj));
			if (lockObj) mouseCache.wasLockState = *static_cast<int*>(il2cpp_object_unbox(lockObj));

			mouseCache.hasCached = true;
		}
		});
}

void DirectX::ApplyMouseState(bool visible, int lockState)
{
	Resolver::Protection::safe_call([&]() {
		Il2CppClass* cursorClass = Resolver::FindClass("UnityEngine", "Cursor");
		if (!cursorClass) return;

		const MethodInfo* setVis = il2cpp_class_get_method_from_name(cursorClass, "set_visible", 1);

		const MethodInfo* setLock = il2cpp_class_get_method_from_name(cursorClass, "set_lockState", 1);

		if (setVis && setLock) {
			void* pVis[] = { &visible };
			void* pLock[] = { &lockState };

			il2cpp_runtime_invoke(setLock, nullptr, pLock, nullptr);
			il2cpp_runtime_invoke(setVis, nullptr, pVis, nullptr);
		}
		});
}

