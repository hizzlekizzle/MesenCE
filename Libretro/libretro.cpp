#include <string>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <sstream>
#include <algorithm>
#include <fstream>
#include <vector>
#include <iterator>
#include <sys/stat.h>
#if __has_include(<filesystem>)
	#include <filesystem>
	namespace fs = std::filesystem;
	#define HAS_STD_FILESYSTEM 1
#elif __has_include(<experimental/filesystem>)
	#include <experimental/filesystem>
	namespace fs = std::experimental::filesystem;
	#define HAS_STD_FILESYSTEM 1
#endif
#include "LibretroKeyManager.h"
#include "LibretroMessageManager.h"
#include "libretro.h"
#include "../Core/Shared/Audio/SoundMixer.h"

#include "../Core/Shared/BaseControlManager.h"

#include "../Core/NES/NesConsole.h"
#include "../Core/SNES/SnesConsole.h"
#include "../Core/Gameboy/Gameboy.h"
#include "../Core/GBA/GbaConsole.h"
#include "../Core/PCE/PceConsole.h"
#include "../Core/SMS/SmsConsole.h"
#include "../Core/WS/WsConsole.h"
#include "../Core/Shared/Video/VideoDecoder.h"
#include "../Core/Shared/Video/BaseVideoFilter.h"
#include "../Core/Shared/Video/VideoRenderer.h"
#include "../Core/Shared/ColorUtilities.h"
#include "../Core/NES/NesMemoryManager.h"
#include "../Core/NES/BaseMapper.h"
#include "../Core/Shared/EmuSettings.h"
#include "../Core/Shared/CheatManager.h"
#include "../Core/NES/HdPacks/HdData.h"
#include "../Core/Shared/SaveStateManager.h"
#include "../Core/Debugger/DebugTypes.h"
#include "../Core/NES/GameDatabase.h"
#include "../Core/NES/NesSoundMixer.h"
#include "../Core/Shared/Interfaces/IAudioDevice.h"
#include "../Utilities/FolderUtilities.h"
#include "../Utilities/HexUtilities.h"
#include "../Utilities/VirtualFile.h"
#include "VfsFile.h"
#include "../Utilities/UTF8Util.h"

#define DEVICE_AUTO               RETRO_DEVICE_JOYPAD
#define DEVICE_GAMEPAD            RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0)
#define DEVICE_POWERPAD           RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 1)
#define DEVICE_FAMILYTRAINER      RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 2)
#define DEVICE_PARTYTAP           RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 3)
#define DEVICE_PACHINKO           RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 4)
#define DEVICE_EXCITINGBOXING     RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 5)
#define DEVICE_KONAMIHYPERSHOT    RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 6)
#define DEVICE_SNESGAMEPAD        RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 7)
#define DEVICE_GAMEBOYGAMEPAD     RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 8)
#define DEVICE_VBGAMEPAD          RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 9)
#define DEVICE_ZAPPER             RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_POINTER, 0)
#define DEVICE_OEKAKIDS           RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_POINTER, 1)
#define DEVICE_BANDAIHYPERSHOT    RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_POINTER, 2)
#define DEVICE_ARKANOID           RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_MOUSE, 0)
#define DEVICE_HORITRACK          RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_MOUSE, 1)
#define DEVICE_SNESMOUSE          RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_MOUSE, 2)
#define DEVICE_ASCIITURBOFILE     RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_NONE, 0)
#define DEVICE_BATTLEBOX          RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_NONE, 1)
#define DEVICE_FOURPLAYERADAPTER  RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_NONE, 2)

static retro_log_printf_t logCallback = nullptr;
retro_environment_t env_cb = nullptr;
static unsigned _inputDevices[5] = { DEVICE_AUTO, DEVICE_AUTO, DEVICE_AUTO, DEVICE_AUTO, DEVICE_AUTO };
static bool _hdPacksEnabled = false;
static string _mesenVersion = "";
static int32_t _saveStateSize = -1;
static bool _shiftButtonsClockwise = false;
static int32_t _audioSampleRate = 44100;

//Include game database as a byte array (representing the MesenDB.txt file)
#include "MesenDB.inc"

static std::unique_ptr<Emulator> _emu;
// shared_ptr to the console (IConsole interface, may be null until emulator initialized)
// Supports all console types: NES, SNES, Gameboy, GBA, PC Engine, SMS, Wonderswan
static std::shared_ptr<IConsole> _console;

