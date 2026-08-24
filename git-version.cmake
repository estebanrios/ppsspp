set(GIT_VERSION_FILE "${OUTPUT_DIR}/git-version.cpp")
set(GIT_VERSION "unknown")
set(GIT_VERSION_UPDATE "1")

find_package(Git)
# STV: version pineada. La rama stv/1.20.4 lleva commits encima del tag y el
# describe daria "v1.20.4-N-g..." contaminando el binario; la prueba de
# identidad byte a byte del paso 0 de F6 exige la cadena exacta del build de
# produccion. La identidad real de cada build viaja en el versionName del APK
# y en los strings de marca STV_*.
set(GIT_VERSION "v1.20.4")
if(FALSE AND GIT_FOUND AND EXISTS "${SOURCE_DIR}/.git/")
	execute_process(COMMAND ${GIT_EXECUTABLE} describe --always
		WORKING_DIRECTORY ${SOURCE_DIR}
		RESULT_VARIABLE exit_code
		OUTPUT_VARIABLE GIT_VERSION)
	if(NOT ${exit_code} EQUAL 0)
		message(WARNING "git describe failed, unable to include version.")
	endif()
	string(STRIP ${GIT_VERSION} GIT_VERSION)
else()
	message(WARNING "git not found, unable to include version.")
endif()

if(EXISTS ${GIT_VERSION_FILE})
	# Don't update if marked not to update.
	file(STRINGS ${GIT_VERSION_FILE} match
		REGEX "PPSSPP_GIT_VERSION_NO_UPDATE 1")
	if(NOT ${match} EQUAL "")
		set(GIT_VERSION_UPDATE "0")
	endif()

	# Let's also skip if it's the same.
	string(REPLACE "." "\\." GIT_VERSION_ESCAPED ${GIT_VERSION})
	file(STRINGS ${GIT_VERSION_FILE} match
		REGEX "PPSSPP_GIT_VERSION = \"${GIT_VERSION_ESCAPED}\";")
	if(NOT ${match} EQUAL "")
		set(GIT_VERSION_UPDATE "0")
	endif()
endif()

set(code_string "// This is a generated file.\n\n"
	"const char *PPSSPP_GIT_VERSION = \"${GIT_VERSION}\"\;\n\n"
	"// If you don't want this file to update/recompile, change to 1.\n"
	"#define PPSSPP_GIT_VERSION_NO_UPDATE 0\n")

if ("${GIT_VERSION_UPDATE}" EQUAL "1")
	file(WRITE ${GIT_VERSION_FILE} ${code_string})
endif()
