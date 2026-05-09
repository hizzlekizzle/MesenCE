#pragma once
#include "libretro.h"
#include <atomic>
#include <cstdio>
#include <thread>
#include <functional>
#include "../Core/Shared/Interfaces/IKeyManager.h"
#include "../Core/Shared/KeyManager.h"
#include "../Core/Shared/SystemActionManager.h"
//#include "../Core/VsSystemActionManager.h"
#include "../Core/Shared/Interfaces/IConsole.h"

// Forward-declare Emulator so we can pass a pointer without pulling headers into this
class Emulator;

class LibretroKeyManager : public IKeyManager
{
private:
    std::shared_ptr<IConsole> _console;
    // pointer to the global Emulator instance (may be nullptr in some contexts)
    Emulator* _emuPtr = nullptr;
    std::atomic<retro_input_state_t> _getInputState { nullptr };
	int16_t _joypadButtons[5] = { };
    std::atomic<retro_input_poll_t> _pollInput { nullptr };
    // The thread id of the thread that constructed this key manager (typically the frontend/main thread).
    // Input callbacks registered by the frontend must only be called from that thread.
    std::thread::id _mainThreadId;
    // Hash of the thread id which set the input callbacks (set when SetGetInputState/SetPollInput are called).
    // We avoid calling frontend callbacks from other threads (e.g., ShortcutKeyHandler) because many
    // frontends are not thread-safe for input callbacks.
    std::atomic<size_t> _ownerThreadHash { 0 };
    // Backend readiness is handled at the KeyManager wrapper level via
    // KeyManager::SetBackendReady().
	bool _mouseButtons[3] = { false, false, false };
	bool _supportsInputBitmasks = false;
	bool _wasPushed[16] = { };

	bool ProcessAction(uint32_t button)
	{
		bool buttonPressed = false;
		if (_supportsInputBitmasks)
			buttonPressed = (_joypadButtons[0] & (1 << button)) != 0;
		else
        {
            auto getState = _getInputState.load();
            if(getState) buttonPressed = getState(0, RETRO_DEVICE_JOYPAD, 0, button) != 0;
            else buttonPressed = false;
        }

		if(buttonPressed) {
			if(!_wasPushed[button]) {
				//Newly pressed, process action
				_wasPushed[button] = true;
				return true;
			}
		} else {
			_wasPushed[button] = false;
		}
		return false;
	}
	
public:
    LibretroKeyManager(std::shared_ptr<IConsole> console, Emulator* emu = nullptr)
    {
        _console = console;
        _emuPtr = emu;
        _mainThreadId = std::this_thread::get_id();
    }

    // Update the console pointer when the emulator loads a ROM and the concrete
    // console instance becomes available. This lets RefreshState avoid early exits
    // and allows SetGetInputState/SetPollInput to mark the backend ready.
    void SetConsole(std::shared_ptr<IConsole> console, Emulator* emu = nullptr)
    {
        _console = console;
        if(emu) _emuPtr = emu;
        // If callbacks were already registered, mark the KeyManager backend ready
        if(_getInputState.load() || _pollInput.load()) {
            KeyManager::SetBackendReady(true);
        }
    }

    ~LibretroKeyManager()
    {
    }

    void SetGetInputState(retro_input_state_t getInputState)
    {
        _getInputState.store(getInputState);
        if(getInputState) {
            if(getenv("MESEN_LIBRETRO_VERBOSE_INPUT")) fprintf(stderr, "[libretro] SetGetInputState called\n");
            size_t h = std::hash<std::thread::id>()(std::this_thread::get_id());
            _ownerThreadHash.store(h);
            if(_console) KeyManager::SetBackendReady(true);
        } else {
            _ownerThreadHash.store(0);
            KeyManager::SetBackendReady(false);
        }
    }

    void SetPollInput(retro_input_poll_t pollInput)
    {
        _pollInput.store(pollInput);
        if(pollInput) {
            if(getenv("MESEN_LIBRETRO_VERBOSE_INPUT")) fprintf(stderr, "[libretro] SetPollInput called\n");
            size_t h = std::hash<std::thread::id>()(std::this_thread::get_id());
            _ownerThreadHash.store(h);
            if(_console) KeyManager::SetBackendReady(true);
        } else {
            _ownerThreadHash.store(0);
            KeyManager::SetBackendReady(false);
        }
    }

    void SetSupportsInputBitmasks(bool supportsInputBitmasks)
    {
        _supportsInputBitmasks = supportsInputBitmasks;
    }

