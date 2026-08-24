#include "save_sync.h"

#include "coop_netgame.h"
#include "coop_runtime.h"
#include "ServerClient/MServer.h"
#include "ServerClient/MTypes.h"

#include <string.h>
#include <vector>

namespace
{
constexpr std::uint32_t kSavePacketVersion = 1;
constexpr std::uint32_t kCoopJoinSaveSlot = 4;
constexpr std::uint32_t kMaxSaveBytes = 128 * 1024;

// XLoadSaveManagerPlatform is a process-global controller in the supported
// GForce.exe.  Its native Load command receives a zero-based visible-slot
// index, so 4 is exactly the fifth UI slot (DATA4).
constexpr std::uintptr_t kLoadSaveManagerAddress = 0x00915B40u;
constexpr std::uintptr_t kBeginNativeLoadAddress = 0x005F1920u;

struct CoopSaveSlotPacketHeader
{
	PacketHeader packet;
	std::uint32_t version;
	std::uint32_t slot;
	std::uint32_t save_size;
	std::uint32_t checksum;
};

std::uint32_t SaveChecksum(const BYTE* data, std::size_t size)
{
	std::uint32_t value = 2166136261u;
	for (std::size_t index = 0; index < size; ++index)
	{
		value ^= data[index];
		value *= 16777619u;
	}
	return value;
}

bool GetGameDirectory(wchar_t directory[MAX_PATH])
{
	if (!directory)
		return false;
	directory[0] = L'\0';
	wchar_t executable[MAX_PATH] = {};
	const DWORD length = GetModuleFileNameW(NULL, executable,
		static_cast<DWORD>(_countof(executable)));
	if (length == 0 || length >= _countof(executable))
		return false;
	wchar_t* slash = wcsrchr(executable, L'\\');
	if (!slash)
		return false;
	*slash = L'\0';
	if (lstrlenW(executable) >= MAX_PATH)
		return false;
	lstrcpyW(directory, executable);
	return true;
}

bool BuildSavePath(wchar_t path[MAX_PATH], const wchar_t* file_name)
{
	if (!path || !file_name)
		return false;
	wchar_t directory[MAX_PATH] = {};
	if (!GetGameDirectory(directory))
		return false;
	if (lstrlenW(directory) + 1 + lstrlenW(file_name) >= MAX_PATH)
		return false;
	lstrcpyW(path, directory);
	lstrcatW(path, L"\\");
	lstrcatW(path, file_name);
	return true;
}

bool ReadWholeSave(std::vector<BYTE>& data)
{
	data.clear();
	wchar_t path[MAX_PATH] = {};
	if (!BuildSavePath(path, L"DATA4"))
		return false;
	const HANDLE file = CreateFileW(path, GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
	{
		coop::CoopRuntime::Instance().Log(
			"[save-sync] host DATA4 cannot be opened error=%lu\r\n",
			GetLastError());
		return false;
	}
	const DWORD size = GetFileSize(file, NULL);
	if (size == INVALID_FILE_SIZE || size == 0 || size > kMaxSaveBytes)
	{
		CloseHandle(file);
		coop::CoopRuntime::Instance().Log(
			"[save-sync] host DATA4 has invalid size=%lu\r\n", size);
		return false;
	}
	data.resize(size);
	DWORD read = 0;
	const bool ok = ReadFile(file, data.data(), size, &read, NULL) != FALSE &&
		read == size;
	CloseHandle(file);
	if (!ok)
	{
		data.clear();
		coop::CoopRuntime::Instance().Log(
			"[save-sync] host DATA4 read failed\r\n");
	}
	return ok;
}

bool WriteWholeJoinSave(const BYTE* data, std::uint32_t size)
{
	if (!data || size == 0 || size > kMaxSaveBytes)
		return false;
	wchar_t target[MAX_PATH] = {};
	wchar_t temporary[MAX_PATH] = {};
	if (!BuildSavePath(target, L"DATA4") ||
		!BuildSavePath(temporary, L"DATA4.coop.tmp"))
		return false;

	const HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
	{
		coop::CoopRuntime::Instance().Log(
			"[save-sync] client temporary save cannot be opened error=%lu\r\n",
			GetLastError());
		return false;
	}
	DWORD written = 0;
	const bool wrote = WriteFile(file, data, size, &written, NULL) != FALSE &&
		written == size;
	const bool flushed = wrote && FlushFileBuffers(file) != FALSE;
	CloseHandle(file);
	if (!flushed)
	{
		DeleteFileW(temporary);
		coop::CoopRuntime::Instance().Log(
			"[save-sync] client temporary save write failed\r\n");
		return false;
	}
	if (!MoveFileExW(temporary, target,
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		DeleteFileW(temporary);
		coop::CoopRuntime::Instance().Log(
			"[save-sync] client DATA4 replacement failed error=%lu\r\n",
			GetLastError());
		return false;
	}
	return true;
}
}

namespace coop
{
SaveSync& SaveSync::Instance()
{
	static SaveSync instance;
	return instance;
}

void SaveSync::SendHostJoinSave(CSteamOfflineSocketServer* server,
	std::int32_t connection)
{
	if (!server || !CoopNetGame::Instance().IsHost())
		return;

	std::vector<BYTE> save;
	if (!ReadWholeSave(save))
		return;

	std::vector<BYTE> packet(sizeof(CoopSaveSlotPacketHeader) + save.size());
	CoopSaveSlotPacketHeader* header =
		reinterpret_cast<CoopSaveSlotPacketHeader*>(packet.data());
	ZeroMemory(header, sizeof(*header));
	header->packet.m_PacketID = kCoopPacketSaveSlot;
	header->packet.m_RealSize = static_cast<std::uint32_t>(
		packet.size() - sizeof(PacketHeader));
	header->packet.m_SizeOne = header->packet.m_RealSize;
	header->version = kSavePacketVersion;
	header->slot = kCoopJoinSaveSlot;
	header->save_size = static_cast<std::uint32_t>(save.size());
	header->checksum = SaveChecksum(save.data(), save.size());
	memcpy(packet.data() + sizeof(*header), save.data(), save.size());

	if (server->SendRaw(connection, packet.data(),
		static_cast<std::uint32_t>(packet.size()),
		k_nSteamNetworkingSend_Reliable))
	{
		CoopRuntime::Instance().Log(
			"[save-sync] sent host DATA4 (%u bytes)\r\n",
			header->save_size);
	}
	else
	{
		CoopRuntime::Instance().Log("[save-sync] failed to send host DATA4\r\n");
	}
}

bool SaveSync::OnRemotePacket(const void* data, std::uint32_t size)
{
	if (!data || size < sizeof(CoopSaveSlotPacketHeader))
		return false;
	const CoopSaveSlotPacketHeader* header =
		static_cast<const CoopSaveSlotPacketHeader*>(data);
	if (header->packet.m_PacketID != kCoopPacketSaveSlot)
		return false;

	if (!CoopNetGame::Instance().IsClient() ||
		CoopNetGame::Instance().IsHost() ||
		header->version != kSavePacketVersion ||
		header->slot != kCoopJoinSaveSlot ||
		header->save_size == 0 || header->save_size > kMaxSaveBytes ||
		header->packet.Size() != size ||
		size != sizeof(*header) + header->save_size)
	{
		CoopRuntime::Instance().Log("[save-sync] rejected invalid host save packet\r\n");
		return true;
	}

	const BYTE* save = reinterpret_cast<const BYTE*>(header + 1);
	if (SaveChecksum(save, header->save_size) != header->checksum)
	{
		CoopRuntime::Instance().Log("[save-sync] rejected host DATA4 checksum\r\n");
		return true;
	}
	if (WriteWholeJoinSave(save, header->save_size))
	{
		InterlockedExchange(&m_pending_load, 1);
		CoopRuntime::Instance().Log(
			"[save-sync] received host DATA4 (%u bytes); native load of slot 5 queued\r\n",
			header->save_size);
	}
	return true;
}

void SaveSync::OnMainFrame()
{
	if (InterlockedExchange(&m_pending_load, 0) == 0)
		return;

	if (!CoopNetGame::Instance().IsClient() || CoopNetGame::Instance().IsHost())
		return;

	typedef bool (__thiscall* BeginNativeLoadFn)(void*, std::uint32_t);
	BeginNativeLoadFn begin_native_load =
		reinterpret_cast<BeginNativeLoadFn>(kBeginNativeLoadAddress);
	void* load_save_manager = reinterpret_cast<void*>(kLoadSaveManagerAddress);
	if (begin_native_load(load_save_manager, kCoopJoinSaveSlot))
	{
		CoopRuntime::Instance().Log(
			"[save-sync] entering stock Load Game for DATA4 (slot 5)\r\n");
	}
	else
	{
		CoopRuntime::Instance().Log(
			"[save-sync] stock Load Game rejected DATA4\r\n");
	}
}
}
