#
# RfUdrl - reflective DLL loader built on Crystal Palace
#
name        "RfUdrl"
describe    "Reflective DLL loader with runtime setup and an NtContinue entry"

x64:
	# our loader; +gofirst puts go() at offset 0 of the PIC
	load "bin/loader.x64.o"
		make pic +gofirst

		# our runtime setup module (TLS, .pdata, per-section protection)
		load "bin/rdllsetup.x64.o"
			merge

		# our entry transfer module (synthetic stack + NtContinue)
		load "bin/transfer.x64.o"
			merge

		# PIC bootstrap services: fixbss (globals), dfr, resolvers
		run "../simple_pic/services.spec"

		# the Tradecraft Garden helper library
		mergelib "../libtcg/libtcg.x64.zip"

		# weave the runtime setup in after imports are handled, so every caller
		# of ProcessImports gets it and not just our own go().
		# preserve keeps the call inside rdll_process_imports going to the real
		# ProcessImports, otherwise the redirect would recurse.
		redirect "ProcessImports" "rdll_process_imports"
		preserve "ProcessImports" "rdll_process_imports"

		# the DLL to load, linked into this PIC as the my_data section
		push $DLL
			link "my_data"

		export