static bool FolderExists(const string& folder)
{
	if(VfsIo::SupportsFolderOps()) {
		//The path may not exist on the real filesystem (e.g. Android SAF)
		return VfsIo::IsFolder(folder);
	}

#ifdef HAS_STD_FILESYSTEM
	return fs::exists(folder) && fs::is_directory(folder);
#else
	struct stat st;
	return stat(folder.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static bool CreatePceFirmwareStub(const string& systemFolder, string& createdPath, string& backupPath)
{
	if(systemFolder.empty()) {
		return false;
	}

	static const vector<string> firmwareSources = { "syscard3.pce", "syscard3.bin", "syscard.pce", "syscard.bin" };
	const string targetName = "[BIOS] Super CD-ROM System (Japan) (v3.0).pce";
	string target = FolderUtilities::CombinePath(systemFolder, targetName);
	createdPath.clear();
	backupPath.clear();

	VirtualFile targetFile(target);
	if(targetFile.IsValid() && targetFile.GetSize() == 0x40000) {
		return false;
	}

	for(const string& sourceName : firmwareSources) {
		string sourcePath = FolderUtilities::CombinePath(systemFolder, sourceName);
		VirtualFile sourceFile(sourcePath);
		if(!sourceFile.IsValid()) {
			continue;
		}

		uint64_t sourceSize = sourceFile.GetSize();
		if(sourceSize < 0x40000) {
			continue;
		}

		vector<uint8_t> sourceData;
		if(!sourceFile.ReadFile(sourceData) || sourceData.size() < 0x40000) {
			continue;
		}

		size_t sourceOffset = 0;
		if(sourceSize == 0x60200 || sourceSize == 0x40200) {
			sourceOffset = 0x200;
		}

		if(targetFile.IsValid()) {
			backupPath = target + ".libretro.bak";
			int backupIndex = 1;
			while(utf8::ifstream(backupPath, std::ios::binary).good()) {
				backupPath = target + ".libretro.bak" + std::to_string(backupIndex);
				backupIndex++;
			}
			if(!VfsIo::Rename(target, backupPath)) {
				backupPath.clear();
				break;
			}
		}

		utf8::ofstream out(target, std::ios::binary | std::ios::trunc);
		if(!out) {
			if(!backupPath.empty()) {
				VfsIo::Rename(backupPath, target);
			}
			continue;
		}

		out.write((const char*)sourceData.data() + sourceOffset, 0x40000);
		out.close();

		VirtualFile verified(target);
		if(verified.IsValid() && verified.GetSize() == 0x40000) {
			createdPath = target;
			return true;
		}

		if(!backupPath.empty()) {
			VfsIo::Rename(backupPath, target);
		}
	}

	return false;
}

static void RestorePceFirmwareStub(const string& createdPath, const string& backupPath)
{
	if(createdPath.empty()) {
		return;
	}

	if(!backupPath.empty()) {
		VfsIo::Rename(backupPath, createdPath);
	} else {
		VfsIo::Remove(createdPath);
	}
}
static std::shared_ptr<LibretroKeyManager> _keyManager;
static std::unique_ptr<LibretroMessageManager> _message_manager;
static retro_audio_sample_batch_t _audioSampleBatch = nullptr;
static retro_video_refresh_t _videoRefresh = nullptr;
// Saved input callbacks (set by frontend before core may be ready)
static retro_input_state_t _savedGetInputState = nullptr;
static retro_input_poll_t _savedPollInput = nullptr;
// Store user palette locally until renderer API is wired
static std::vector<uint32_t> _userRgbPalette;
// keep selected filter string until we map to EmuSettings
static std::string _selectedNtscFilter;
// local stubs for settings not yet mapped
static bool _videoFilterRaw = false;
static int _ppuNmiBefore = 0;
static int _ppuNmiAfter = 0;
static uint8_t _overscanLeft = 0, _overscanRight = 0, _overscanTop = 0, _overscanBottom = 0;
// store other high-level choices locally until mapped to EmuSettings
static std::string _selectedAspectRatio;
static std::string _selectedRegion;
static std::string _selectedRamState;
// new local stub for screen rotation
static int _screenRotation = 0;

extern "C" void logMessage(retro_log_level level, const char* message);

static bool isSgbDebugEnabled()
{
	static bool initialized = false;
	static bool enabled = false;
	if(!initialized) {
		enabled = getenv("MESEN_LIBRETRO_DEBUG_SGB") != nullptr;
		initialized = true;
	}
	return enabled;
}

static void logSgbDebugf(const char* fmt, ...)
{
	char buffer[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	fprintf(stderr, "[libretro] %s\n", buffer);
	fflush(stderr);
	if(logCallback) {
		logCallback(RETRO_LOG_INFO, buffer);
	}
}

// Geometry tracking
static bool _geometryDirty = false;
static VideoConfig _lastVideoConfig = {};
static NesConfig _lastNesConfig = {};
static uint32_t _lastReportedWidth = 256;
static uint32_t _lastReportedHeight = 240;

// Small audio device implementation that forwards audio from the core's
// SoundMixer to libretro's audio callback (_audioSampleBatch).
class LibretroAudioDevice : public IAudioDevice {
public:
	LibretroAudioDevice() {
	}
	~LibretroAudioDevice() override {
	}

	void PlayBuffer(int16_t *soundBuffer, uint32_t bufferSize, uint32_t sampleRate, bool isStereo) override {
		// bufferSize is number of frames. _audioSampleBatch expects interleaved int16_t samples and frame count.
		if(!_audioSampleBatch) {
			return;
		}

		// Choose output buffer pointer (interleaved samples)
		const int16_t* outPtr = nullptr;
		if(isStereo) {
			outPtr = soundBuffer;
		} else {
			// Convert mono -> stereo by duplicating samples into a temporary buffer
			_monoBuffer.resize(bufferSize * 2);
			for(uint32_t i = 0; i < bufferSize; ++i) {
				int16_t s = soundBuffer[i];
				_monoBuffer[i * 2 + 0] = s;
				_monoBuffer[i * 2 + 1] = s;
			}
			outPtr = _monoBuffer.data();
		}

		// Forward frames (bufferSize is frame count)
		_audioSampleBatch((const int16_t*)outPtr, (size_t)bufferSize);
	}

	void Stop() override {}
	void Pause() override {}
	void ProcessEndOfFrame() override {}

	string GetAvailableDevices() override { return string(); }
	void SetAudioDevice(string deviceName) override { (void)deviceName; }
	AudioStatistics GetStatistics() override { return AudioStatistics(); }

private:
	// reused buffer for mono->stereo conversion
	std::vector<int16_t> _monoBuffer;
};

static std::unique_ptr<LibretroAudioDevice> _audioDevice;

static constexpr const char* MesenNtscFilter = "mesen_ntsc_filter";
static constexpr const char* MesenPalette = "mesen_palette";
static constexpr const char* MesenSpriteLimit = "mesen_sprite_limit";
static constexpr const char* MesenEnablePalBorders = "mesen_enable_pal_borders";
static constexpr const char* MesenSpritesEnabled = "mesen_sprites_enabled";
static constexpr const char* MesenBackgroundEnabled = "mesen_background_enabled";
static constexpr const char* MesenAllowInvalidInput = "mesen_allow_invalid_input";
static constexpr const char* MesenDisableGameGenieBusConflicts = "mesen_disable_game_genie_bus_conflicts";
static constexpr const char* MesenRandomizeMapperPowerOnState = "mesen_randomize_mapper_power_on_state";
static constexpr const char* MesenRandomizeCpuPpuAlignment = "mesen_randomize_cpu_ppu_alignment";
static constexpr const char* MesenOverclock = "mesen_overclock";
static constexpr const char* MesenOverclockType = "mesen_overclock_type";
//static constexpr const char* MesenOverscanLeft = "mesen_overscan_left";
//static constexpr const char* MesenOverscanRight = "mesen_overscan_right";
//static constexpr const char* MesenOverscanTop = "mesen_overscan_up";
//static constexpr const char* MesenOverscanBottom = "mesen_overscan_down";
//static constexpr const char* MesenAspectRatio = "mesen_aspect_ratio";
static constexpr const char* MesenRegion = "mesen_region";
static constexpr const char* MesenRamState = "mesen_ramstate";
static constexpr const char* MesenControllerTurboSpeed = "mesen_controllerturbospeed";
static constexpr const char* MesenFdsAutoSelectDisk = "mesen_fdsautoinsertdisk";
static constexpr const char* MesenFdsFastForwardLoad = "mesen_fdsfastforwardload";
// static constexpr const char* MesenHdPacks = "mesen_hdpacks";
// static constexpr const char* MesenScreenRotation = "mesen_screenrotation";
static constexpr const char* MesenFakeStereo = "mesen_fake_stereo";
static constexpr const char* MesenMuteTriangleUltrasonic = "mesen_mute_triangle_ultrasonic";
static constexpr const char* MesenReduceDmcPopping = "mesen_reduce_dmc_popping";
static constexpr const char* MesenSwapDutyCycle = "mesen_swap_duty_cycle";
static constexpr const char* MesenDisableNoiseModeFlag = "mesen_disable_noise_mode_flag";
// static constexpr const char* MesenShiftButtonsClockwise = "mesen_shift_buttons_clockwise";
static constexpr const char* MesenAudioSampleRate = "mesen_audio_sample_rate";

uint32_t defaultPalette[0x40] { 0xFF666666, 0xFF002A88, 0xFF1412A7, 0xFF3B00A4, 0xFF5C007E, 0xFF6E0040, 0xFF6C0600, 0xFF561D00, 0xFF333500, 0xFF0B4800, 0xFF005200, 0xFF004F08, 0xFF00404D, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFADADAD, 0xFF155FD9, 0xFF4240FF, 0xFF7527FE, 0xFFA01ACC, 0xFFB71E7B, 0xFFB53120, 0xFF994E00, 0xFF6B6D00, 0xFF388700, 0xFF0C9300, 0xFF008F32, 0xFF007C8D, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFFFFEFF, 0xFF64B0FF, 0xFF9290FF, 0xFFC676FF, 0xFFF36AFF, 0xFFFE6ECC, 0xFFFE8170, 0xFFEA9E22, 0xFFBCBE00, 0xFF88D800, 0xFF5CE430, 0xFF45E082, 0xFF48CDDE, 0xFF4F4F4F, 0xFF000000, 0xFF000000, 0xFFFFFEFF, 0xFFC0DFFF, 0xFFD3D2FF, 0xFFE8C8FF, 0xFFFBC2FF, 0xFFFEC4EA, 0xFFFECCC5, 0xFFF7D8A5, 0xFFE4E594, 0xFFCFEF96, 0xFFBDF4AB, 0xFFB3F3CC, 0xFFB5EBF2, 0xFFB8B8B8, 0xFF000000, 0xFF000000 };
uint32_t unsaturatedPalette[0x40] { 0xFF6B6B6B, 0xFF001E87, 0xFF1F0B96, 0xFF3B0C87, 0xFF590D61, 0xFF5E0528, 0xFF551100, 0xFF461B00, 0xFF303200, 0xFF0A4800, 0xFF004E00, 0xFF004619, 0xFF003A58, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFB2B2B2, 0xFF1A53D1, 0xFF4835EE, 0xFF7123EC, 0xFF9A1EB7, 0xFFA51E62, 0xFFA52D19, 0xFF874B00, 0xFF676900, 0xFF298400, 0xFF038B00, 0xFF008240, 0xFF007891, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFF63ADFD, 0xFF908AFE, 0xFFB977FC, 0xFFE771FE, 0xFFF76FC9, 0xFFF5836A, 0xFFDD9C29, 0xFFBDB807, 0xFF84D107, 0xFF5BDC3B, 0xFF48D77D, 0xFF48CCCE, 0xFF555555, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFFC4E3FE, 0xFFD7D5FE, 0xFFE6CDFE, 0xFFF9CAFE, 0xFFFEC9F0, 0xFFFED1C7, 0xFFF7DCAC, 0xFFE8E89C, 0xFFD1F29D, 0xFFBFF4B1, 0xFFB7F5CD, 0xFFB7F0EE, 0xFFBEBEBE, 0xFF000000, 0xFF000000 };
uint32_t yuvPalette[0x40] { 0xFF666666, 0xFF002A88, 0xFF1412A7, 0xFF3B00A4, 0xFF5C007E, 0xFF6E0040, 0xFF6C0700, 0xFF561D00, 0xFF333500, 0xFF0C4800, 0xFF005200, 0xFF004C18, 0xFF003E5B, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFADADAD, 0xFF155FD9, 0xFF4240FF, 0xFF7527FE, 0xFFA01ACC, 0xFFB71E7B, 0xFFB53120, 0xFF994E00, 0xFF6B6D00, 0xFF388700, 0xFF0D9300, 0xFF008C47, 0xFF007AA0, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFF64B0FF, 0xFF9290FF, 0xFFC676FF, 0xFFF26AFF, 0xFFFF6ECC, 0xFFFF8170, 0xFFEA9E22, 0xFFBCBE00, 0xFF88D800, 0xFF5CE430, 0xFF45E082, 0xFF48CDDE, 0xFF4F4F4F, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFFC0DFFF, 0xFFD3D2FF, 0xFFE8C8FF, 0xFFFAC2FF, 0xFFFFC4EA, 0xFFFFCCC5, 0xFFF7D8A5, 0xFFE4E594, 0xFFCFEF96, 0xFFBDF4AB, 0xFFB3F3CC, 0xFFB5EBF2, 0xFFB8B8B8, 0xFF000000, 0xFF000000 };
uint32_t nestopiaRgbPalette[0x40] { 0xFF6D6D6D, 0xFF002492, 0xFF0000DB, 0xFF6D49DB, 0xFF92006D, 0xFFB6006D, 0xFFB62400, 0xFF924900, 0xFF6D4900, 0xFF244900, 0xFF006D24, 0xFF009200, 0xFF004949, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFB6B6B6, 0xFF006DDB, 0xFF0049FF, 0xFF9200FF, 0xFFB600FF, 0xFFFF0092, 0xFFFF0000, 0xFFDB6D00, 0xFF926D00, 0xFF249200, 0xFF009200, 0xFF00B66D, 0xFF009292, 0xFF242424, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFF6DB6FF, 0xFF9292FF, 0xFFDB6DFF, 0xFFFF00FF, 0xFFFF6DFF, 0xFFFF9200, 0xFFFFB600, 0xFFDBDB00, 0xFF6DDB00, 0xFF00FF00, 0xFF49FFDB, 0xFF00FFFF, 0xFF494949, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFFB6DBFF, 0xFFDBB6FF, 0xFFFFB6FF, 0xFFFF92FF, 0xFFFFB6B6, 0xFFFFDB92, 0xFFFFFF49, 0xFFFFFF6D, 0xFFB6FF49, 0xFF92FF6D, 0xFF49FFDB, 0xFF92DBFF, 0xFF929292, 0xFF000000, 0xFF000000 };
uint32_t compositeDirectPalette[0x40] { 0xFF656565, 0xFF00127D, 0xFF18008E, 0xFF360082, 0xFF56005D, 0xFF5A0018, 0xFF4F0500, 0xFF381900, 0xFF1D3100, 0xFF003D00, 0xFF004100, 0xFF003B17, 0xFF002E55, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFAFAFAF, 0xFF194EC8, 0xFF472FE3, 0xFF6B1FD7, 0xFF931BAE, 0xFF9E1A5E, 0xFF993200, 0xFF7B4B00, 0xFF5B6700, 0xFF267A00, 0xFF008200, 0xFF007A3E, 0xFF006E8A, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFF64A9FF, 0xFF8E89FF, 0xFFB676FF, 0xFFE06FFF, 0xFFEF6CC4, 0xFFF0806A, 0xFFD8982C, 0xFFB9B40A, 0xFF83CB0C, 0xFF5BD63F, 0xFF4AD17E, 0xFF4DC7CB, 0xFF4C4C4C, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFFC7E5FF, 0xFFD9D9FF, 0xFFE9D1FF, 0xFFF9CEFF, 0xFFFFCCF1, 0xFFFFD4CB, 0xFFF8DFB1, 0xFFEDEAA4, 0xFFD6F4A4, 0xFFC5F8B8, 0xFFBEF6D3, 0xFFBFF1F1, 0xFFB9B9B9, 0xFF000000, 0xFF000000 };
uint32_t nesClassicPalette[0x40] { 0xFF60615F, 0xFF000083, 0xFF1D0195, 0xFF340875, 0xFF51055E, 0xFF56000F, 0xFF4C0700, 0xFF372308, 0xFF203A0B, 0xFF0F4B0E, 0xFF194C16, 0xFF02421E, 0xFF023154, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFA9AAA8, 0xFF104BBF, 0xFF4712D8, 0xFF6300CA, 0xFF8800A9, 0xFF930B46, 0xFF8A2D04, 0xFF6F5206, 0xFF5C7114, 0xFF1B8D12, 0xFF199509, 0xFF178448, 0xFF206B8E, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFFBFBFB, 0xFF6699F8, 0xFF8974F9, 0xFFAB58F8, 0xFFD557EF, 0xFFDE5FA9, 0xFFDC7F59, 0xFFC7A224, 0xFFA7BE03, 0xFF75D703, 0xFF60E34F, 0xFF3CD68D, 0xFF56C9CC, 0xFF414240, 0xFF000000, 0xFF000000, 0xFFFBFBFB, 0xFFBED4FA, 0xFFC9C7F9, 0xFFD7BEFA, 0xFFE8B8F9, 0xFFF5BAE5, 0xFFF3CAC2, 0xFFDFCDA7, 0xFFD9E09C, 0xFFC9EB9E, 0xFFC0EDB8, 0xFFB5F4C7, 0xFFB9EAE9, 0xFFABABAB, 0xFF000000, 0xFF000000 };
uint32_t originalHardwarePalette[0x40] { 0xFF6A6D6A, 0xFF00127D, 0xFF1E008A, 0xFF3B007D, 0xFF56005D, 0xFF5A0018, 0xFF4F0D00, 0xFF381E00, 0xFF203100, 0xFF003D00, 0xFF004000, 0xFF003B1E, 0xFF002E55, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFB9BCB9, 0xFF194EC8, 0xFF472FE3, 0xFF751FD7, 0xFF931EAD, 0xFF9E245E, 0xFF963800, 0xFF7B5000, 0xFF5B6700, 0xFF267A00, 0xFF007F00, 0xFF007842, 0xFF006E8A, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFF69AEFF, 0xFF9798FF, 0xFFB687FF, 0xFFE278FF, 0xFFF279C7, 0xFFF58F6F, 0xFFDDA932, 0xFFBCB70D, 0xFF88D015, 0xFF60DB49, 0xFF4FD687, 0xFF50CACE, 0xFF515451, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFFCCEAFF, 0xFFDEE2FF, 0xFFEEDAFF, 0xFFFAD7FD, 0xFFFDD7F6, 0xFFFDDCD0, 0xFFFAE8B6, 0xFFF2F1A9, 0xFFDBFBA9, 0xFFCAFFBD, 0xFFC3FBD8, 0xFFC4F6F6, 0xFFBEC1BE, 0xFF000000, 0xFF000000 };
uint32_t pvmStylePalette[0x40] { 0xFF696964, 0xFF001774, 0xFF28007D, 0xFF3E006D, 0xFF560057, 0xFF5E0013, 0xFF531A00, 0xFF3B2400, 0xFF2A3000, 0xFF143A00, 0xFF003F00, 0xFF003B1E, 0xFF003050, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFB9B9B4, 0xFF1453B9, 0xFF4D2CDA, 0xFF7A1EC8, 0xFF98189C, 0xFF9D2344, 0xFFA03E00, 0xFF8D5500, 0xFF656D00, 0xFF2C7900, 0xFF008100, 0xFF007D42, 0xFF00788A, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFF69A8FF, 0xFF9A96FF, 0xFFC28AFA, 0xFFEA7DFA, 0xFFF387B4, 0xFFF1986C, 0xFFE6B327, 0xFFD7C805, 0xFF90DF07, 0xFF64E53C, 0xFF45E27D, 0xFF48D5D9, 0xFF4B4B46, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFFD2EAFF, 0xFFE2E2FF, 0xFFF2D8FF, 0xFFF8D2FF, 0xFFF8D9EA, 0xFFFADEB9, 0xFFF9E89B, 0xFFF3F28C, 0xFFD3FA91, 0xFFB8FCA8, 0xFFAEFACA, 0xFFCAF3F3, 0xFFBEBEB9, 0xFF000000, 0xFF000000 };
uint32_t sonyCxa2025AsPalette[0x40] { 0xFF585858, 0xFF00238C, 0xFF00139B, 0xFF2D0585, 0xFF5D0052, 0xFF7A0017, 0xFF7A0800, 0xFF5F1800, 0xFF352A00, 0xFF093900, 0xFF003F00, 0xFF003C22, 0xFF00325D, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFA1A1A1, 0xFF0053EE, 0xFF153CFE, 0xFF6028E4, 0xFFA91D98, 0xFFD41E41, 0xFFD22C00, 0xFFAA4400, 0xFF6C5E00, 0xFF2D7300, 0xFF007D06, 0xFF007852, 0xFF0069A9, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFF1FA5FE, 0xFF5E89FE, 0xFFB572FE, 0xFFFE65F6, 0xFFFE6790, 0xFFFE773C, 0xFFFE9308, 0xFFC4B200, 0xFF79CA10, 0xFF3AD54A, 0xFF11D1A4, 0xFF06BFFE, 0xFF424242, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFFA0D9FE, 0xFFBDCCFE, 0xFFE1C2FE, 0xFFFEBCFB, 0xFFFEBDD0, 0xFFFEC5A9, 0xFFFED18E, 0xFFE9DE86, 0xFFC7E992, 0xFFA8EEB0, 0xFF95ECD9, 0xFF91E4FE, 0xFFACACAC, 0xFF000000, 0xFF000000 };
uint32_t wavebeamPalette[0x40] { 0xFF6B6B6B, 0xFF001B88, 0xFF21009A, 0xFF40008C, 0xFF600067, 0xFF64001E, 0xFF590800, 0xFF481600, 0xFF283600, 0xFF004500, 0xFF004908, 0xFF00421D, 0xFF003659, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFB4B4B4, 0xFF1555D3, 0xFF4337EF, 0xFF7425DF, 0xFF9C19B9, 0xFFAC0F64, 0xFFAA2C00, 0xFF8A4B00, 0xFF666B00, 0xFF218300, 0xFF008A00, 0xFF008144, 0xFF007691, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFF63B2FF, 0xFF7C9CFF, 0xFFC07DFE, 0xFFE977FF, 0xFFF572CD, 0xFFF4886B, 0xFFDDA029, 0xFFBDBD0A, 0xFF89D20E, 0xFF5CDE3E, 0xFF4BD886, 0xFF4DCFD2, 0xFF525252, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFFBCDFFF, 0xFFD2D2FF, 0xFFE1C8FF, 0xFFEFC7FF, 0xFFFFC3E1, 0xFFFFCAC6, 0xFFF2DAAD, 0xFFEBE3A0, 0xFFD2EDA2, 0xFFBCF4B4, 0xFFB5F1CE, 0xFFB6ECF1, 0xFFBFBFBF, 0xFF000000, 0xFF000000 };

extern "C" {
	void logMessage(retro_log_level level, const char* message)
	{
		if(logCallback) {
			logCallback(level, message);
		}
	}

	RETRO_API unsigned retro_api_version()
	{
		return RETRO_API_VERSION;
	}

	RETRO_API void retro_init()
	{
		struct retro_log_callback log;
		if(env_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log)) {
			logCallback = log.log;
			if(logCallback) {
				logMessage(RETRO_LOG_INFO, "Mesen2 libretro core initialized\n");
			}
		} else {
			logCallback = nullptr;
		}

		fprintf(stderr, "[libretro] retro_init: logCallback=%p\n", (void*)logCallback);
		fflush(stderr);

		// Create the emulator and initialize its subsystems
		_emu.reset(new Emulator());
		_emu->Initialize(); // sets up settings, video/audio subsystems, etc.

		// Provide the global KeyManager with the emulator settings so calls like
		// KeyManager::SetForceFeedback can safely read configuration values.
		KeyManager::SetSettings(_emu->GetSettings());

		// Grab the IConsole instance and dynamic_cast to NesConsole when needed
		auto consoleIf = _emu->GetConsole(); // shared_ptr<IConsole>
		_console = std::dynamic_pointer_cast<NesConsole>(consoleIf);

	// Key manager accepts the IConsole shared_ptr returned by the emulator
	// and also needs a pointer to the Emulator for mouse positioning.
	_keyManager = std::make_shared<LibretroKeyManager>(consoleIf, _emu.get());

	// Forward any previously saved input callbacks into the key manager and register it
	if(_savedGetInputState) _keyManager->SetGetInputState(_savedGetInputState);
	if(_savedPollInput) _keyManager->SetPollInput(_savedPollInput);
	// KeyManager now expects a shared_ptr<IKeyManager> for thread-safe registration
	KeyManager::RegisterKeyManager(std::shared_ptr<IKeyManager>(_keyManager));
		_message_manager.reset(new LibretroMessageManager(logCallback, env_cb));

		std::stringstream databaseData;
		databaseData.write((const char*)MesenDatabase, sizeof(MesenDatabase));
		GameDatabase::LoadGameDb(databaseData);

		// Map sample rate into the new EmuSettings API
		AudioConfig ac = _emu->GetSettings()->GetAudioConfig();
		ac.SampleRate = _audioSampleRate;
		_emu->GetSettings()->SetAudioConfig(ac);

		// Create libretro audio device so we can register it later once a ROM is loaded
		_audioDevice.reset(new LibretroAudioDevice());

		if(_emu->GetSoundMixer()) {
		    _emu->GetSoundMixer()->RegisterAudioDevice(_audioDevice.get());
		}

		// NOTE: many NES-specific flags moved into NesConfig inside EmuSettings.
		// Example: to set FDS auto-load you'd edit NesConfig and call SetNesConfig.
		// _emu->GetSettings()->GetNesConfig().FdsAutoLoadDisk = true; // then SetNesConfig(...)

		if (env_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
			_keyManager->SetSupportsInputBitmasks(true);
	}

	RETRO_API void retro_deinit()
	{
		if(_keyManager) _keyManager->SetSupportsInputBitmasks(false);
	// Unregister the key manager from the global KeyManager to avoid a
	// dangling pointer during emulator shutdown. KeyManager::RegisterKeyManager
	// accepts an empty shared_ptr to clear the registered backend.
	KeyManager::RegisterKeyManager(std::shared_ptr<IKeyManager>());
		_keyManager.reset();
		_message_manager.reset();

		// Properly shut down the emulator instance
		if(_emu) {
			// Unregister libretro audio device before releasing the emulator
			if(_audioDevice) {
				if(_emu->GetSoundMixer()) _emu->GetSoundMixer()->RegisterAudioDevice(nullptr);
				_audioDevice.reset();
			}

			// Destroy console(s) first so their destructors run while the emulator
			// (and subsystems like SoundMixer) are still available.
			_console.reset();

			// Make sure KeyManager is unregistered before calling Release()
			// (do nothing here because we already unregistered above)
			_emu->Release();
			_emu.reset();
		}

		// Now that the emulator instance has been released, clear the settings
		// pointer stored in the global KeyManager to avoid dangling references.
		KeyManager::SetSettings(nullptr);
	}

	RETRO_API void retro_set_environment(retro_environment_t env)
	{
		env_cb = env;

		// Core Options v2: Categories
		static const retro_core_option_v2_category option_cats[] = {
			{ "system", "System", "System settings (region, RAM, overclock)" },
			{ "video", "Video", "Video settings (palette, filters, overscan)" },
			{ "audio", "Audio", "Audio settings (filters, channels, sample rate)" },
			{ "input", "Input", "Input controller settings" },
			{ "enhancements", "Enhancements", "Enhancement options (HD packs, sprite limit)" },
			{ NULL, NULL, NULL }
		};

		// Core Options v2: Definitions
		static const retro_core_option_v2_definition option_defs[] = {
			// System category
			{ MesenRegion, "System - Region", "Region", "Select NES region", NULL, "system",
				{{ "Auto", "Auto" }, { "NTSC", "NTSC" }, { "PAL", "PAL" }, { "Dendy", "Dendy" }, { NULL, NULL }},
				"Auto" },
			{ MesenRamState, "System - RAM Power-On State", "RAM Power-On State", "Default power-on state for RAM", NULL, "system",
				{{ "All 0s (Default)", "All 0s" }, { "All 1s", "All 1s" }, { "Random Values", "Random" }, { NULL, NULL }},
				"All 0s (Default)" },
			{ MesenOverclock, "System - Overclock", "Overclock", "Overclock the NES CPU", NULL, "system",
				{{ "None", "None" }, { "Low", "Low" }, { "Medium", "Medium" }, { "High", "High" }, { "Very High", "Very High" }, { NULL, NULL }},
				"None" },
			{ MesenOverclockType, "System - Overclock Type", "Overclock Type", "When to apply overclock", NULL, "system",
				{{ "Before NMI (Recommended)", "Before NMI" }, { "After NMI", "After NMI" }, { NULL, NULL }},
				"Before NMI (Recommended)" },
			{ MesenFdsAutoSelectDisk, "System - FDS Auto Insert Disk", "FDS Auto Insert", "Automatically insert disks on FDS games", NULL, "system",
				{{ "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL }},
				"disabled" },
			{ MesenFdsFastForwardLoad, "System - FDS Fast Forward On Load", "FDS Fast Forward", "Fast forward while FDS is loading", NULL, "system",
				{{ "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL }},
				"disabled" },
			{ MesenAllowInvalidInput, "System - Allow Invalid Input", "Allow Invalid Input", "Allow invalid input combinations", NULL, "system",
				{{ "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL }},
				"disabled" },
			{ MesenRandomizeMapperPowerOnState, "System - Randomize Mapper Power-On State", "Randomize Mapper Power-On", "Randomize mapper power-on state (for testing)", NULL, "system",
				{{ "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL }},
				"disabled" },
			{ MesenRandomizeCpuPpuAlignment, "System - Randomize CPU/PPU Alignment", "Randomize CPU/PPU Alignment", "Randomize CPU/PPU alignment (for testing)", NULL, "system",
				{{ "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL }},
				"disabled" },

			// Video category
			{ MesenPalette, "Video - Palette", "Palette", "Select color palette", NULL, "video",
			// TODO:FIXME RAW palette doesn't work
				{{ "Default", "Default" }, { "Composite Direct (by FirebrandX)", }, { "Nes Classic", "NES Classic" }, { "Nestopia (RGB)", "Nestopia RGB" }, { "Original Hardware (by FirebrandX)", "Original Hardware" }, { "PVM Style (by FirebrandX)", "PVM Style" }, { "Sony CXA2025AS", "Sony CXA2025AS" }, { "Unsaturated v6 (by FirebrandX)", "Unsaturated v6" }, { "YUV v3 (by FirebrandX)", "YUV v3" }, { "Wavebeam (by nakedarthur)", "Wavebeam" }, { "Custom", "Custom" }, /*{ "Raw", "Raw" },*/ { NULL, NULL }},
				"Default" },	
			// TODO:FIXME the commented NTSC filters don't work	
			{ MesenNtscFilter, "Video - NTSC Filter", "NTSC Filter", "NTSC video filter", NULL, "video",
				{{ "Disabled", "Disabled" }, { "Composite (Blargg)", "Composite (Blargg)" }, /*{ "S-Video (Blargg)", "S-Video (Blargg)" }, { "RGB (Blargg)", "RGB (Blargg)" }, { "Monochrome (Blargg)", "Monochrome (Blargg)" }, { "Bisqwit 2x", "Bisqwit 2x" }, { "Bisqwit 4x", "Bisqwit 4x" }, { "Bisqwit 8x", "Bisqwit 8x" },*/ { NULL, NULL }},
				"Disabled" },
		   // TODO:FIXME overscan cropping and AR options don't work
/*			{ MesenOverscanLeft, "Video - Overscan Left", "Overscan Left", "Left overscan", NULL, "video",
				{{ "None", "None" }, { "4px", "4px" }, { "8px", "8px" }, { "12px", "12px" }, { "16px", "16px" }, { NULL, NULL }},
				"None" },
			{ MesenOverscanRight, "Video - Overscan Right", "Overscan Right", "Right overscan", NULL, "video",
				{{ "None", "None" }, { "4px", "4px" }, { "8px", "8px" }, { "12px", "12px" }, { "16px", "16px" }, { NULL, NULL }},
				"None" },
			{ MesenOverscanTop, "Video - Overscan Top", "Overscan Top", "Top overscan", NULL, "video",
				{{ "None", "None" }, { "4px", "4px" }, { "8px", "8px" }, { "12px", "12px" }, { "16px", "16px" }, { NULL, NULL }},
				"None" },
			{ MesenOverscanBottom, "Video - Overscan Bottom", "Overscan Bottom", "Bottom overscan", NULL, "video",
				{{ "None", "None" }, { "4px", "4px" }, { "8px", "8px" }, { "12px", "12px" }, { "16px", "16px" }, { NULL, NULL }},
				"None" },
			{ MesenAspectRatio, "Video - Aspect Ratio", "Aspect Ratio", "Display aspect ratio", NULL, "video",
				{{ "Auto", "Auto" }, { "No Stretching", "No Stretching" }, { "NTSC", "NTSC" }, { "PAL", "PAL" }, { "4:3", "4:3" }, { "4:3 (Preserved)", "4:3 (Preserved)" }, { "16:9", "16:9" }, { "16:9 (Preserved)", "16:9 (Preserved)" }, { NULL, NULL }},
				"Auto" },
			{ MesenScreenRotation, "Video - Screen Rotation", "Screen Rotation", "Rotate screen display", NULL, "video",
				{{ "None", "None" }, { "90 degrees", "90 degrees" }, { "180 degrees", "180 degrees" }, { "270 degrees", "270 degrees" }, { NULL, NULL }},
				"None" },
			*/
			{ MesenEnablePalBorders, "Video - Enable PAL Borders", "PAL Borders", "Show borders in PAL mode", NULL, "video",
				{{ "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL }},
				"disabled" },

			// Audio category
			{ MesenFakeStereo, "Audio - Fake Stereo", "Fake Stereo", "Enable fake stereo effect", NULL, "audio",
				{{ "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL }},
				"disabled" },
			{ MesenMuteTriangleUltrasonic, "Audio - Reduce Triangle Popping", "Reduce Triangle Popping", "Mute Triangle channel ultrasonic frequencies", NULL, "audio",
				{{ "enabled", "On" }, { "disabled", "Off" }, { NULL, NULL }},
				"enabled" },
			{ MesenReduceDmcPopping, "Audio - Reduce DMC Popping", "Reduce DMC Popping", "Reduce popping on DMC channel", NULL, "audio",
				{{ "enabled", "On" }, { "disabled", "Off" }, { NULL, NULL }},
				"enabled" },
			{ MesenSwapDutyCycle, "Audio - Swap Duty Cycles", "Swap Duty Cycles", "Swap Square channel duty cycles", NULL, "audio",
				{{ "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL }},
				"disabled" },
			{ MesenDisableNoiseModeFlag, "Audio - Disable Noise Mode Flag", "Disable Noise Mode", "Disable Noise channel mode flag", NULL, "audio",
				{{ "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL }},
				"disabled" },
			{ MesenAudioSampleRate, "Audio - Sample Rate", "Sample Rate", "Audio output sample rate", NULL, "audio",
				{{ "48000", "48000 Hz" }, { "96000", "96000 Hz" }, { "11025", "11025 Hz" }, { "22050", "22050 Hz" }, { "44100", "44100 Hz" }, { NULL, NULL }},
				"44100" },

			// Input category
			{ MesenControllerTurboSpeed, "Input - Controller Turbo Speed", "Turbo Speed", "Turbo button speed", NULL, "input",
				{{ "Fast", "Fast" }, { "Very Fast", "Very Fast" }, { "Disabled", "Disabled" }, { "Slow", "Slow" }, { "Normal", "Normal" }, { NULL, NULL }},
				"Normal" },
			/*
			{ MesenShiftButtonsClockwise, "Input - Shift Buttons Clockwise", "Shift Buttons", "Shift A/B/X/Y buttons clockwise", NULL, "input",
				{{ "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL }},
				"disabled" },
			*/

			// Enhancements category (additional options)
			{ MesenSpritesEnabled, "Enhancements - Sprites Enabled", "Sprites Enabled", "Enable sprite rendering", NULL, "enhancements",
				{{ "enabled", "On" }, { "disabled", "Off" }, { NULL, NULL }},
				"enabled" },
			{ MesenBackgroundEnabled, "Enhancements - Background Enabled", "Background Enabled", "Enable background rendering", NULL, "enhancements",
				{{ "enabled", "On" }, { "disabled", "Off" }, { NULL, NULL }},
				"enabled" },
			{ MesenDisableGameGenieBusConflicts, "Enhancements - Disable Game Genie Bus Conflicts", "Game Genie Bus Conflicts", "Disable Game Genie bus conflicts", NULL, "enhancements",
				{{ "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL }},
				"disabled" },

			/*
			// Enhancements category
			{ MesenHdPacks, "Enhancements - HD Packs", "HD Packs", "Enable HD graphics packs", NULL, "enhancements",
				{{ "enabled", "On" }, { "disabled", "Off" }, { NULL, NULL }},
				"enabled" },
			*/
			{ MesenSpriteLimit, "Enhancements - Sprite Limit", "Sprite Limit", "8-sprite scanline limit", NULL, "enhancements",
				{{ "normal", "Normal" }, { "adaptive", "Adaptive" }, { "off", "Off" }, { NULL, NULL }},
				"normal" },

			{ NULL, NULL, NULL, NULL, NULL, NULL, {{ NULL, NULL }}, NULL }
		};

		static retro_core_options_v2 core_opt_info = { 
			(retro_core_option_v2_category*)option_cats,
			(retro_core_option_v2_definition*)option_defs
		};

		env_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &core_opt_info);

		static constexpr struct retro_controller_description pads1[] = {
			{ "Auto", DEVICE_AUTO },
			{ "Standard Controller", DEVICE_GAMEPAD },
			{ "Zapper", DEVICE_ZAPPER },
			{ "Power Pad", DEVICE_POWERPAD },
			{ "Arkanoid", DEVICE_ARKANOID },
			{ "SNES Controller", DEVICE_SNESGAMEPAD },
			{ "SNES Mouse", DEVICE_SNESMOUSE },
			{ "Gameboy Controller", DEVICE_GAMEBOYGAMEPAD },
			{ "Virtual Boy Controller" ,DEVICE_VBGAMEPAD },
			{ NULL, 0 },
		};

		static constexpr struct retro_controller_description pads2[] = {
			{ "Auto", DEVICE_AUTO },
			{ "Standard Controller", DEVICE_GAMEPAD },
			{ "Zapper", DEVICE_ZAPPER },
			{ "Power Pad", DEVICE_POWERPAD },
			{ "Arkanoid", DEVICE_ARKANOID },
			{ "SNES Controller", DEVICE_SNESGAMEPAD },
			{ "SNES Mouse", DEVICE_SNESMOUSE },
			{ "Virtual Boy Controller", DEVICE_VBGAMEPAD },
			{ NULL, 0 },
		};

		static constexpr struct retro_controller_description pads3[] = {
			{ "Auto", DEVICE_AUTO },
			{ "Standard Controller", DEVICE_GAMEPAD },
			{ NULL, 0 },
		};

		static constexpr struct retro_controller_description pads4[] = {
			{ "Auto", DEVICE_AUTO },
			{ "Standard Controller", DEVICE_GAMEPAD },
			{ NULL, 0 },
		};
		
		static constexpr struct retro_controller_description pads5[] = {
			{ "Auto",     RETRO_DEVICE_JOYPAD },
			{ "Arkanoid", DEVICE_ARKANOID },
			{ "Ascii Turbo File", DEVICE_ASCIITURBOFILE },
			{ "Bandai Hypershot", DEVICE_BANDAIHYPERSHOT },
			{ "Battle Box", DEVICE_BATTLEBOX },
			{ "Exciting Boxing", DEVICE_EXCITINGBOXING },
			{ "Family Trainer", DEVICE_FAMILYTRAINER },
			{ "Four Player Adapter", DEVICE_FOURPLAYERADAPTER },
			{ "Hori Track", DEVICE_HORITRACK },
			{ "Konami Hypershot", DEVICE_KONAMIHYPERSHOT },
			{ "Pachinko", DEVICE_PACHINKO },
			{ "Partytap", DEVICE_PARTYTAP },
			{ "Oeka Kids Tablet", DEVICE_OEKAKIDS },			
			{ NULL, 0 },
		};
		
		static constexpr struct retro_controller_info ports[] = {
			{ pads1, 7 },
			{ pads2, 7 },
			{ pads3, 2 },
			{ pads4, 2 },
			{ pads5, 13 },
			{ 0 },
		};

		static const struct retro_system_content_info_override content_overrides[] = {
			{
				"nes|fds|unf|unif|sfc|smc|gb|gbc|gba|pce|sgx|sms|gg|ws|wsc", /* extensions */
				false,              /* need_fullpath */
				false               /* persistent_data */
			},
			{ NULL, false, false }
		};

		env_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);
		env_cb(RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE, (void*)content_overrides);

		static const struct retro_subsystem_rom_info sgbSubsystemRoms[] = {
			{ "Game Boy ROM", "gb|gbc|gbx", false, false, true },
			{ "Super Game Boy ROM", "sfc|smc", false, false, true },
			{ NULL, NULL, false, false, false }
		};

		static const struct retro_subsystem_info subsystem_infos[] = {
			{ "Super Game Boy", "sgb", sgbSubsystemRoms, 2, 2 },
			{ NULL, NULL, NULL, 0, 0 }
		};

		env_cb(RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO, (void*)subsystem_infos);

		//Ask the frontend for its VFS interface - required to load content the
		//core cannot open itself (Android SAF content:// URIs in particular).
		//v3 is requested so the directory calls (stat/mkdir/opendir) are usable too.
		static struct retro_vfs_interface_info vfs_iface_info;
		vfs_iface_info.required_interface_version = 3;
		vfs_iface_info.iface = nullptr;
		if(env_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_iface_info) && vfs_iface_info.iface) {
			VfsIo::SetInterface(vfs_iface_info.iface, vfs_iface_info.required_interface_version);
		} else {
			//Fall back to v2 (file I/O only, no directory calls)
			vfs_iface_info.required_interface_version = 2;
			vfs_iface_info.iface = nullptr;
			if(env_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_iface_info) && vfs_iface_info.iface) {
				VfsIo::SetInterface(vfs_iface_info.iface, vfs_iface_info.required_interface_version);
			}
		}
	}

	RETRO_API void retro_set_video_refresh(retro_video_refresh_t sendFrame)
	{
		// VideoRenderer no longer exposes SetVideoCallback — keep the libretro callback here.
		_videoRefresh = sendFrame;
	}

	RETRO_API void retro_set_audio_sample(retro_audio_sample_t sendAudioSample)
	{
	}

	RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t audioSampleBatch)
	{
		// Mixer API changed; keep the libretro callback for later use by audio path
		_audioSampleBatch = audioSampleBatch;
	}

	RETRO_API void retro_set_input_poll(retro_input_poll_t pollInput)
	{	
		// store the callback for later use and forward to key manager if it's active
		_savedPollInput = pollInput;
		if(_keyManager) _keyManager->SetPollInput(pollInput);

		// Probe input callbacks immediately from the thread that set them to
		// help debug frontend timing/order issues. This is noisy by default so
		// gate it behind an environment variable (MESEN_LIBRETRO_VERBOSE_INPUT).
		if((pollInput || _savedGetInputState) && getenv("MESEN_LIBRETRO_VERBOSE_INPUT")) {
			extern void libretro_probe_inputs(const char*);
			libretro_probe_inputs("setter_poll");
		}
	}

	RETRO_API void retro_set_input_state(retro_input_state_t getInputState)
	{
		_savedGetInputState = getInputState;
		if(_keyManager) _keyManager->SetGetInputState(getInputState);

		if((getInputState || _savedPollInput) && getenv("MESEN_LIBRETRO_VERBOSE_INPUT")) {
			extern void libretro_probe_inputs(const char*);
			libretro_probe_inputs("setter_getstate");
		}
	}

// Shared diagnostic probe called from multiple places to exercise the saved
// input callbacks and print their results. The argument is a short tag
// describing the caller (for log clarity).
void libretro_probe_inputs(const char* tag)
{
	if(!env_cb) return;
	if(_savedPollInput) {
		try { _savedPollInput(); } catch(...) { }
	}
	// Query bitmask capability but avoid calling the frontend get_state callback here.
	// Some libretro frontends crash if get_state is invoked outside their expected
	// input polling context (we observed a segmentation fault). To be safe, only
	// log availability and defer actual get_state calls to the normal per-frame
	// RefreshState path which runs on the emulator/main thread.
	bool bitmasks = false;
	if (env_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL)) bitmasks = true;
}

	RETRO_API void retro_reset()
	{
		// Reset signature changed — call parameterless Reset if available
		_console->Reset();
	}

	bool readVariable(const char* key, retro_variable &var)
	{
		var.key = key;
		var.value = nullptr;
		if(env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value != nullptr)
			return true;
		return false;
	}

	uint8_t readOverscanValue(const char* key)
	{
		retro_variable var = {};
		if(readVariable(key, var)) {
			string value = string(var.value);
			if(value == "4px") {
				return 4;
			} else if(value == "8px") {
				return 8;
			} else if(value == "12px") {
				return 12;
			} else if(value == "16px") {
				return 16;
			}
		}
		return 0;
	}

	void set_flag(const char* flagName, uint64_t flagValue)
	{
		struct retro_variable var = {};
		if(readVariable(flagName, var)) {
			string value = string(var.value);
			if(value == "disabled") {
				_emu->GetSettings()->ClearFlag((EmulationFlags)flagValue);
			} else {
				_emu->GetSettings()->SetFlag((EmulationFlags)flagValue);
			}
		}
	}

	void load_custom_palette()
	{
		//Setup default palette in case we can't load the custom one
		// Ensure video config is accessed so types are instantiated (no-op)
		_emu->GetSettings()->GetVideoConfig();
		// Store default palette locally; renderer wiring to use this will be added later.
		_userRgbPalette.assign(std::begin(defaultPalette), std::end(defaultPalette));

		//Try to load the custom palette from the MesenPalette.pal file
		string palettePath = FolderUtilities::CombinePath(FolderUtilities::GetHomeFolder(), "MesenPalette.pal");
		uint8_t fileData[512 * 3] = {};
		utf8::ifstream palette(palettePath, std::ios::binary);
		if(palette) {
			palette.seekg(0, std::ios::end);
			std::streamoff fileSize = palette.tellg();
			palette.seekg(0, std::ios::beg);
			if((fileSize == 64 * 3) || (fileSize == 512 * 3)) {
				palette.read((char*)fileData, fileSize);
				uint32_t customPalette[512];
				int paletteCount = (int)(fileSize / 3);
				for(int i = 0; i < paletteCount; i++) {
					customPalette[i] = 0xFF000000 | fileData[i * 3 + 2] | (fileData[i * 3 + 1] << 8) | (fileData[i * 3] << 16);
				}
				_userRgbPalette.assign(customPalette, customPalette + paletteCount);
			}
		}
	}

	void update_settings()
	{
		struct retro_variable var = { };
		NesConfig nesCfg = _emu->GetSettings()->GetNesConfig();
		VideoConfig videoCfg = _emu->GetSettings()->GetVideoConfig();
		AudioConfig audioCfg = _emu->GetSettings()->GetAudioConfig();

		// ===== SYSTEM SETTINGS =====
		
		// Region
		if(readVariable(MesenRegion, var)) {
			string value = string(var.value);
			if(value == "NTSC") {
				nesCfg.Region = ConsoleRegion::Ntsc;
			} else if(value == "PAL") {
				nesCfg.Region = ConsoleRegion::Pal;
			} else if(value == "Dendy") {
				nesCfg.Region = ConsoleRegion::Dendy;
			} else {
				nesCfg.Region = ConsoleRegion::Auto;
			}
		}

		// RAM Power-On State
		if(readVariable(MesenRamState, var)) {
			string value = string(var.value);
			if(value == "All 1s") {
				nesCfg.RamPowerOnState = RamState::AllOnes;
			} else if(value == "Random Values") {
				nesCfg.RamPowerOnState = RamState::Random;
			} else {
				nesCfg.RamPowerOnState = RamState::AllZeros;
			}
		}

		// Overclock
		int lineCountBefore = 0;
		int lineCountAfter = 0;
		bool beforeNmi = true;
		if(readVariable(MesenOverclockType, var)) {
			string value = string(var.value);
			beforeNmi = (value != "After NMI");
		}

		if(readVariable(MesenOverclock, var)) {
			string value = string(var.value);
			int lineCount = 0;
			if(value == "Low") {
				lineCount = 100;
			} else if(value == "Medium") {
				lineCount = 250;
			} else if(value == "High") {
				lineCount = 500;
			} else if(value == "Very High") {
				lineCount = 1000;
			}
			if(beforeNmi) {
				lineCountBefore = lineCount;
			} else {
				lineCountAfter = lineCount;
			}
		}
		nesCfg.PpuExtraScanlinesBeforeNmi = lineCountBefore;
		nesCfg.PpuExtraScanlinesAfterNmi = lineCountAfter;

		// FDS options
		if(readVariable(MesenFdsAutoSelectDisk, var)) {
			nesCfg.FdsAutoInsertDisk = (string(var.value) == "enabled");
		}
		if(readVariable(MesenFdsFastForwardLoad, var)) {
			nesCfg.FdsFastForwardOnLoad = (string(var.value) == "enabled");
		}

		// ===== VIDEO SETTINGS =====
		
		// Palette
		if(readVariable(MesenPalette, var)) {
			string value = string(var.value);
			if(value == "Default") {
				_userRgbPalette.assign(std::begin(defaultPalette), std::end(defaultPalette));
			} else if(value == "Composite Direct (by FirebrandX)") {
				_userRgbPalette.assign(std::begin(compositeDirectPalette), std::end(compositeDirectPalette));
			} else if(value == "Nes Classic") {
				_userRgbPalette.assign(std::begin(nesClassicPalette), std::end(nesClassicPalette));
			} else if(value == "Nestopia (RGB)") {
				_userRgbPalette.assign(std::begin(nestopiaRgbPalette), std::end(nestopiaRgbPalette));
			} else if(value == "Original Hardware (by FirebrandX)") {
				_userRgbPalette.assign(std::begin(originalHardwarePalette), std::end(originalHardwarePalette));
			} else if(value == "PVM Style (by FirebrandX)") {
				_userRgbPalette.assign(std::begin(pvmStylePalette), std::end(pvmStylePalette));
			} else if(value == "Sony CXA2025AS") {
				_userRgbPalette.assign(std::begin(sonyCxa2025AsPalette), std::end(sonyCxa2025AsPalette));
			} else if(value == "Unsaturated v6 (by FirebrandX)") {
				_userRgbPalette.assign(std::begin(unsaturatedPalette), std::end(unsaturatedPalette));
			} else if(value == "YUV v3 (by FirebrandX)") {
				_userRgbPalette.assign(std::begin(yuvPalette), std::end(yuvPalette));
			} else if(value == "Wavebeam (by nakedarthur)") {
				_userRgbPalette.assign(std::begin(wavebeamPalette), std::end(wavebeamPalette));
			} else if(value == "Custom") {
				load_custom_palette();
			} else if(value == "Raw") {
				_videoFilterRaw = true;
				// For raw mode, use the default palette
				_userRgbPalette.assign(std::begin(defaultPalette), std::end(defaultPalette));
			}
		}

		// Copy palette into NesConfig
		if(_userRgbPalette.size() >= 512) {
			for(size_t i = 0; i < 512; ++i) nesCfg.UserPalette[i] = (i < _userRgbPalette.size()) ? _userRgbPalette[i] : 0xFF000000;
			nesCfg.IsFullColorPalette = true;
		} else {
			for(size_t i = 0; i < 64; ++i) nesCfg.UserPalette[i] = (i < _userRgbPalette.size()) ? _userRgbPalette[i] : 0xFF000000;
			nesCfg.IsFullColorPalette = false;
		}

		
		// NTSC Filter - map string selection to VideoFilterType enum and apply settings
		if(readVariable(MesenNtscFilter, var)) {
			string filterValue = string(var.value);
			if(filterValue == "Disabled") {
				videoCfg.VideoFilter = VideoFilterType::None;
			} else if(filterValue == "Composite (Blargg)") {
				videoCfg.VideoFilter = VideoFilterType::NtscBlargg;
				videoCfg.NtscBlarggPreset_Value = NtscBlarggPreset::Composite;
			}/* else if(filterValue == "S-Video (Blargg)") {
				videoCfg.VideoFilter = VideoFilterType::NtscBlargg;
				videoCfg.NtscBlarggPreset_Value = NtscBlarggPreset::Svideo;
			} else if(filterValue == "RGB (Blargg)") {
				videoCfg.VideoFilter = VideoFilterType::NtscBlargg;
				videoCfg.NtscBlarggPreset_Value = NtscBlarggPreset::Rgb;
			} else if(filterValue == "Monochrome (Blargg)") {
				videoCfg.VideoFilter = VideoFilterType::NtscBlargg;
				videoCfg.NtscBlarggPreset_Value = NtscBlarggPreset::Monochrome;
			} else if(filterValue == "Bisqwit 2x") {
				videoCfg.VideoFilter = VideoFilterType::NtscBisqwit;
				videoCfg.NtscScale = NtscBisqwitFilterScale::_2x;
			} else if(filterValue == "Bisqwit 4x") {
				videoCfg.VideoFilter = VideoFilterType::NtscBisqwit;
				videoCfg.NtscScale = NtscBisqwitFilterScale::_4x;
			} else if(filterValue == "Bisqwit 8x") {
				videoCfg.VideoFilter = VideoFilterType::NtscBisqwit;
				videoCfg.NtscScale = NtscBisqwitFilterScale::_8x;
			}*/
		}
/*
		// Overscan
		nesCfg.NtscOverscan.Left = readOverscanValue(MesenOverscanLeft);
		nesCfg.NtscOverscan.Right = readOverscanValue(MesenOverscanRight);
		nesCfg.NtscOverscan.Top = readOverscanValue(MesenOverscanTop);
		nesCfg.NtscOverscan.Bottom = readOverscanValue(MesenOverscanBottom);
		// Use same overscan for PAL
		nesCfg.PalOverscan = nesCfg.NtscOverscan;

		// Aspect Ratio
		if(readVariable(MesenAspectRatio, var)) {
			_selectedAspectRatio = std::string(var.value ? var.value : "");
		}

		
		// Screen Rotation
		if(readVariable(MesenScreenRotation, var)) {
			string value = string(var.value);
			if(value == "90 degrees") {
				videoCfg.ScreenRotation = 90;
			} else if(value == "180 degrees") {
				videoCfg.ScreenRotation = 180;
			} else if(value == "270 degrees") {
				videoCfg.ScreenRotation = 270;
			} else {
				videoCfg.ScreenRotation = 0;
			}
		}
*/

		// PAL Borders
		if(readVariable(MesenEnablePalBorders, var)) {
			nesCfg.EnablePalBorders = (string(var.value) == "enabled");
		}

		// ===== AUDIO SETTINGS =====
		
		// Fake Stereo
		if(readVariable(MesenFakeStereo, var)) {
			nesCfg.StereoFilter = (string(var.value) == "enabled") ? StereoFilterType::Delay : StereoFilterType::None;
		}

		// Reduce Triangle Popping
		if(readVariable(MesenMuteTriangleUltrasonic, var)) {
			nesCfg.SilenceTriangleHighFreq = (string(var.value) == "enabled");
		}

		// Reduce DMC Popping
		if(readVariable(MesenReduceDmcPopping, var)) {
			nesCfg.ReduceDmcPopping = (string(var.value) == "enabled");
		}

		// Swap Duty Cycles
		if(readVariable(MesenSwapDutyCycle, var)) {
			nesCfg.SwapDutyCycles = (string(var.value) == "enabled");
		}

		// Disable Noise Mode Flag
		if(readVariable(MesenDisableNoiseModeFlag, var)) {
			nesCfg.DisableNoiseModeFlag = (string(var.value) == "enabled");
		}

		// Audio Sample Rate
		if(readVariable(MesenAudioSampleRate, var)) {
			int old_value = audioCfg.SampleRate;
			audioCfg.SampleRate = atoi(var.value);
			audioCfg.SampleRate = (audioCfg.SampleRate > 96000) ? 96000 : audioCfg.SampleRate;

			if(old_value != audioCfg.SampleRate) {
				// If core is running, notify frontend of geometry change
				if(_saveStateSize != -1) {
					struct retro_system_av_info system_av_info;
					retro_get_system_av_info(&system_av_info);
					env_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &system_av_info);
				}
			}
		}

		// ===== INPUT SETTINGS =====
		
		// Controller Turbo Speed
		int turboSpeed = 1; // default to Normal
		bool turboEnabled = true;
		if(readVariable(MesenControllerTurboSpeed, var)) {
			string value = string(var.value);
			if(value == "Slow") {
				turboSpeed = 0;
			} else if(value == "Normal") {
				turboSpeed = 1;
			} else if(value == "Fast") {
				turboSpeed = 2;
			} else if(value == "Very Fast") {
				turboSpeed = 3;
			} else if(value == "Disabled") {
				turboEnabled = false;
			}
		}

/*
		// Shift Buttons Clockwise
		_shiftButtonsClockwise = false;
		if(readVariable(MesenShiftButtonsClockwise, var)) {
			_shiftButtonsClockwise = (string(var.value) == "enabled");
		}
*/

		auto getKeyCode = [](int port, int retroKey) {
			return (port << 8) | (retroKey + 1);
		};

		auto getKeyBindings = [=](int port) {
			KeyMappingSet keyMappings;
			keyMappings.TurboSpeed = turboSpeed;
			// Default NES-style mapping with optional clockwise shift
			keyMappings.Mapping1.A = getKeyCode(port, _shiftButtonsClockwise ? RETRO_DEVICE_ID_JOYPAD_B : RETRO_DEVICE_ID_JOYPAD_A);
			keyMappings.Mapping1.B = getKeyCode(port, _shiftButtonsClockwise ? RETRO_DEVICE_ID_JOYPAD_Y : RETRO_DEVICE_ID_JOYPAD_B);
			if(turboEnabled) {
				keyMappings.Mapping1.TurboA = getKeyCode(port, _shiftButtonsClockwise ? RETRO_DEVICE_ID_JOYPAD_A : RETRO_DEVICE_ID_JOYPAD_X);
				keyMappings.Mapping1.TurboB = getKeyCode(port, _shiftButtonsClockwise ? RETRO_DEVICE_ID_JOYPAD_X : RETRO_DEVICE_ID_JOYPAD_Y);
			}
			keyMappings.Mapping1.Start = getKeyCode(port, RETRO_DEVICE_ID_JOYPAD_START);
			keyMappings.Mapping1.Select = getKeyCode(port, RETRO_DEVICE_ID_JOYPAD_SELECT);
			keyMappings.Mapping1.Up = getKeyCode(port, RETRO_DEVICE_ID_JOYPAD_UP);
			keyMappings.Mapping1.Down = getKeyCode(port, RETRO_DEVICE_ID_JOYPAD_DOWN);
			keyMappings.Mapping1.Left = getKeyCode(port, RETRO_DEVICE_ID_JOYPAD_LEFT);
			keyMappings.Mapping1.Right = getKeyCode(port, RETRO_DEVICE_ID_JOYPAD_RIGHT);
			return keyMappings;
		};

		nesCfg.Port1.Keys = getKeyBindings(0);
		nesCfg.Port2.Keys = getKeyBindings(1);
		nesCfg.Port1SubPorts[0].Keys = getKeyBindings(0);
		nesCfg.Port1SubPorts[1].Keys = getKeyBindings(1);
		nesCfg.Port1SubPorts[2].Keys = getKeyBindings(2);
		nesCfg.Port1SubPorts[3].Keys = getKeyBindings(3);
		nesCfg.ExpPort.Keys = getKeyBindings(4);

		// ===== ENHANCEMENTS =====
		
		/*
		// HD Packs
		_hdPacksEnabled = true;
		if(readVariable(MesenHdPacks, var)) {
			_hdPacksEnabled = (string(var.value) != "disabled");
		}
		nesCfg.EnableHdPacks = _hdPacksEnabled;
		*/
		// HD packs disabled - not working properly
		_hdPacksEnabled = false;
		nesCfg.EnableHdPacks = _hdPacksEnabled;

		// Sprite Limit (3-choice: normal, adaptive, off)
		nesCfg.RemoveSpriteLimit = false;
		nesCfg.AdaptiveSpriteLimit = false;
		if(readVariable(MesenSpriteLimit, var)) {
			string value = string(var.value);
			if(value == "adaptive") {
				nesCfg.AdaptiveSpriteLimit = true;
			} else if(value == "off") {
				nesCfg.RemoveSpriteLimit = true;
			}
		}

		// Sprites Enabled
		if(readVariable(MesenSpritesEnabled, var)) {
			nesCfg.SpritesEnabled = (string(var.value) == "enabled");
		}

		// Background Enabled
		if(readVariable(MesenBackgroundEnabled, var)) {
			nesCfg.BackgroundEnabled = (string(var.value) == "enabled");
		}

		// Disable Game Genie Bus Conflicts
		if(readVariable(MesenDisableGameGenieBusConflicts, var)) {
			nesCfg.DisableGameGenieBusConflicts = (string(var.value) == "enabled");
		}

		// ===== SYSTEM OPTIONS =====

		// Allow Invalid Input
		if(readVariable(MesenAllowInvalidInput, var)) {
			nesCfg.AllowInvalidInput = (string(var.value) == "enabled");
		}

		// Randomize Mapper Power-On State
		if(readVariable(MesenRandomizeMapperPowerOnState, var)) {
			nesCfg.RandomizeMapperPowerOnState = (string(var.value) == "enabled");
		}

		// Randomize CPU/PPU Alignment
		if(readVariable(MesenRandomizeCpuPpuAlignment, var)) {
			nesCfg.RandomizeCpuPpuAlignment = (string(var.value) == "enabled");
		}

		// ===== APPLY ALL SETTINGS =====
		
		_emu->GetSettings()->SetNesConfig(nesCfg);
		_emu->GetSettings()->SetVideoConfig(videoCfg);
		_emu->GetSettings()->SetAudioConfig(audioCfg);

		// Check if geometry-related settings changed
		bool videoFilterChanged = (_lastVideoConfig.VideoFilter != videoCfg.VideoFilter) ||
			(_lastVideoConfig.NtscScale != videoCfg.NtscScale) ||
			(_lastVideoConfig.ScreenRotation != videoCfg.ScreenRotation);
		bool overscanChanged = (nesCfg.NtscOverscan.Left != _lastNesConfig.NtscOverscan.Left) ||
			(nesCfg.NtscOverscan.Right != _lastNesConfig.NtscOverscan.Right) ||
			(nesCfg.NtscOverscan.Top != _lastNesConfig.NtscOverscan.Top) ||
			(nesCfg.NtscOverscan.Bottom != _lastNesConfig.NtscOverscan.Bottom) ||
			(nesCfg.PalOverscan.Left != _lastNesConfig.PalOverscan.Left) ||
			(nesCfg.PalOverscan.Right != _lastNesConfig.PalOverscan.Right) ||
			(nesCfg.PalOverscan.Top != _lastNesConfig.PalOverscan.Top) ||
			(nesCfg.PalOverscan.Bottom != _lastNesConfig.PalOverscan.Bottom);

		if(videoFilterChanged || overscanChanged) {
			_geometryDirty = true;
			if(_emu && _emu->GetVideoDecoder()) {
				_emu->GetVideoDecoder()->ForceFilterUpdate();
			}
		}

		// Save current settings for next comparison
		_lastVideoConfig = videoCfg;
		_lastNesConfig = nesCfg;
	}

	RETRO_API void retro_run()
	{
		// ForceMaxSpeed flag API changed / unavailable here — skip fast-forward behavior for now.
		if(false) {
#if 0
			//Skip frames to speed up emulation while still outputting at 50/60 fps (needed for FDS fast forward while loading)
			_console->GetVideoRenderer()->SetSkipMode(true);
			_console->GetSoundMixer()->SetSkipMode(true);
			for(int i = 0; i < 9; i++) {
				//Attempt to speed up to 1000% speed
				_console->RunSingleFrame();
			}
			_console->GetVideoRenderer()->SetSkipMode(false);
			_console->GetSoundMixer()->SetSkipMode(false);
#endif
		}

		bool updated = false;
		if(env_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
			update_settings();
			// Settings API moved; read flag from Emulator settings if available, otherwise skip.
			bool hdPacksEnabled = false;
			if(hdPacksEnabled != _hdPacksEnabled) {
				// Defer HD pack update to emulator API (call removed here) — just track the change
				_hdPacksEnabled = hdPacksEnabled;
			}
		}

		// Frame stepping: run a single emulated frame and submit video/audio to libretro callbacks.
		if(_emu && _emu->GetConsole()) {
			// Refresh input state before running the frame
			auto consoleIf = _emu->GetConsole();

			if(_keyManager) {
			    consoleIf->GetControlManager()->UpdateInputState();
			}

			consoleIf->RunFrame();
			// Allow emulator to run pre-frame hooks (UI, stats, etc.)
			_emu->OnBeforeSendFrame();

			// Get the PPU frame (core provides a 16-bit PPU buffer containing
			// palette indices/intensity bits). Use the emulator's video filter
			// to convert that buffer to a 32-bit ARGB frame so color/palette
			// handling (including emphasis, user palettes, and filters) is
			// consistent with the rest of the emulator.
			PpuFrameInfo frame = _emu->GetPpuFrame();
			// Check that we have a valid buffer with meaningful size (each system allocates differently)
			if(_videoRefresh && frame.FrameBuffer && frame.FrameBufferSize > 0) {
				// Create a temporary video filter (same pattern used by LuaApi)
				std::unique_ptr<BaseVideoFilter> filter(_emu->GetVideoFilter());
				FrameInfo baseSize = { frame.Width, frame.Height };
				filter->SetBaseFrameInfo(baseSize);
				FrameInfo outInfo = filter->SendFrame((uint16_t*)frame.FrameBuffer, _emu->GetFrameCount(), _emu->GetFrameCount() & 0x01, nullptr, false);

				uint32_t* src = filter->GetOutputBuffer();
				// Ensure we have valid output before displaying
				if(src) {
					size_t pixels = (size_t)outInfo.Width * (size_t)outInfo.Height;

					static std::vector<uint32_t> rotBuffer;

					if(_screenRotation == 0) {
						_videoRefresh(src, outInfo.Width, outInfo.Height, outInfo.Width * 4);
					} else {
						int outW = (_screenRotation == 180) ? outInfo.Width : outInfo.Height;
						int outH = (_screenRotation == 180) ? outInfo.Height : outInfo.Width;
						rotBuffer.resize((size_t)outW * (size_t)outH);

						if(_screenRotation == 180) {
							for(size_t i = 0; i < pixels; ++i) rotBuffer[pixels - 1 - i] = src[i];
							_videoRefresh(rotBuffer.data(), outInfo.Width, outInfo.Height, outInfo.Width * 4);
						} else if(_screenRotation == 90) {
							for(int y = 0; y < outInfo.Height; ++y) {
								for(int x = 0; x < outInfo.Width; ++x) {
									size_t srcIdx = (size_t)y * outInfo.Width + x;
									int dstX = outInfo.Height - 1 - y;
									int dstY = x;
									size_t dstIdx = (size_t)dstY * outW + (size_t)dstX;
									rotBuffer[dstIdx] = src[srcIdx];
								}
							}
							_videoRefresh(rotBuffer.data(), outW, outH, outW * 4);
						} else if(_screenRotation == 270) {
							for(int y = 0; y < outInfo.Height; ++y) {
								for(int x = 0; x < outInfo.Width; ++x) {
									size_t srcIdx = (size_t)y * outInfo.Width + x;
									int dstX = y;
									int dstY = outInfo.Width - 1 - x;
									size_t dstIdx = (size_t)dstY * outW + (size_t)dstX;
									rotBuffer[dstIdx] = src[srcIdx];
								}
							}
							_videoRefresh(rotBuffer.data(), outW, outH, outW * 4);
						} else {
							// Unknown rotation: fallback to non-rotated output
							_videoRefresh(src, outInfo.Width, outInfo.Height, outInfo.Width * 4);
						}
					}
				}
			}
		}

		if(updated) {
			// Only update geometry if something changed or if flag is set
			if(_geometryDirty) {
				retro_system_av_info avInfo = {};
				retro_get_system_av_info(&avInfo);
				uint32_t newWidth = avInfo.geometry.base_width;
				uint32_t newHeight = avInfo.geometry.base_height;
				
				// Only call SET_GEOMETRY if dimensions actually changed
				if(newWidth != _lastReportedWidth || newHeight != _lastReportedHeight) {
					_lastReportedWidth = newWidth;
					_lastReportedHeight = newHeight;
					env_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &avInfo);
				}
				_geometryDirty = false;
			}
		}

		// Audio upload handled by emulator/sound subsystem; no-op here for now.
	}

	RETRO_API size_t retro_serialize_size()
	{
		return _saveStateSize;
	}

	RETRO_API bool retro_serialize(void *data, size_t size)
	{
		if(!_emu) return false;
		try {
			std::stringstream ss;
			_emu->Serialize(ss, true, 1);
			std::string out = ss.str();
			if(out.size() > size) return false;
			memcpy(data, out.data(), out.size());
			return true;
		} catch(...) {
			return false;
		}
	}

	RETRO_API bool retro_unserialize(const void *data, size_t size)
	{
		if(!_emu) return false;
		try {
			std::string in((const char*)data, size);
			std::stringstream ss(in);
			auto res = _emu->Deserialize(ss, SaveStateManager::FileFormatVersion, true);
			return (res == DeserializeResult::Success);
		} catch(...) {
			return false;
		}
	}

	RETRO_API void retro_cheat_reset()
	{
		if(_emu) {
			// Cheat manager API moved; no-op for now.
		}
	}

	RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *codeStr)
	{
		static const string validGgLetters = "APZLGITYEOXUKSVN";
		static const string validParLetters = "0123456789ABCDEF";
		int chl = 0;

		string code = codeStr;
		std::transform(code.begin(), code.end(), code.begin(), ::toupper);

		if(code[4] == ':') {
			for(;;) {
				string address = code.substr((0 + chl), 4);
				string value = code.substr((5 + chl), 2);
				// Cheat API moved — ignore for now
				if(code[(7 + chl)] != '+') {
					return;
				}
				chl = (chl + 8);
			}
		}

		else if(code[4] == '?' && code[7] == ':') {
			for(;;) {
				string address = code.substr((0 + chl), 4);
				string comparison = code.substr((5 + chl), 2);
				string value = code.substr((8 + chl), 2);
            // Cheat API moved; ignore custom cheat add here for now.
				if(code[(10 + chl)] != '+') {
					return;
				}
				chl = (chl + 11);
			}
		}

		else {
			//This is either a GG or PAR code
			bool isValidGgCode = true;
			bool isValidParCode = true;

			for(size_t i = 0; i < 6; i++) {
				if(validGgLetters.find(code[i]) == string::npos) {
					isValidGgCode = false;
				}
			}
			for(size_t i = 0; i < 8; i++) {
				if(validParLetters.find(code[i]) == string::npos) {
					isValidParCode = false;
				}
			}

			if(isValidGgCode && code[6] == '+') {
				for(;;) {
					string code1 = code.substr((0 + chl), 6);
				// Cheat API moved; ignore Game Genie add here for now.
					if(code[(6 + chl)] != '+') {
						return;
					}
					chl = (chl + 7);
				}
			}
			else if(isValidGgCode && code[8] == '+') {
				for(;;) {
					string code1 = code.substr((0 + chl), 8);
				// Cheat API moved; ignore Game Genie add here for now.
					if(code[(8 + chl)] != '+') {
						return;
					}
					chl = (chl + 9);
				}
			}
			else if(isValidGgCode) {
				// Cheat API moved; ignore Game Genie add here for now.
			}

			else if(isValidParCode && code[8] == '+') {
				for(;;) {
					string code1 = code.substr((0 + chl), 8);
				// Cheat API moved; ignore Pro Action Rocky code add here for now.
					if(code[(8 + chl)] != '+') {
						return;
					}
					chl = (chl + 9);
				}
			}
			else if(isValidParCode) {
				// Cheat API moved; ignore Pro Action Rocky code add here for now.
			}

		}

	}

	void update_input_descriptors()
	{
		if(!_emu || !_emu->GetConsole()) return;

		vector<retro_input_descriptor> desc;

		auto addDesc = [&desc](unsigned port, unsigned button, const char* name) {
			retro_input_descriptor d = { port, RETRO_DEVICE_JOYPAD, 0, button, name };
			desc.push_back(d);
		};

		// Detect the current system type
		ConsoleType consoleType = _emu->GetConsole()->GetConsoleType();

		// Generic SNES button layout (works for SNES, GBA, etc.)
		auto setupSnesButtons = [&addDesc](int port) {
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_A, "A");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_B, "B");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_X, "X");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_Y, "Y");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_L, "L");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_R, "R");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_START, "Start");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select");
		};

		// Gameboy button layout
		auto setupGameboyButtons = [&addDesc](int port) {
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_A, "A");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_B, "B");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_START, "Start");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select");
		};

		// NES button layout (with turbo support)
		auto setupNesButtons = [&addDesc](int port) {
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_A, "A");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_B, "B");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_X, "Turbo A");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_Y, "Turbo B");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_START, "Start");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select");
			
			if(port == 0) {
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_L, "(FDS) Insert Next Disk");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_R, "(FDS) Switch Disk Side");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_L2, "(VS) Insert Coin 1");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_R2, "(VS) Insert Coin 2");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_L3, "(Famicom) Microphone (P2)");
			}
		};

		// SMS/GG button layout
		auto setupSmsButtons = [&addDesc](int port) {
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_A, "Button 1");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_B, "Button 2");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_START, "Start");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_SELECT, "Pause");
		};

		// PCE/TG16 button layout
		auto setupPceButtons = [&addDesc](int port) {
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_B, "Button 1");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_A, "Button 2");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_Y, "Button 3");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_X, "Button 4");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_L, "Button 5");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_R, "Button 6");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_START, "Run");
		};

		// Wonderswan button layout
		auto setupWsButtons = [&addDesc](int port) {
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_A, "A");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_B, "B");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_START, "Start");
		};

		// GBA button layout
		auto setupGbaButtons = [&addDesc](int port) {
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_A, "A");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_B, "B");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_L, "L");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_R, "R");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_START, "Start");
			addDesc(port, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select");
		};

		// Set up descriptors based on system type
		switch(consoleType) {
			case ConsoleType::Snes:
				setupSnesButtons(0);
				setupSnesButtons(1);
				setupSnesButtons(2);
				setupSnesButtons(3);
				break;
			case ConsoleType::Gameboy:
				setupGameboyButtons(0);
				setupGameboyButtons(1);
				break;
			case ConsoleType::Nes:
				setupNesButtons(0);
				setupNesButtons(1);
				break;
			case ConsoleType::PcEngine:
				setupPceButtons(0);
				setupPceButtons(1);
				setupPceButtons(2);
				setupPceButtons(3);
				setupPceButtons(4);
				break;
			case ConsoleType::Sms:
				setupSmsButtons(0);
				setupSmsButtons(1);
				break;
			case ConsoleType::Gba:
				setupGbaButtons(0);
				break;
			case ConsoleType::Ws:
				setupWsButtons(0);
				break;
			default:
				setupNesButtons(0);
				break;
		}

		retro_input_descriptor end = { 0 };
		desc.push_back(end);

		// Avoid sending identical input descriptors repeatedly (RetroArch logs each SET_INPUT_DESCRIPTORS call).
		// Build a lightweight representation we can compare to the last one and skip the env_cb if unchanged.
		struct SimpleDesc { unsigned port; unsigned device; unsigned index; unsigned id; std::string name; };
		static std::vector<SimpleDesc> lastDesc;
		std::vector<SimpleDesc> curDesc;
		curDesc.reserve(desc.size());
		for(size_t i = 0; i < desc.size(); ++i) {
			const retro_input_descriptor &d = desc[i];
			if(d.description == nullptr) break; // end sentinel
			curDesc.push_back({ d.port, d.device, d.index, d.id, std::string(d.description) });
		}

		bool same = (curDesc.size() == lastDesc.size());
		if(same) {
			for(size_t i = 0; i < curDesc.size(); ++i) {
				const SimpleDesc &a = curDesc[i];
				const SimpleDesc &b = lastDesc[i];
				if(a.port != b.port || a.device != b.device || a.index != b.index || a.id != b.id || a.name != b.name) { same = false; break; }
			}
		}

		if(!same) {
			env_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc.data());
			lastDesc.swap(curDesc);
		}
	}

	void update_core_controllers()
	{
		// Map libretro-selected devices into the emulator's settings.
		// Guard against being called before a console/emulator exists.
		if(!_emu || !_emu->GetConsole()) return;

		// Ensure ports 0/1 default to gamepads when still "auto"
		if(_inputDevices[0] == DEVICE_AUTO) _inputDevices[0] = DEVICE_GAMEPAD;
		if(_inputDevices[1] == DEVICE_AUTO) _inputDevices[1] = DEVICE_GAMEPAD;
		// make sure other ports have a sane default
		for(int port = 2; port < 5; ++port) {
			if(_inputDevices[port] == DEVICE_AUTO) _inputDevices[port] = RETRO_DEVICE_NONE;
		}

		ConsoleType consoleType = _emu->GetConsole()->GetConsoleType();

		// Map port 0/1 controller types based on the system
		switch(consoleType) {
			case ConsoleType::Snes: {
				SnesConfig snesCfg = _emu->GetSettings()->GetSnesConfig();
				// Always set controller types based on frontend selection or default to gamepad
				snesCfg.Port1.Type = (_inputDevices[0] == DEVICE_GAMEPAD) ? ControllerType::SnesController : ControllerType::None;
				snesCfg.Port2.Type = (_inputDevices[1] == DEVICE_GAMEPAD) ? ControllerType::SnesController : ControllerType::None;
				_emu->GetSettings()->SetSnesConfig(snesCfg);
				break;
			}
			case ConsoleType::Gameboy: {
				GameboyConfig gbCfg = _emu->GetSettings()->GetGameboyConfig();
				gbCfg.Controller.Type = (_inputDevices[0] == DEVICE_GAMEPAD) ? ControllerType::GameboyController : ControllerType::None;
				_emu->GetSettings()->SetGameboyConfig(gbCfg);
				break;
			}
			case ConsoleType::Gba: {
				GbaConfig gbaCfg = _emu->GetSettings()->GetGbaConfig();
				gbaCfg.Controller.Type = (_inputDevices[0] == DEVICE_GAMEPAD) ? ControllerType::GbaController : ControllerType::None;
				_emu->GetSettings()->SetGbaConfig(gbaCfg);
				break;
			}
			case ConsoleType::PcEngine: {
				PcEngineConfig pceCfg = _emu->GetSettings()->GetPcEngineConfig();
				pceCfg.Port1.Type = (_inputDevices[0] == DEVICE_GAMEPAD) ? ControllerType::PceController : ControllerType::None;
				// Note: PCE has only Port1 but we still support multi-port in libretro
				_emu->GetSettings()->SetPcEngineConfig(pceCfg);
				break;
			}
			case ConsoleType::Sms: {
				SmsConfig smsCfg = _emu->GetSettings()->GetSmsConfig();
				smsCfg.Port1.Type = (_inputDevices[0] == DEVICE_GAMEPAD) ? ControllerType::SmsController : ControllerType::None;
				smsCfg.Port2.Type = (_inputDevices[1] == DEVICE_GAMEPAD) ? ControllerType::SmsController : ControllerType::None;
				for(int i = 0; i < 4; i++) {
					smsCfg.ChannelVolumes[i] = 100;
				}
				_emu->GetSettings()->SetSmsConfig(smsCfg);
				break;
			}
			case ConsoleType::Ws: {
				WsConfig wsCfg = _emu->GetSettings()->GetWsConfig();
				wsCfg.ControllerHorizontal.Type = (_inputDevices[0] == DEVICE_GAMEPAD) ? ControllerType::WsController : ControllerType::None;
				_emu->GetSettings()->SetWsConfig(wsCfg);
				break;
			}
			case ConsoleType::Nes:
			default: {
				// Default to NES handling for backward compatibility
				NesConfig nesCfg = _emu->GetSettings()->GetNesConfig();
				for(int port = 0; port <= 1; ++port) {
					ControllerType type = ControllerType::NesController;
					switch(_inputDevices[port]) {
						case RETRO_DEVICE_NONE: type = ControllerType::None; break;
						case DEVICE_GAMEPAD: type = ControllerType::NesController; break;
						case DEVICE_ZAPPER: type = ControllerType::NesZapper; break;
						case DEVICE_POWERPAD: type = ControllerType::PowerPadSideA; break;
						case DEVICE_ARKANOID: type = ControllerType::NesArkanoidController; break;
						default: break;
					}
					if(port == 0) nesCfg.Port1.Type = type;
					else if(port == 1) nesCfg.Port2.Type = type;
				}
				_emu->GetSettings()->SetNesConfig(nesCfg);
				break;
			}
		}
	}

	void setup_default_key_mappings()
	{
		// Set up default libretro button mappings for all systems
		// Keycode format: (port << 8) | (retroKey + 1)
		// This enables input even when no custom key mappings have been configured
		
		if(!_emu || !_emu->GetConsole()) return;
		
		ConsoleType consoleType = _emu->GetConsole()->GetConsoleType();
		
		// Helper lambda to set a KeyMapping button
		auto setButton = [](uint16_t& dest, int port, int retroButton) {
			dest = (port << 8) | (retroButton + 1);
		};
		
		// Helper lambda to set up a full KeyMappingSet with default buttons
		auto setupDefaultMapping = [&](KeyMappingSet& keys, int port) {
			// Only set up Mapping1 if it doesn't already have keys
			if(!keys.Mapping1.HasKeySet()) {
				// D-Pad
				setButton(keys.Mapping1.Up, port, RETRO_DEVICE_ID_JOYPAD_UP);
				setButton(keys.Mapping1.Down, port, RETRO_DEVICE_ID_JOYPAD_DOWN);
				setButton(keys.Mapping1.Left, port, RETRO_DEVICE_ID_JOYPAD_LEFT);
				setButton(keys.Mapping1.Right, port, RETRO_DEVICE_ID_JOYPAD_RIGHT);
				
				// Action buttons
				setButton(keys.Mapping1.A, port, RETRO_DEVICE_ID_JOYPAD_A);
				setButton(keys.Mapping1.B, port, RETRO_DEVICE_ID_JOYPAD_B);
				setButton(keys.Mapping1.Start, port, RETRO_DEVICE_ID_JOYPAD_START);
				setButton(keys.Mapping1.Select, port, RETRO_DEVICE_ID_JOYPAD_SELECT);
				
				// Additional buttons (X, Y, L, R for systems that have them)
				setButton(keys.Mapping1.X, port, RETRO_DEVICE_ID_JOYPAD_X);
				setButton(keys.Mapping1.Y, port, RETRO_DEVICE_ID_JOYPAD_Y);
				setButton(keys.Mapping1.L, port, RETRO_DEVICE_ID_JOYPAD_L);
				setButton(keys.Mapping1.R, port, RETRO_DEVICE_ID_JOYPAD_R);
			}
		};
		
		switch(consoleType) {
			case ConsoleType::Snes: {
				SnesConfig snesCfg = _emu->GetSettings()->GetSnesConfig();
				setupDefaultMapping(snesCfg.Port1.Keys, 0);
				setupDefaultMapping(snesCfg.Port2.Keys, 1);
				_emu->GetSettings()->SetSnesConfig(snesCfg);
				break;
			}
			case ConsoleType::Gameboy: {
				GameboyConfig gbCfg = _emu->GetSettings()->GetGameboyConfig();
				setupDefaultMapping(gbCfg.Controller.Keys, 0);
				_emu->GetSettings()->SetGameboyConfig(gbCfg);
				break;
			}
			case ConsoleType::Gba: {
				GbaConfig gbaCfg = _emu->GetSettings()->GetGbaConfig();
				setupDefaultMapping(gbaCfg.Controller.Keys, 0);
				_emu->GetSettings()->SetGbaConfig(gbaCfg);
				break;
			}
			case ConsoleType::PcEngine: {
				PcEngineConfig pceCfg = _emu->GetSettings()->GetPcEngineConfig();
				setupDefaultMapping(pceCfg.Port1.Keys, 0);
				setupDefaultMapping(pceCfg.Port1.Keys, 1);  // Note: PcEngine only has Port1, treating as multi-port
				_emu->GetSettings()->SetPcEngineConfig(pceCfg);
				break;
			}
			case ConsoleType::Sms: {
				SmsConfig smsCfg = _emu->GetSettings()->GetSmsConfig();
				setupDefaultMapping(smsCfg.Port1.Keys, 0);
				setupDefaultMapping(smsCfg.Port2.Keys, 1);
				_emu->GetSettings()->SetSmsConfig(smsCfg);
				break;
			}
			case ConsoleType::Ws: {
				WsConfig wsCfg = _emu->GetSettings()->GetWsConfig();
				setupDefaultMapping(wsCfg.ControllerHorizontal.Keys, 0);
				_emu->GetSettings()->SetWsConfig(wsCfg);
				break;
			}
			case ConsoleType::Nes:
			default: {
				// For NES, set up default keys if not already configured
				NesConfig nesCfg = _emu->GetSettings()->GetNesConfig();
				setupDefaultMapping(nesCfg.Port1.Keys, 0);
				setupDefaultMapping(nesCfg.Port2.Keys, 1);
				_emu->GetSettings()->SetNesConfig(nesCfg);
				break;
			}
		}
	}

	void retro_set_memory_maps()
	{
		// The console/Memory APIs were refactored. Provide an empty memory map for now.
		retro_memory_map memoryMap = {};
		memoryMap.descriptors = nullptr;
		memoryMap.num_descriptors = 0;
		env_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &memoryMap);
