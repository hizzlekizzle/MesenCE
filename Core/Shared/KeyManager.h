#pragma once
#include "pch.h"
#include "Shared/Interfaces/IKeyManager.h"
#include "Utilities/SimpleLock.h"
#include <memory>

class Emulator;
class EmuSettings;

class KeyManager
{
private:
#ifdef LIBRETRO
	static std::shared_ptr<IKeyManager> _keyManager;
	// Backend readiness flag set by the registered backend when fully initialized.
	static std::atomic<bool> _backendReady;
#else
	static IKeyManager* _keyManager;
#endif
	static MousePosition _mousePosition;
	static double _xMouseMovement;
	static double _yMouseMovement;
	static EmuSettings* _settings;
	static SimpleLock _lock;

public:
#ifdef LIBRETRO
	static void RegisterKeyManager(std::shared_ptr<IKeyManager> keyManager);
	// Called by the registered backend when it has finished initializing
	// internal state (callbacks, console pointer, etc.) and is safe to be
	// polled from background threads.
	static void SetBackendReady(bool ready);
	static bool IsReady();
#else
	static void RegisterKeyManager(IKeyManager* keyManager);
#endif
	static void SetSettings(EmuSettings* settings);

	static void RefreshKeyState();
	static bool IsKeyPressed(uint16_t keyCode);
	static optional<int16_t> GetAxisPosition(uint16_t keyCode);
	static bool IsMouseButtonPressed(MouseButton button);
	static vector<uint16_t> GetPressedKeys();
	static string GetKeyName(uint16_t keyCode);
	static uint16_t GetKeyCode(string keyName);

	static void UpdateDevices();

	static void SetMouseMovement(int16_t x, int16_t y);
	static MouseMovement GetMouseMovement(Emulator* emu, uint32_t mouseSensitivity);

	static void SetMousePosition(Emulator* emu, double x, double y);
	static MousePosition GetMousePosition();

	static void SetForceFeedback(uint16_t magnitude);
	static void SetForceFeedback(uint16_t magnitudeRight, uint16_t magnitudeLeft);
};
