# Compare an installed ELF with its built owner without editing either file.
# The caller supplies the final target INSTALL_RPATH and toolchain readelf;
# private copies account only for CMake's normal install-time RPATH editing.
function(correctness_verify_elf_runpath installed expected readelf)
	execute_process(COMMAND "${CMAKE_COMMAND}" -E env LC_ALL=C
		"${readelf}" -dW "${installed}"
		RESULT_VARIABLE status OUTPUT_VARIABLE dynamic ERROR_VARIABLE error)
	if(NOT status STREQUAL "0")
		message(FATAL_ERROR "Cannot inspect installed ELF dynamic paths: ${installed}: ${error}")
	endif()
	string(REGEX MATCHALL "\\((RPATH|RUNPATH)\\)[^\n]*" paths "${dynamic}")
	list(LENGTH paths count)
	if(expected STREQUAL "")
		if(NOT count EQUAL 0)
			message(FATAL_ERROR "Installed ELF has a dynamic path but target INSTALL_RPATH is empty: ${installed}")
		endif()
	else()
		if(NOT count EQUAL 1)
			message(FATAL_ERROR "Installed ELF must have exactly one declared RUNPATH: ${installed}")
		endif()
		list(GET paths 0 path)
		if(NOT path MATCHES "^\\(RUNPATH\\)[^[]*\\[([^]]*)\\][ \t\r]*$")
			message(FATAL_ERROR "Installed ELF dynamic path must be RUNPATH, not RPATH: ${installed}")
		endif()
		if(NOT CMAKE_MATCH_1 STREQUAL expected)
			message(FATAL_ERROR "Installed ELF RUNPATH differs from declared target INSTALL_RPATH: ${installed}")
		endif()
	endif()
endfunction()

function(correctness_compare_elf built installed expected readelf private)
	# Validate the actual installed owner before normalization. Otherwise an
	# unexpected loader path could disappear from the comparison unnoticed.
	correctness_verify_elf_runpath("${installed}" "${expected}" "${readelf}")
	# This helper owns only a fresh, explicitly named scratch directory. Never
	# recursively remove a pre-existing caller directory after a successful compare.
	if(NOT IS_ABSOLUTE "${private}" OR EXISTS "${private}")
		message(FATAL_ERROR "ELF comparison requires a fresh absolute private directory: ${private}")
	endif()
	file(MAKE_DIRECTORY "${private}")
	file(REAL_PATH "${private}" verified_private)
	get_filename_component(private_name "${private}" NAME)
	get_filename_component(private_parent "${private}" DIRECTORY)
	file(REAL_PATH "${private_parent}" verified_parent)
	if(NOT verified_private STREQUAL "${verified_parent}/${private_name}")
		message(FATAL_ERROR "ELF comparison private directory was redirected: ${private}")
	endif()
	file(COPY_FILE "${built}" "${private}/built")
	file(COPY_FILE "${installed}" "${private}/installed")
	# CMake's own ELF editor removes only the dynamic RPATH/RUNPATH entry and
	# its string storage. Originals are never edited; every other byte must agree.
	file(RPATH_REMOVE FILE "${private}/built")
	file(RPATH_REMOVE FILE "${private}/installed")
	file(SHA256 "${private}/built" built_hash)
	file(SHA256 "${private}/installed" installed_hash)
	if(NOT built_hash STREQUAL installed_hash)
		message(FATAL_ERROR "Installed ELF differs from built target outside RPATH: ${installed}")
	endif()
	# Originals and their raw hashes remain untouched. Retain failed comparisons
	# for diagnosis, but release these exact owned copies after successful proof.
	file(REMOVE_RECURSE "${verified_private}")
endfunction()