/*		//Expose internal RAM and work/save RAM for retroachievements
		retro_memory_descriptor descriptors[256] = {};
		retro_memory_map memoryMap = {};

		int count = 0;
		for(int i = 0; i <= 0xFFFF; i += 0x100) {
			uint8_t* ram = _console->GetRamBuffer(i);
			if(ram) {
				descriptors[count].ptr = ram;
				descriptors[count].start = i;
				descriptors[count].len = 0x100;
				count++;
			}
		}

		memoryMap.descriptors = descriptors;
		memoryMap.num_descriptors = count;

		env_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &memoryMap);
*/	}

	RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
	{
		if(port < 5 && _inputDevices[port] != device) {
			_inputDevices[port] = device;
		update_core_controllers();
		update_input_descriptors();
		}
	}

	RETRO_API bool retro_load_game(const struct retro_game_info *game)
	{
		logSgbDebugf("retro_load_game called: path=%s", game && game->path ? game->path : "(null)");
		char *saveFolder;
		char *systemFolder;
		if(!env_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &systemFolder) || !systemFolder)
			return false;

		if(!env_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &saveFolder)) {
			logMessage(RETRO_LOG_ERROR, "Could not find save directory.\n");
		}

		enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
		if(!env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
			logMessage(RETRO_LOG_ERROR, "XRGB8888 is not supported.\n");
			return false;
		}

		//Expect the following structure:
		// /system/disksys.rom
		// /system/HdPacks/*
		// /system/<BIOS files>
		// /saves/*.sav
		FolderUtilities::SetHomeFolder(systemFolder);
		// SetFolderOverrides signature expects 4 strings (save, savestate, screenshot, firmware).
		// Point firmware lookups at the system directory root.
		FolderUtilities::SetFolderOverrides(saveFolder, std::string(""), std::string(""), string(systemFolder));
		update_settings();

		// Controller/Settings API changed; skip initial controller setup here
		//Plug in 2 standard controllers by default, game database will switch the controller types for recognized games
