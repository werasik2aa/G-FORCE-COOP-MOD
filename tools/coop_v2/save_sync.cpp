#include "save_sync.h"

#include "coop_netgame.h"
#include "coop_runtime.h"
#include "gforce_constants.h"
#include "world_sync.h"
#include "ServerClient/MServer.h"
#include "ServerClient/MServerONLINE.h"
#include "ServerClient/MTypes.h"

#include <string.h>
#include <vector>

namespace
{
	constexpr std::uint32_t kSavePacketVersion = 1;
	constexpr std::uint32_t kMaxSaveBytes = 128 * 1024;

	struct CoopSaveSlotPacketHeader
	{
		PacketHeader packet;
		std::uint32_t version;
		std::uint32_t slot;
		std::uint32_t save_size;
		std::uint32_t checksum;
	};
	static_assert(sizeof(CoopSaveSlotPacketHeader) == 36,
		"save packet header must remain a 36-byte wire record");

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

	bool GetSaveFileName(std::uint32_t slot, const wchar_t*& file_name)
	{
		static const wchar_t* const file_names[] = {
			L"DATA0", L"DATA1", L"DATA2", L"DATA3", L"DATA4"
		};
		static_assert(_countof(file_names) == coop::gforce::kVisibleSaveSlotCount,
			"file names must cover every native visible save slot");
		if (slot >= _countof(file_names))
			return false;
		file_name = file_names[slot];
		return true;
	}

	bool VerifyNativeLoadPath()
	{
		using namespace coop::gforce;
		if (memcmp(reinterpret_cast<const void*>(kBeginNativeSaveLoad),
			kExpectedBeginNativeSaveLoad,
			sizeof(kExpectedBeginNativeSaveLoad)) == 0)
		{
			return true;
		}
		coop::CoopRuntime::Instance().Log(
			"[save-sync] native load fingerprint mismatch\r\n");
		return false;
	}

	bool ReadHostSelectedSlot(std::uint32_t& slot)
	{
		using namespace coop::gforce;
		if (!VerifyNativeLoadPath())
			return false;
		__try
		{
			slot = *reinterpret_cast<const volatile std::uint32_t*>(
				kLoadSaveManager + kLoadSaveSelectedSlotOffset);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			coop::CoopRuntime::Instance().Log(
				"[save-sync] cannot read host selected slot\r\n");
			return false;
		}
		if (slot >= kVisibleSaveSlotCount)
		{
			coop::CoopRuntime::Instance().Log(
				"[save-sync] host selected slot is invalid: %u\r\n", slot);
			return false;
		}
		return true;
	}