    // Inherited via IKeyManager
    virtual void RefreshState() override
    {
        // One-time init for optional raw-button probing when bitmasks report zero.
        // Set MESEN_LIBRETRO_RAWBTN_FRAMES to a positive integer to enable probing
        // individual buttons for that many frames (e.g., export MESEN_LIBRETRO_RAWBTN_FRAMES=120).
        static int _rawProbeFrames = -1;
        if(_rawProbeFrames == -1) {
            const char* ev = getenv("MESEN_LIBRETRO_RAWBTN_FRAMES");
            if(ev) _rawProbeFrames = atoi(ev);
            else _rawProbeFrames = 0;
        }
        // One-time init for optional verbose input logging (useful for debugging).
        // Set MESEN_LIBRETRO_VERBOSE_INPUT to a positive integer to force per-frame
        // mask prints for that many frames (e.g., export MESEN_LIBRETRO_VERBOSE_INPUT=300).
        static int _verboseInputFrames = -1;
        if(_verboseInputFrames == -1) {
            const char* ev = getenv("MESEN_LIBRETRO_VERBOSE_INPUT");
            if(ev) _verboseInputFrames = atoi(ev);
            else _verboseInputFrames = 0;
        }

        // Fast path: never call frontend callbacks from threads other than the one
        // that constructed this key manager. Avoid computing the current thread
        // hash unless an owner was previously set (cheap-path when unset).
        size_t owner = _ownerThreadHash.load();
        size_t curHash = 0;
        // Defensive checks: if core/frontend objects aren't ready yet, bail out.
        // We've observed cases where the libretro host registers input callbacks before
        // the console/shared objects are available; calling into KeyManager or the
        // emulator with null pointers can cause crashes. Skip processing until
        // initialization is complete.
        if(!_console) {
            // Not initialized yet, ensure KeyManager isn't considered ready.
            KeyManager::SetBackendReady(false);
            // suppressed: if(getenv("MESEN_LIBRETRO_VERBOSE_INPUT")) fprintf(stderr, "[libretro] RefreshState: no console available yet\n");
            return;
        }
        if(std::this_thread::get_id() != _mainThreadId) {
            return;
        }
        // Full input refresh behavior:
        // - Call the poll callback if present
        // - Read joypad state for ports 0..4 either via bitmasks (if supported)
        //   or by querying each button individually
        // - Read pointer (mouse) X/Y/Pressed for port 0 and forward position to KeyManager
        // Avoid calling the frontend input callbacks from a different thread than the one
        // that registered them. Some frontends (including RetroArch) expect input callbacks
        // to be called only from the thread that set them; calling them from the
        // ShortcutKeyHandler background thread can cause crashes.
        // If the owner hash was set when callbacks were registered, ensure the
        // calling thread matches that owner. Only compute curHash when owner!=0.
        if(owner != 0) {
            curHash = std::hash<std::thread::id>()(std::this_thread::get_id());
            if(curHash != owner) {
                // suppressed: if(getenv("MESEN_LIBRETRO_VERBOSE_INPUT")) fprintf(stderr, "[libretro] RefreshState: thread mismatch owner=%zu cur=%zu\n", owner, curHash);
                return;
            }
        }

        auto poll = _pollInput.load();
        if(poll) {
            try {
                // Quiet: do not print poll() calls to avoid noisy logs in normal runs.
                // if(getenv("MESEN_LIBRETRO_VERBOSE_INPUT")) fprintf(stderr, "[libretro] RefreshState: calling poll (poll=%p getState=%p supportsMask=%d)\n", (void*)poll, (void*)_getInputState.load(), (int)_supportsInputBitmasks);
                poll();
            } catch(...) {
                // if(getenv("MESEN_LIBRETRO_VERBOSE_INPUT")) fprintf(stderr, "[libretro] RefreshState: poll() threw\n");
            }
        } else {
            if(getenv("MESEN_LIBRETRO_VERBOSE_INPUT")) fprintf(stderr, "[libretro] RefreshState: poll callback is null (getState=%p supportsMask=%d)\n", (void*)_getInputState.load(), (int)_supportsInputBitmasks);
        }

        auto getState = _getInputState.load();
        if(!getState) {
            if(getenv("MESEN_LIBRETRO_VERBOSE_INPUT")) fprintf(stderr, "[libretro] RefreshState: no getState callback (poll=%p supportsMask=%d)\n", (void*)poll, (int)_supportsInputBitmasks);
            return;
        } else {
            // Log pointer addresses to ensure the callbacks are the same as those set by the frontend
            if(getenv("MESEN_LIBRETRO_VERBOSE_INPUT")) fprintf(stderr, "[libretro] RefreshState: getState=%p poll=%p supportsMask=%d\n", (void*)getState, (void*)poll, (int)_supportsInputBitmasks);
        }

        // Query joypad state for ports 0..4
            static int _dbgRefreshCount = 0;
            for(int port = 0; port < 5; ++port) {
            int16_t mask = 0;
            if(_supportsInputBitmasks) {
                // Request the joystick bitmask in one call
                try {
                    int32_t val = getState(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
                    mask = (int16_t)(val & 0xFFFF);
                } catch(...) {
                    mask = 0;
                }
                // If the bitmask is zero but raw probing is enabled, perform per-button queries
                if(mask == 0 && _rawProbeFrames > 0) {
                    // suppressed: raw-probe active debug prints
                    int probeMask = 0;
                    for(int b = 0; b < 16; ++b) {
                        try {
                            int16_t v = getState(port, RETRO_DEVICE_JOYPAD, 0, b);
                            if(v) {
                                probeMask |= (1 << b);
                                // suppressed: fprintf(stderr, "[libretro] RefreshState: raw-probe port %d btn %d = %d\n", port, b, (int)v);
                            }
                        } catch(...) {
                        }
                    }
                    // suppressed: if(probeMask != 0) fprintf(stderr, "[libretro] RefreshState: raw-probe computed mask 0x%04x for port %d\n", probeMask, port);
                }
            } else {
                // Query each button individually (0..15)
                for(int b = 0; b < 16; ++b) {
                    try {
                        int16_t v = getState(port, RETRO_DEVICE_JOYPAD, 0, b);
                        if(v) mask |= (1 << b);
                    } catch(...) {
                        // ignore
                    }
                }
            }
            _joypadButtons[port] = mask;
            // Print diagnostics only when MESEN_LIBRETRO_VERBOSE_INPUT is enabled.
            bool wantPrint = (_verboseInputFrames > 0);
            if(wantPrint) {
                // suppressed: fprintf(stderr, "[libretro] RefreshState: port %d mask 0x%04x\n", port, (unsigned)mask);
            }
            if(_verboseInputFrames > 0) {
                // decrement after printing for all ports (only once per frame)
                if(port == 4) _verboseInputFrames--;
            }
            if(_rawProbeFrames > 0 && port == 4) _rawProbeFrames--;
            if(port == 4) _dbgRefreshCount++;
        }

        // Read pointer (mouse) on port 0: X/Y and pressed
        try {
            int16_t rawX = getState(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
            int16_t rawY = getState(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
            int16_t pressed = getState(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED);

            // Normalize coordinates into 0..1 range. The frontend typically provides unsigned coords
            // but the callback type is int16_t; interpret as uint16_t and clamp to [0,1].
            double relX = std::min(1.0, (double)(uint16_t)rawX / 65535.0);
            double relY = std::min(1.0, (double)(uint16_t)rawY / 65535.0);

            if(_emuPtr) {
                KeyManager::SetMousePosition(_emuPtr, relX, relY);
            }

            _mouseButtons[0] = (pressed != 0);
            // leave other mouse buttons unchanged as libretro pointer only reports primary press
        } catch(...) {
            // ignore pointer read errors
        }
    }

    // Readiness is now communicated via KeyManager::SetBackendReady/IsReady()

    // Implementations required by the current IKeyManager interface:

    // update device list / poll devices (no-op here, keep compatibility)
    virtual void UpdateDevices() override
    {
        // libretro backend drives input via RefreshState(), so nothing needed
    }

    // Return currently pressed keys (not used by libretro backend; return empty)
    virtual vector<uint16_t> GetPressedKeys() override
    {
        return {};
    }

    // IsKeyPressed signature updated to match IKeyManager
    virtual bool IsKeyPressed(uint16_t keyCode) override
    {
        if(keyCode > 0 && _getInputState.load())
        {
            // old implementation assumed (port<<8) | (retroKey+1)
            int port = (keyCode >> 8) & 0xFF;
            int retroKey = (keyCode & 0xFF) - 1;
            if(retroKey < 0) return false;
            if(_supportsInputBitmasks) {
                // bitmask path
                return (_joypadButtons[port] & (1 << retroKey)) != 0;
            }
            auto getState = _getInputState.load();
            if(getState) return getState(port, RETRO_DEVICE_JOYPAD, 0, retroKey) != 0;
            return false;
        }
        return false;
    }

    virtual bool IsMouseButtonPressed(MouseButton button) override
    {
        return _mouseButtons[(int)button];
    }

    // GetKeyName/GetKeyCode signatures updated to match IKeyManager
    virtual string GetKeyName(uint16_t keyCode) override
    {
        // Not used; return empty string
        return string();
    }

    virtual uint16_t GetKeyCode(string keyName) override
    {
        // Not used; return 0
        return 0;
    }

    // New control methods required by IKeyManager
    virtual bool SetKeyState(uint16_t scanCode, bool state) override
    {
        // Not supported in this backend
        (void)scanCode; (void)state;
        return false;
    }

    virtual void ResetKeyState() override
    {
        // No persistent key state to clear
    }

    virtual void SetDisabled(bool disabled) override
    {
        // No-op for libretro backend
        (void)disabled;
    }

    // Optional force feedback (kept as no-op)
    virtual void SetForceFeedback(uint16_t magnitudeRight, uint16_t magnitudeLeft) override
    {
        (void)magnitudeRight; (void)magnitudeLeft;
    }
};