/*		_console->GetSettings()->SetMasterVolume(10.0);
		_console->GetSettings()->SetControllerType(0, ControllerType::StandardController);
		_console->GetSettings()->SetControllerType(1, ControllerType::StandardController);
		_console->GetSettings()->SetControllerType(2, ControllerType::None);
		_console->GetSettings()->SetControllerType(3, ControllerType::None);
*/
		// Attempt to fetch extended game info
		const struct retro_game_info_ext *gameExt = NULL;
		const void *gameData = NULL;
		size_t gameSize = 0;
		string gamePath("");
		if (env_cb(RETRO_ENVIRONMENT_GET_GAME_INFO_EXT, &gameExt)) {
			gameData = gameExt->data;
			gameSize = gameExt->size;
			if (gameExt->file_in_archive) {
				// We don't have a 'physical' file in this
				// case, but the core still needs a filename
				// in order to detect associated content
				// (i.e. HdPacks). We therefore fake it, using
				// the content directory, canonical content
				// name, and content file extension
#if defined(_WIN32)
				char slash = '\\';
#else
				char slash = '/';
#endif
				gamePath = string(gameExt->dir) +
							  string(1, slash) +
							  string(gameExt->name) +
							  "." +
							  string(gameExt->ext);
			} else {
				gamePath = gameExt->full_path;
			}
		} else {
			// No extended game info; all we have is the
			// content fullpath from the retro_game_info
			// struct
			gamePath = game->path;
		}

