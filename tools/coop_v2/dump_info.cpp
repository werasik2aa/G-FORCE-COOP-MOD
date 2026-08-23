#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>

#pragma comment(lib, "dbghelp.lib")

const BYTE* FindDumpMemory(void* view, ULONG64 address, ULONG size)
{
	PMINIDUMP_MEMORY64_LIST memory64 = NULL;
	ULONG stream_size = 0;
	if (MiniDumpReadDumpStream(view, Memory64ListStream, NULL,
		reinterpret_cast<void**>(&memory64), &stream_size) && memory64)
	{
		ULONG64 rva = memory64->BaseRva;
		for (ULONG64 i = 0; i < memory64->NumberOfMemoryRanges; ++i)
		{
			const MINIDUMP_MEMORY_DESCRIPTOR64& range = memory64->MemoryRanges[i];
			if (address >= range.StartOfMemoryRange &&
				address + size <= range.StartOfMemoryRange + range.DataSize)
			{
				return reinterpret_cast<const BYTE*>(view) + rva +
					(address - range.StartOfMemoryRange);
			}
			rva += range.DataSize;
		}
	}

	PMINIDUMP_MEMORY_LIST memory = NULL;
	if (MiniDumpReadDumpStream(view, MemoryListStream, NULL,
		reinterpret_cast<void**>(&memory), &stream_size) && memory)
	{
		for (ULONG i = 0; i < memory->NumberOfMemoryRanges; ++i)
		{
			const MINIDUMP_MEMORY_DESCRIPTOR& range = memory->MemoryRanges[i];
			if (address >= range.StartOfMemoryRange &&
				address + size <= range.StartOfMemoryRange + range.Memory.DataSize)
			{
				return reinterpret_cast<const BYTE*>(view) + range.Memory.Rva +
					(address - range.StartOfMemoryRange);
			}
		}
	}
	return NULL;
}

int wmain(int argc, wchar_t** argv)
{
	if (argc != 2)
	{
		fwprintf(stderr, L"usage: dump_info <file.dmp>\n");
		return 2;
	}

	HANDLE file = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return 3;
	HANDLE mapping = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
	if (!mapping)
	{
		CloseHandle(file);
		return 4;
	}
	void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
	if (!view)
	{
		CloseHandle(mapping);
		CloseHandle(file);
		return 5;
	}

	PMINIDUMP_EXCEPTION_STREAM exception = NULL;
	ULONG stream_size = 0;
	if (MiniDumpReadDumpStream(view, ExceptionStream, NULL,
		reinterpret_cast<void**>(&exception), &stream_size) && exception)
	{
		wprintf(L"exception_code=0x%08X exception_address=0x%016llX thread=%u\n",
			exception->ExceptionRecord.ExceptionCode,
			exception->ExceptionRecord.ExceptionAddress,
			exception->ThreadId);
		if (exception->ThreadContext.Rva &&
			exception->ThreadContext.DataSize >= sizeof(WOW64_CONTEXT))
		{
			const WOW64_CONTEXT* context = reinterpret_cast<const WOW64_CONTEXT*>(
				reinterpret_cast<const BYTE*>(view) + exception->ThreadContext.Rva);
			wprintf(L"eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X ebp=%08X esp=%08X eip=%08X\n",
				context->Eax, context->Ebx, context->Ecx, context->Edx,
				context->Esi, context->Edi, context->Ebp, context->Esp,
				context->Eip);
			const DWORD stack_bytes = 0x200;
			const DWORD* stack = reinterpret_cast<const DWORD*>(
				FindDumpMemory(view, context->Esp, stack_bytes));
			if (stack)
			{
				wprintf(L"probable_game_return_addresses:\n");
				for (DWORD i = 0; i < stack_bytes / sizeof(DWORD); ++i)
				{
					if (stack[i] >= 0x00400000 && stack[i] < 0x00AE4000)
						wprintf(L"  esp+0x%03X = %08X\n", i * 4, stack[i]);
				}
			}
		}
	}

	PMINIDUMP_MODULE_LIST modules = NULL;
	if (MiniDumpReadDumpStream(view, ModuleListStream, NULL,
		reinterpret_cast<void**>(&modules), &stream_size) && modules && exception)
	{
		const ULONG64 address = exception->ExceptionRecord.ExceptionAddress;
		for (ULONG i = 0; i < modules->NumberOfModules; ++i)
		{
			const MINIDUMP_MODULE& module = modules->Modules[i];
			if (address < module.BaseOfImage ||
				address >= module.BaseOfImage + module.SizeOfImage)
				continue;
			const MINIDUMP_STRING* name = reinterpret_cast<const MINIDUMP_STRING*>(
				reinterpret_cast<const BYTE*>(view) + module.ModuleNameRva);
			const int characters = name ? static_cast<int>(name->Length / sizeof(wchar_t)) : 0;
			wprintf(L"module=%.*s base=0x%016llX rva=0x%llX size=0x%X\n",
				characters, name ? name->Buffer : L"", module.BaseOfImage,
				address - module.BaseOfImage, module.SizeOfImage);
			break;
		}
	}

	UnmapViewOfFile(view);
	CloseHandle(mapping);
	CloseHandle(file);
	return 0;
}
