/*
 * RfUdrl - runtime setup module.
 *
 * This is the part Tradecraft Garden's simple_rdll does not do. Without it a
 * reflectively loaded image is left RWX for its whole life, its TLS callbacks
 * never run, and the OS cannot unwind through it because the image exception
 * directory was never registered.
 *
 * It is wired in with redirect "ProcessImports" "rdll_process_imports" in
 * loader.spec, so it applies to any caller of ProcessImports.
 */

#include <windows.h>
#include "tcg.h"
#include "rdllsetup.h"

WINBASEAPI WINBOOL WINAPI KERNEL32$VirtualProtect(LPVOID lpAddress, SIZE_T dwSize, DWORD flNewProtect, PDWORD lpflOldProtect);
WINBASEAPI BOOLEAN NTAPI  NTDLL$RtlAddFunctionTable(PRUNTIME_FUNCTION FunctionTable, DWORD EntryCount, DWORD64 BaseAddress);

typedef void (NTAPI * RDLL_TLS_CALLBACK)(PVOID, DWORD, PVOID);

static IMAGE_SECTION_HEADER * rdll_first_section(DLLDATA * dll) {
	return (IMAGE_SECTION_HEADER *)PTR_OFFSET(dll->OptionalHeader, dll->NtHeaders->FileHeader.SizeOfOptionalHeader);
}

/*
 * Register the exception directory (.pdata) for this image so the OS can walk
 * stack frames inside it. Without this every frame shows as an unresolved
 * address, which is both a functional and a visibility problem.
 */
static void rdll_register_unwind(DLLDATA * dll, char * base) {
	IMAGE_DATA_DIRECTORY * dir     = NULL;
	RUNTIME_FUNCTION     * table   = NULL;
	DWORD                  entries = 0;

	dir = GetDataDirectory(dll, IMAGE_DIRECTORY_ENTRY_EXCEPTION);

	if (dir->VirtualAddress == 0 || dir->Size < sizeof(RUNTIME_FUNCTION))
		return;

	table   = (RUNTIME_FUNCTION *)(base + dir->VirtualAddress);
	entries = dir->Size / sizeof(RUNTIME_FUNCTION);

	NTDLL$RtlAddFunctionTable(table, entries, (DWORD64)base);
}

/*
 * Run the image TLS callbacks. AddressOfCallBacks is an absolute VA, rebased for
 * us by ProcessRelocations inside LoadDLL. It is range checked anyway, so a
 * malformed or relocation-stripped image cannot send us off into nowhere.
 */
static void rdll_run_tls_callbacks(DLLDATA * dll, char * base, DWORD reason) {
	IMAGE_DATA_DIRECTORY * dir = NULL;
	IMAGE_TLS_DIRECTORY  * tls = NULL;
	RDLL_TLS_CALLBACK    * cb  = NULL;
	char                 * end = NULL;

	dir = GetDataDirectory(dll, IMAGE_DIRECTORY_ENTRY_TLS);

	if (dir->VirtualAddress == 0 || dir->Size < sizeof(IMAGE_TLS_DIRECTORY))
		return;

	tls = (IMAGE_TLS_DIRECTORY *)(base + dir->VirtualAddress);
	cb  = (RDLL_TLS_CALLBACK *)tls->AddressOfCallBacks;
	end = base + SizeOfDLL(dll);

	if ((char *)cb < base || (char *)cb >= end)
		return;

	while (*cb != NULL) {
		(*cb)((PVOID)base, reason, NULL);
		cb++;
	}
}

/*
 * Give every section the protection its characteristics ask for, and make the
 * header page read only the way a real image is. The allocation starts out RWX.
 */
static void rdll_protect_sections(DLLDATA * dll, char * base) {
	IMAGE_SECTION_HEADER * section = NULL;
	DWORD                  old     = 0;
	DWORD                  protect = 0;
	WORD                   count   = 0;
	WORD                   i       = 0;

	if (dll->OptionalHeader->SizeOfHeaders != 0)
		KERNEL32$VirtualProtect(base, dll->OptionalHeader->SizeOfHeaders, PAGE_READONLY, &old);

	count   = dll->NtHeaders->FileHeader.NumberOfSections;
	section = rdll_first_section(dll);

	for (i = 0; i < count; i++) {
		if (section[i].Misc.VirtualSize == 0)
			continue;

		if ((section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) {
			protect = ((section[i].Characteristics & IMAGE_SCN_MEM_WRITE) != 0)
				? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
		} else {
			protect = ((section[i].Characteristics & IMAGE_SCN_MEM_WRITE) != 0)
				? PAGE_READWRITE : PAGE_READONLY;
		}

		KERNEL32$VirtualProtect(base + section[i].VirtualAddress, section[i].Misc.VirtualSize, protect, &old);
	}
}

void rdll_runtime_setup(DLLDATA * dll, char * base) {
	rdll_register_unwind(dll, base);
	rdll_run_tls_callbacks(dll, base, DLL_PROCESS_ATTACH);
	rdll_protect_sections(dll, base);
}

/*
 * The redirect target. preserve "ProcessImports" "rdll_process_imports" in
 * loader.spec is what keeps the call below pointing at the real ProcessImports
 * instead of recursing back here.
 */
void rdll_process_imports(IMPORTFUNCS * funcs, DLLDATA * dll, char * base) {
	ProcessImports(funcs, dll, base);
	rdll_runtime_setup(dll, base);
}