#pragma once
#include "tcg.h"

/*
 * Apply the runtime setup a normally loaded DLL gets.
 *
 * Call after the image is mapped, relocated and has its imports resolved.
 * Performs, in this order (matching what a real load does):
 *   1. register the image exception directory with RtlAddFunctionTable
 *   2. run the TLS callbacks with reason
 *   3. apply per-section page protection and make the header page read only
 */
void rdll_runtime_setup(DLLDATA * dll, char * base);

/*
 * ProcessImports replacement that also applies the runtime setup.
 *
 * loader.spec redirects ProcessImports to this, with preserve so the call below
 * still reaches the real one. The redirect is what makes the setup apply to any
 * caller of ProcessImports rather than only to our own go().
 *
 * The hook deliberately wraps ProcessImports and not LoadDLL. Order matters: the
 * TLS callbacks run the image's own code, which may call imported APIs, so the
 * imports have to be resolved first. Wrapping LoadDLL would run the callbacks
 * before that and break any image whose TLS callback uses an import.
 */
void rdll_process_imports(IMPORTFUNCS * funcs, DLLDATA * dll, char * base);