/*		// Load content
		VirtualFile romData(gameData, gameSize, gamePath);
		bool result = _console->Initialize(romData);

		if(result) {
			//Set default dipswitches for some VS System games
			switch(_console->GetRomInfo().Hash.PrgCrc32) {
				case 0x8850924B: _console->GetSettings()->SetDipSwitches(32); break; //VS Tetris
				case 0xE1AA8214: _console->GetSettings()->SetDipSwitches(32); break; //StarLuster
				default: _console->GetSettings()->SetDipSwitches(0); break;
			}

			update_core_controllers();
			update_input_descriptors();

			//Savestates in Mesen may change size over time
			//Retroarch doesn't like this for netplay or rewinding - it requires the states to always be the exact same size
			//So we need to send a large enough size to Retroarch to ensure Mesen's state will always fit within that buffer.
			std::stringstream ss;
			_console->GetSaveStateManager()->SaveState(ss);

			//Round up to the next 1kb multiple
			_saveStateSize = ((ss.str().size() * 2) + 0x400) & ~0x3FF;
			retro_set_memory_maps();
		}

		return result;
*/
		// Build a VirtualFile for the ROM (either from memory or from a path)
		VirtualFile romData;
		if(gameData && gameSize > 0) {
			romData = VirtualFile(gameData, gameSize, gamePath);
		} else {
			romData = VirtualFile(gamePath);
		}

		// Set HD pack configuration BEFORE loading ROM (LoadHdPack checks this during ROM load)
		{
			NesConfig nesCfg = _emu->GetSettings()->GetNesConfig();
			nesCfg.EnableHdPacks = _hdPacksEnabled;
			_emu->GetSettings()->SetNesConfig(nesCfg);
		}

		// Attempt to load the ROM via the Emulator API
		bool result = false;
		string tempPceFirmwarePath;
		string tempPceFirmwareBackup;
		if(romData.GetFileExtension() == ".cue") {
			char *systemFolder;
			if(env_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &systemFolder) && systemFolder) {
				if(CreatePceFirmwareStub(string(systemFolder), tempPceFirmwarePath, tempPceFirmwareBackup)) {
					fprintf(stderr, "[libretro] Created temporary PCE firmware stub: %s\n", tempPceFirmwarePath.c_str());
					fflush(stderr);
				}
			}
		}
		try {
			// Do not instruct the emulator to stop any existing ROM here - letting it avoid
			// the Stop() path which can trigger complex shutdown behavior inside a libretro host.
			fprintf(stderr, "[libretro] Attempting to load ROM: %s\n", gamePath.c_str());
			fprintf(stderr, "[libretro]   Path: %s\n", romData.GetFilePath().c_str());
			fprintf(stderr, "[libretro]   Folder: %s\n", romData.GetFolderPath().c_str());
			fprintf(stderr, "[libretro]   Extension: %s\n", romData.GetFileExtension().c_str());
			fprintf(stderr, "[libretro]   From memory: %s\n", (gameData && gameSize > 0) ? "yes" : "no");
			fflush(stderr);
			result = _emu->LoadRom(romData, VirtualFile(), false, false);
			fprintf(stderr, "[libretro] LoadRom returned: %s\n", result ? "success" : "failure");
			fflush(stderr);
		} catch(const std::exception& e) {
			fprintf(stderr, "[libretro] LoadRom threw exception: %s\n", e.what());
			fflush(stderr);
			result = false;
		} catch(...) {
			fprintf(stderr, "[libretro] LoadRom threw unknown exception\n");
			fflush(stderr);
			result = false;
		}

		if(!tempPceFirmwarePath.empty()) {
			RestorePceFirmwareStub(tempPceFirmwarePath, tempPceFirmwareBackup);
			fprintf(stderr, "[libretro] Restored temporary PCE firmware stub: %s\n", tempPceFirmwarePath.c_str());
			fflush(stderr);
		}

		if(result) {
			// Update the console shared_ptr now that a ROM is loaded
			// Use the generic IConsole interface to support all system types
			auto consoleIf = _emu->GetConsole();
			_console = consoleIf;

			// Disable frame blending for libretro - we want raw frames
			// Post-processing should happen in the frontend
			ConsoleType type = _console->GetConsoleType();
			switch(type) {
				case ConsoleType::Snes: {
					SnesConfig cfg = _emu->GetSettings()->GetSnesConfig();
					cfg.HighResBlendMode = SnesHighResBlendMode::None;
					_emu->GetSettings()->SetSnesConfig(cfg);
					break;
				}
				case ConsoleType::Gameboy: {
					GameboyConfig cfg = _emu->GetSettings()->GetGameboyConfig();
					cfg.BlendFrames = false;
					_emu->GetSettings()->SetGameboyConfig(cfg);
					break;
				}
				case ConsoleType::Gba: {
					GbaConfig cfg = _emu->GetSettings()->GetGbaConfig();
					cfg.BlendFrames = false;
					_emu->GetSettings()->SetGbaConfig(cfg);
					break;
				}
				case ConsoleType::Sms: {
					SmsConfig cfg = _emu->GetSettings()->GetSmsConfig();
					cfg.GgBlendFrames = false;
					_emu->GetSettings()->SetSmsConfig(cfg);
					break;
				}
				case ConsoleType::PcEngine: {
					// Initialize PCE config with default palette (RGB333)
					// PCE uses a 9-bit color space (RGB333 - 3 bits each for R, G, B)
					PcEngineConfig cfg = _emu->GetSettings()->GetPcEngineConfig();
					// Generate default RGB333 palette if not already set
					if(cfg.Palette[0] == 0) {  // If palette is all zeros, initialize it
						for(int rgb333 = 0; rgb333 < 512; rgb333++) {
							// Extract 3-bit R, G, B components and scale to 8-bit
							// Values for red and green channels are swapped, but it
							// works, so whatever. TODO/FIXME
							uint8_t r = ((rgb333 >> 3) & 0x7) * 255 / 7;  // bits 8,7,6
							uint8_t g = ((rgb333 >> 6) & 0x7) * 255 / 7;  // bits 5,4,3
							uint8_t b = ((rgb333 >> 0) & 0x7) * 255 / 7;  // bits 2,1,0
							cfg.Palette[rgb333] = 0xFF000000 | (r << 16) | (g << 8) | b;
						}
					}
				_emu->GetSettings()->SetPcEngineConfig(cfg);
				break;
			}
				default:
					break;
			}

			// Inform the LibretroKeyManager of the concrete console/emulator so it can
			// safely mark itself ready to be polled and access emulator data (mouse pos, etc.).
			if(_keyManager) {
				_keyManager->SetConsole(consoleIf, _emu.get());
			}

			// Initialize audio settings to enable sound output
			// Ensure audio is enabled and channel volumes are set
			{
				AudioConfig audioCfg = _emu->GetSettings()->GetAudioConfig();
				audioCfg.EnableAudio = true;
				audioCfg.MasterVolume = 100;
				_emu->GetSettings()->SetAudioConfig(audioCfg);
				
				NesConfig nesCfg = _emu->GetSettings()->GetNesConfig();
				// Initialize all channel volumes to 100 (full volume) if they're zero
				for(size_t i = 0; i < 11; ++i) {
					if(nesCfg.ChannelVolumes[i] == 0) {
						nesCfg.ChannelVolumes[i] = 100;
					}
					// Initialize panning to center (50) if zero
					if(nesCfg.ChannelPanning[i] == 0) {
						nesCfg.ChannelPanning[i] = 50;
					}
				}
				_emu->GetSettings()->SetNesConfig(nesCfg);
			}

			// Allow the game/db-specific controller initialization to run
			update_core_controllers();
			
			// Set up default libretro button mappings for systems that don't have custom key configs
			setup_default_key_mappings();
			
			// Re-run controller initialization with the new key mappings
			update_core_controllers();
			
			update_input_descriptors();

			// Compute a safe save state size to report to the frontend
			std::stringstream ss;
			_emu->GetSaveStateManager()->SaveState(ss);
			_saveStateSize = ((ss.str().size() * 2) + 0x400) & ~0x3FF;

			// Update memory maps (still a stubbed implementation for now)
			retro_set_memory_maps();

			// Register the libretro audio device now that the emulator is initialized and consoles are ready
			if(_audioDevice && _emu && _emu->GetSoundMixer()) {
				_emu->GetSoundMixer()->RegisterAudioDevice(_audioDevice.get());
			}

			// Update geometry if HD packs are loaded (they may change the resolution)
			// HD packs are NES-specific, so only check for NES consoles
			if(_console && _console->GetConsoleType() == ConsoleType::Nes) {
				auto nesConsole = std::dynamic_pointer_cast<NesConsole>(_console);
				if(nesConsole && nesConsole->GetHdData()) {
					retro_system_av_info avInfo = {};
					retro_get_system_av_info(&avInfo);
					env_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &avInfo);
				}
			}

			// Forward any saved input callbacks into the key manager now that console/key manager are initialized.
			if(_keyManager) {
				if(_savedGetInputState) _keyManager->SetGetInputState(_savedGetInputState);
				if(_savedPollInput) _keyManager->SetPollInput(_savedPollInput);
				// Re-check bitmask support
				if (env_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
					_keyManager->SetSupportsInputBitmasks(true);
			}

			// Diagnostic probe: call saved callbacks once from this thread and log their values.
			// This uses the shared probe helper so we don't duplicate the implementation.
			if(_savedPollInput || _savedGetInputState) {
				// forward-declare the shared probe helper (defined later in this file)
				if(getenv("MESEN_LIBRETRO_VERBOSE_INPUT")) {
					void libretro_probe_inputs(const char* tag);
					libretro_probe_inputs("retro_load_game");
				}
			}
		} else {
			logMessage(RETRO_LOG_ERROR, "retro_load_game: Failed to load ROM via Emulator::LoadRom.\n");
			_saveStateSize = 0;
		}

		return result;
	}

	static constexpr unsigned SubsystemId_Sgb = 2;

	static bool isSupportedSubsystem(unsigned game_type)
	{
		return game_type == SubsystemId_Sgb; // Super Game Boy subsystem
	}

	static bool isGameboyRom(const struct retro_game_info* info)
	{
		if(!info) {
			return false;
		}

		if(info->path) {
			string path = info->path;
			transform(path.begin(), path.end(), path.begin(), ::tolower);
			if(path.find(".gb") != string::npos || path.find(".gbc") != string::npos || path.find(".gbx") != string::npos) {
				return true;
			}
		}

		return false;
	}

	static bool isSgbFirmwareRom(const struct retro_game_info* info, bool& useSgb2)
	{
		if(!info) {
			return false;
		}

		logSgbDebugf("isSgbFirmwareRom: info=%p data=%p size=%zu path=%s", (const void*)info, info->data, info->size, info->path ? info->path : "(null)");

		auto checkSize = [&](size_t size) {
			if(size == 0x40000) {
				useSgb2 = false;
				return true;
			}
			if(size == 0x80000) {
				useSgb2 = true;
				return true;
			}
			return false;
		};

		if(info->data && info->size > 0) {
			return checkSize(info->size);
		}

		if(info->path) {
			string path = info->path;
			transform(path.begin(), path.end(), path.begin(), ::tolower);
			if(path.find(".sfc") != string::npos || path.find(".smc") != string::npos) {
				if(info->size > 0) {
					return checkSize(info->size);
				}
				VirtualFile firmwareFile(info->path);
				if(firmwareFile.IsValid()) {
					return checkSize(firmwareFile.GetSize());
				}
			}
		}

		return false;
	}

	static bool writeGameInfoToFile(const struct retro_game_info* info, const string& outputPath)
	{
		if(!info || outputPath.empty()) {
			return false;
		}

		vector<uint8_t> sourceData;
		if(info->data && info->size > 0) {
			sourceData.assign((const uint8_t*)info->data, (const uint8_t*)info->data + info->size);
		} else if(info->path) {
			VirtualFile sourceFile(info->path);
			if(!sourceFile.IsValid()) {
				return false;
			}
			if(!sourceFile.ReadFile(sourceData)) {
				return false;
			}
		} else {
			return false;
		}

		utf8::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
		if(!output) {
			return false;
		}
		output.write((const char*)sourceData.data(), sourceData.size());
		return output.good();
	}

	static bool copySgbFirmwareToSystem(const struct retro_game_info* info, string& createdFirmwarePath, string& backupFirmwarePath)
	{
		if(!info) {
			return false;
		}

		bool useSgb2 = false;
		if(!isSgbFirmwareRom(info, useSgb2)) {
			logSgbDebugf("copySgbFirmwareToSystem: info is not recognized as SGB firmware");
			return false;
		}

		string firmwareFilename = useSgb2 ? "SGB2.sfc" : "SGB1.sfc";
		char *systemFolder;
		if(!env_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &systemFolder) || !systemFolder) {
			logSgbDebugf("copySgbFirmwareToSystem: RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY failed");
			return false;
		}

		string firmwarePath = FolderUtilities::CombinePath(string(systemFolder), firmwareFilename);
		logSgbDebugf("copySgbFirmwareToSystem: system folder=%s firmware=%s", systemFolder, firmwarePath.c_str());
		createdFirmwarePath.clear();
		backupFirmwarePath.clear();

		{
			utf8::ifstream existing(firmwarePath, std::ios::binary);
			if(existing.good()) {
				backupFirmwarePath = firmwarePath + ".libretro.bak";
				int backupIndex = 1;
				while(utf8::ifstream(backupFirmwarePath, std::ios::binary).good()) {
					backupFirmwarePath = firmwarePath + ".libretro.bak" + std::to_string(backupIndex);
					backupIndex++;
				}
				if(!VfsIo::Rename(firmwarePath, backupFirmwarePath)) {
					return false;
				}
			}
		}

		if(!writeGameInfoToFile(info, firmwarePath)) {
			logSgbDebugf("copySgbFirmwareToSystem: failed to write custom firmware to %s", firmwarePath.c_str());
			if(!backupFirmwarePath.empty()) {
				VfsIo::Rename(backupFirmwarePath, firmwarePath);
			}
			return false;
		}

		createdFirmwarePath = firmwarePath;
		logSgbDebugf("copySgbFirmwareToSystem: wrote custom firmware to %s", firmwarePath.c_str());
		return true;
	}

	static void restoreSgbFirmware(const string& firmwarePath, const string& backupFirmwarePath)
	{
		if(firmwarePath.empty()) {
			return;
		}

		if(!backupFirmwarePath.empty()) {
			VfsIo::Rename(backupFirmwarePath, firmwarePath);
		} else {
			VfsIo::Remove(firmwarePath);
		}
	}

	static bool loadSubsystemGame(unsigned game_type, const struct retro_game_info *info, size_t num_info)
	{
		logSgbDebugf("loadSubsystemGame called: game_type=%u num_info=%zu", game_type, num_info);
	if(!isSupportedSubsystem(game_type) || !info || num_info == 0) {
		logSgbDebugf("loadSubsystemGame: unsupported subsystem or invalid info");
		return false;
	}

	if(game_type == SubsystemId_Sgb) {
			logSgbDebugf("retro_load_game_special: starting SGB subsystem load");
			logSgbDebugf("loadSubsystemGame: game_type=%u num_info=%zu", game_type, num_info);
			if(info[0].path) logSgbDebugf("  gbInfo.path=%s size=%zu", info[0].path, info[0].size);
			if(num_info > 1 && info[1].path) logSgbDebugf("  sgbInfo.path=%s size=%zu", info[1].path, info[1].size);
			const struct retro_game_info* gbInfo = &info[0];
			const struct retro_game_info* sgbInfo = (num_info > 1 ? &info[1] : nullptr);

			if(!gbInfo || !isGameboyRom(gbInfo)) {
				logMessage(RETRO_LOG_ERROR, "retro_load_game_special: Super Game Boy load requires a Game Boy ROM as the first entry.");
				return false;
			}

			string firmwarePath;
			string backupPath;
			bool customFirmwareInstalled = false;
			if(sgbInfo) {
				bool useSgb2 = false;
				if(isSgbFirmwareRom(sgbInfo, useSgb2)) {
					customFirmwareInstalled = copySgbFirmwareToSystem(sgbInfo, firmwarePath, backupPath);
					if(customFirmwareInstalled) {
						logSgbDebugf("Installed custom SGB firmware: %s", firmwarePath.c_str());
					} else {
						logSgbDebugf("copySgbFirmwareToSystem returned false for firmware path %s", sgbInfo->path ? sgbInfo->path : "(null)");
					}
				} else {
					logMessage(RETRO_LOG_INFO, "retro_load_game_special: Second entry is not valid SGB firmware; falling back to existing system firmware if available.");
				}
			}

			// Tell the emulator to allow SGB for the next Gameboy console that is created
			if(_emu) _emu->SetAllowSgbForNextLoad(true);
			// Temporarily prefer SGB model when selecting the Gameboy model so SGB will be chosen
			GameboyConfig _libretro_prevGbCfg;
			bool _libretro_havePrevGbCfg = false;
			if(_emu) {
				_libretro_prevGbCfg = _emu->GetSettings()->GetGameboyConfig();
				_libretro_havePrevGbCfg = true;
				GameboyConfig tmpGbCfg = _libretro_prevGbCfg;
				tmpGbCfg.Model = GameboyModel::AutoFavorSgb;
				_emu->GetSettings()->SetGameboyConfig(tmpGbCfg);
			}

			bool result = retro_load_game(gbInfo);
			logSgbDebugf("retro_load_game returned %s for GB ROM %s", result ? "success" : "failure", gbInfo->path ? gbInfo->path : "(null)");

			// Restore previous Gameboy config
			if(_emu && _libretro_havePrevGbCfg) {
				_emu->GetSettings()->SetGameboyConfig(_libretro_prevGbCfg);
			}


			if(customFirmwareInstalled) {
				restoreSgbFirmware(firmwarePath, backupPath);
				logSgbDebugf("Restored SGB firmware %s", firmwarePath.c_str());
			}

			return result;
		}

		return false;
	}

	RETRO_API bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
	{
		return loadSubsystemGame(game_type, info, num_info);
	}

	RETRO_API void retro_unload_game()
	{
	}

	RETRO_API unsigned retro_get_region()
	{
	/*	NesModel model = _console->GetModel();
		return model == NesModel::NTSC ? RETRO_REGION_NTSC : RETRO_REGION_PAL;
	*/
		// The NesConsole / Emulator APIs were refactored. Return NTSC for now.
		return RETRO_REGION_NTSC;	
	}

	RETRO_API void retro_get_system_info(struct retro_system_info *info)
	{
//		_mesenVersion = EmuSettings::GetVersionString();
/*
		// Set library name based on loaded console. This pushes configs into
		// separate folders for each sub-core
		static std::string libraryName = "Mesen2";
		if(_console) {
			ConsoleType type = _console->GetConsoleType();
			switch(type) {
				case ConsoleType::Nes: libraryName = "Mesen2-NES"; break;
				case ConsoleType::Snes: libraryName = "Mesen2-SNES"; break;
				case ConsoleType::Gameboy: libraryName = "Mesen2-Gameboy"; break;
				case ConsoleType::Gba: libraryName = "Mesen2-GBA"; break;
				case ConsoleType::PcEngine: libraryName = "Mesen2-PCEngine"; break;
				case ConsoleType::Sms: libraryName = "Mesen2-SMS"; break;
				case ConsoleType::Ws: libraryName = "Mesen2-Wonderswan"; break;
				default: libraryName = "Mesen2"; break;
			}
		}
*/
		info->library_name = "Mesen2";
		info->library_version = "2.0.0";
		info->need_fullpath = true;
		
		// Set valid extensions based on loaded console
		static std::string validExtensions;
		if(_console) {
			ConsoleType type = _console->GetConsoleType();
			switch(type) {
				case ConsoleType::Nes: 
					validExtensions = "nes|fds|unf|unif";
					break;
				case ConsoleType::Snes:
					validExtensions = "sfc|smc";
					break;
				case ConsoleType::Gameboy:
					validExtensions = "gb|gbc";
					break;
				case ConsoleType::Gba:
					validExtensions = "gba";
					break;
				case ConsoleType::PcEngine:
					validExtensions = "pce|sgx|cue";
					break;
				case ConsoleType::Sms:
					validExtensions = "sms|gg";
					break;
				case ConsoleType::Ws:
					validExtensions = "ws|wsc";
					break;
				default:
					validExtensions = "nes|fds|unf|unif|sfc|smc|gb|gbc|gba|pce|sgx|sms|gg|ws|wsc";
					break;
			}
		} else {
			// No console loaded yet, use all supported extensions
			validExtensions = "nes|fds|unf|unif|sfc|smc|gb|gbc|gba|pce|sgx|cue|sms|gg|ws|wsc";
		}
		info->valid_extensions = validExtensions.c_str();
		info->block_extract = false;
	}

	RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
	{
		memset(info, 0, sizeof(*info));
		
		uint32_t width = 256;
		uint32_t height = 240;
		
		// Get actual filtered frame size if available
		if(_emu && _emu->GetVideoDecoder()) {
			FrameInfo frameSize = _emu->GetVideoDecoder()->GetFrameInfo();
			width = frameSize.Width;
			height = frameSize.Height;
		}
		
		// Check if HD packs are loaded and apply additional scaling
		// HD packs are NES-specific
		uint32_t hscale = 1;
		uint32_t vscale = 1;
		
		if(_console && _console->GetConsoleType() == ConsoleType::Nes) {
			auto nesConsole = std::dynamic_pointer_cast<NesConsole>(_console);
			if(nesConsole) {
				auto hdData = nesConsole->GetHdData().lock();
				if(hdData) {
					hscale = hdData->Scale;
					vscale = hdData->Scale;
				}
			}
		}
		
		// Apply HD scale to final dimensions
		width *= hscale;
		height *= vscale;
		
		info->geometry.base_width = width;
		info->geometry.base_height = height;
		// TODO/FIXME: set these to something bigger when HD packs are implemented
		info->geometry.max_width = 512;//256 * 8 * 4; // generous max (8x scale + 4x HD)
		info->geometry.max_height = 480;//240 * 8 * 4;
		float system_ar;
		if(_console) {
			ConsoleType type = _console->GetConsoleType();
			switch(type) {
				case ConsoleType::Nes: system_ar = 1.306; break;
				case ConsoleType::Snes: system_ar = 64.0f / 49.0f; break;
				case ConsoleType::Gameboy: system_ar = 10.0f / 9.0f; break;
				case ConsoleType::Gba: system_ar = 3.0f / 2.0f; break;
				case ConsoleType::PcEngine: system_ar = 4.0f / 3.0f; break;
				case ConsoleType::Sms: system_ar = 4.0f / 3.0f; break;
				case ConsoleType::Ws: system_ar = 14.0f / 9.0f; break;
				default: system_ar = 4.0f / 3.0f; break;
			}
		}
		info->geometry.aspect_ratio = system_ar;
		
		// Get FPS from console, or use default
		double fps = 60.0988; // NES NTSC default
		if(_console) {
			fps = _console->GetFps();
		}
		info->timing.fps = fps;
		info->timing.sample_rate = 44100.0;
	}

	RETRO_API void *retro_get_memory_data(unsigned id)
	{
		// Memory access is NES-specific for now
		if(!_console || _console->GetConsoleType() != ConsoleType::Nes) {
			return nullptr;
		}
		
		auto nesConsole = std::dynamic_pointer_cast<NesConsole>(_console);
		if(!nesConsole) return nullptr;
		
		BaseMapper* mapper = nesConsole->GetMapper();
		switch(id) {
			case RETRO_MEMORY_SAVE_RAM:
				// Save-ram accessor moved/hidden in BaseMapper; return nullptr until mapped to new API.
				(void)mapper;
				return nullptr;
			case RETRO_MEMORY_SYSTEM_RAM:
				// Use the renamed GetInternalRam() if available on NesMemoryManager.
				if(nesConsole->GetMemoryManager()) {
					return nesConsole->GetMemoryManager()->GetInternalRam();
				}
				return nullptr;
		}
		return nullptr;
	}

	RETRO_API size_t retro_get_memory_size(unsigned id)
	{
		switch(id) {
			case RETRO_MEMORY_SAVE_RAM: //return mapper->GetMemorySize(DebugMemoryType::SaveRam);
			   // Mapper API changed; save-RAM access moved. Return 0 for now.
            // TODO: replace with mapper->GetSaveRam() equivalent when API is available.
            return 0;
			case RETRO_MEMORY_SYSTEM_RAM: //return MemoryManager::InternalRAMSize;
            // Memory manager API renamed to GetInternalRam()
            // Use the new method if available; otherwise return 0.
            return 0;		
		}
		return 0;
	}
}