	bool ReadWholeSave(std::uint32_t slot, std::vector<BYTE>& data)
	{
		data.clear();
		const wchar_t* file_name = NULL;
		if (!GetSaveFileName(slot, file_name))
			return false;
		wchar_t path[MAX_PATH] = {};
		if (!BuildSavePath(path, file_name))
			return false;
		const HANDLE file = CreateFileW(path, GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, NULL);
		if (file == INVALID_HANDLE_VALUE)
		{
			coop::CoopRuntime::Instance().Log(
				"[save-sync] host DATA%u cannot be opened error=%lu\r\n", slot,
				GetLastError());
			return false;
		}
		const DWORD size = GetFileSize(file, NULL);
		if (size == INVALID_FILE_SIZE || size == 0 || size > kMaxSaveBytes)
		{
			CloseHandle(file);
			coop::CoopRuntime::Instance().Log(
				"[save-sync] host DATA%u has invalid size=%lu\r\n", slot, size);
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
				"[save-sync] host DATA%u read failed\r\n", slot);
		}
		return ok;
	}

	bool WriteWholeJoinSave(std::uint32_t slot, const BYTE* data,
		std::uint32_t size)
	{
		if (!data || size == 0 || size > kMaxSaveBytes)
			return false;
		const wchar_t* file_name = NULL;
		if (!GetSaveFileName(slot, file_name))
			return false;
		wchar_t temporary_file_name[16] = {};
		lstrcpynW(temporary_file_name, file_name,
			static_cast<int>(_countof(temporary_file_name)));
		lstrcatW(temporary_file_name, L".coop.tmp");
		wchar_t target[MAX_PATH] = {};
		wchar_t temporary[MAX_PATH] = {};
		if (!BuildSavePath(target, file_name) ||
			!BuildSavePath(temporary, temporary_file_name))
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
				"[save-sync] client DATA%u replacement failed error=%lu\r\n", slot,
				GetLastError());
			return false;
		}
		return true;
	}

	bool SendSaveToServer(CSteamOfflineSocketServer* server,
		std::int32_t connection, std::uint32_t slot)
	{
		if (!server || !server->IsSteamSocketOpen() ||
			slot >= coop::gforce::kVisibleSaveSlotCount)
			return false;

		std::vector<BYTE> save;
		if (!ReadWholeSave(slot, save))
			return false;

		CoopSaveSlotPacketHeader header = {};
		header.packet.m_PacketID = kCoopPacketSaveSlot;
		header.packet.m_RealSize = static_cast<std::uint32_t>(
			sizeof(header) - sizeof(PacketHeader) + save.size());
		header.packet.m_SizeOne = header.packet.m_RealSize;
		header.slot = slot;
		header.version = kSavePacketVersion;
		header.save_size = static_cast<std::uint32_t>(save.size());
		header.checksum = SaveChecksum(save.data(), save.size());

		std::vector<BYTE> packet(sizeof(header) + save.size());
		memcpy(packet.data(), &header, sizeof(header));
		memcpy(packet.data() + sizeof(header), save.data(), save.size());
		if (!server->SendRaw(connection, packet.data(),
			static_cast<std::uint32_t>(packet.size()),
			k_nSteamNetworkingSend_Reliable))
		{
			coop::CoopRuntime::Instance().Log(
				"[save-sync] failed to send host DATA%u\r\n", slot);
			return false;
		}
		coop::CoopRuntime::Instance().Log(
			"[save-sync] sent host DATA%u (slot %u, %u bytes)\r\n",
			slot, slot, header.save_size);
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

	void SaveSync::CaptureLoadedHostSlot()
	{
		if (CoopNetGame::Instance().IsClient())
			return;
		std::uint32_t slot = 0;
		if (!ReadHostSelectedSlot(slot))
			return;
		const long previous = InterlockedExchange(&m_host_slot,
			static_cast<long>(slot));
		if (previous != static_cast<long>(slot))
		{
			CoopRuntime::Instance().Log(
				"[save-sync] captured loaded host DATA%u (slot %u)\r\n", slot, slot);
		}
	}

	void SaveSync::OnHostLoadGame(std::uint32_t slot)
	{
		if (!CoopNetGame::Instance().IsHost() ||
			!CoopNetGame::Instance().HasRemotePeer() ||
			slot >= gforce::kVisibleSaveSlotCount)
			return;

		CSteamOfflineSocketServer* servers[] = { SteamOServer, SteamSServer };
		for (CSteamOfflineSocketServer* server : servers)
		{
			if (!server || !server->IsSteamSocketOpen())
				continue;
			for (const HSteamNetConnection connection : server->GetPlayers())
				SendSaveToServer(server, connection, slot);
		}

		// The native loader invalidates every world pointer. Clear both the host
		// registry and Player2 cached native objects before it starts destroying
		// the current level.
		WorldSync::Instance().ResetForWorldLoad();
	}

	void SaveSync::SendHostJoinSave(CSteamOfflineSocketServer* server,
		std::int32_t connection)
	{
		if (!server || !CoopNetGame::Instance().IsHost())
			return;

		const long captured_slot = InterlockedCompareExchange(&m_host_slot, 0, 0);
		if (captured_slot < 0 || static_cast<std::uint32_t>(captured_slot) >=
			gforce::kVisibleSaveSlotCount)
		{
			CoopRuntime::Instance().Log(
				"[save-sync] host join save skipped: no loaded slot captured\r\n");
			return;
		}
		const std::uint32_t slot = static_cast<std::uint32_t>(captured_slot);
		std::vector<BYTE> save;
		if (!ReadWholeSave(slot, save))
			return;

		CoopSaveSlotPacketHeader header = {};
		header.packet.m_PacketID = kCoopPacketSaveSlot;
		header.packet.m_RealSize = static_cast<std::uint32_t>(
		sizeof(header) - sizeof(PacketHeader) + save.size());
		header.packet.m_SizeOne = header.packet.m_RealSize;
		header.version = kSavePacketVersion;
		header.slot = slot;
		header.save_size = static_cast<std::uint32_t>(save.size());
		header.checksum = SaveChecksum(save.data(), save.size());
		std::vector<BYTE> packet(sizeof(header) + save.size());
		memcpy(packet.data(), &header, sizeof(header));
		memcpy(packet.data() + sizeof(header), save.data(), save.size());

		if (server->SendRaw(connection, packet.data(),
			static_cast<std::uint32_t>(packet.size()),
			k_nSteamNetworkingSend_Reliable))
		{
			CoopRuntime::Instance().Log(
				"[save-sync] sent host DATA%u (slot %u, %u bytes)\r\n", slot, slot,
				header.save_size);
		}
		else
		{
			CoopRuntime::Instance().Log(
				"[save-sync] failed to send host DATA%u\r\n", slot);
		}
	}

	bool SaveSync::OnRemotePacket(const void* data, std::uint32_t size)
	{
		if (!data || size < sizeof(CoopSaveSlotPacketHeader))
			return false;
		CoopSaveSlotPacketHeader header = {};
		memcpy(&header, data, sizeof(header));
		if (header.packet.m_PacketID != kCoopPacketSaveSlot)
			return false;

		if (!CoopNetGame::Instance().IsClient() ||
			CoopNetGame::Instance().IsHost() ||
			header.version != kSavePacketVersion ||
			header.slot >= gforce::kVisibleSaveSlotCount ||
			header.save_size == 0 || header.save_size > kMaxSaveBytes ||
			header.packet.Size() != size ||
			size != sizeof(header) + header.save_size)
		{
			CoopRuntime::Instance().Log("[save-sync] rejected invalid host save packet\r\n");
			return true;
		}

		const BYTE* save = static_cast<const BYTE*>(data) + sizeof(header);
		if (SaveChecksum(save, header.save_size) != header.checksum)
		{
			CoopRuntime::Instance().Log(
				"[save-sync] rejected host DATA%u checksum\r\n", header.slot);
			return true;
		}
		if (WriteWholeJoinSave(header.slot, save, header.save_size))
		{
			InterlockedExchange(&m_pending_slot,
				static_cast<long>(header.slot));
			InterlockedExchange(&m_pending_load, 1);
			CoopRuntime::Instance().Log(
				"[save-sync] received host DATA%u (%u bytes); native load queued\r\n",
				header.slot, header.save_size);
		}
		return true;
	}

	bool SaveSync::OnMainFrame()
	{
		if (InterlockedExchange(&m_pending_load, 0) == 0)
			return false;

		if (!CoopNetGame::Instance().IsClient() || CoopNetGame::Instance().IsHost())
			return false;

		const long pending_slot = InterlockedExchange(&m_pending_slot, -1);
		if (pending_slot < 0 || static_cast<std::uint32_t>(pending_slot) >=
			gforce::kVisibleSaveSlotCount || !VerifyNativeLoadPath())
		{
			CoopRuntime::Instance().Log("[save-sync] queued host slot is invalid\r\n");
			return false;
		}

		typedef bool(__thiscall* BeginNativeLoadFn)(void*, std::uint32_t);
		BeginNativeLoadFn begin_native_load =
			reinterpret_cast<BeginNativeLoadFn>(gforce::kBeginNativeSaveLoad);
		void* load_save_manager = reinterpret_cast<void*>(gforce::kLoadSaveManager);
		// All local entity pointers become invalid during the stock load.  Clear the
		// client match table before the loader starts constructing the next world.
		WorldSync::Instance().ResetForWorldLoad();
		bool loaded = false;
		__try
		{
			loaded = begin_native_load(load_save_manager,
				static_cast<std::uint32_t>(pending_slot));
		}
		__except (CoopRuntime::Instance().LogException(
			GetExceptionInformation(), "client-native-load"))
		{
			loaded = false;
		}
		if (loaded)
		{
			CoopRuntime::Instance().Log(
				"[save-sync] entering stock Load Game for DATA%u (slot %u)\r\n",
				static_cast<unsigned>(pending_slot),
				static_cast<unsigned>(pending_slot));
		}
		else
		{
			CoopRuntime::Instance().Log(
				"[save-sync] stock Load Game rejected DATA%u\r\n",
				static_cast<unsigned>(pending_slot));
		}
		return loaded;
	}
}
