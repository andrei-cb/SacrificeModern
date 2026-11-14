# Resolve VS-bundled vcpkg when opening the folder directly in Visual Studio.
if(NOT DEFINED ENV{VCPKG_ROOT} OR "$ENV{VCPKG_ROOT}" STREQUAL "")
  find_program(SACRIFICE_VSWHERE vswhere.exe
    PATHS "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer" NO_DEFAULT_PATH)
  if(NOT SACRIFICE_VSWHERE)
    message(FATAL_ERROR "Set VCPKG_ROOT or configure through build.ps1")
  endif()
  execute_process(COMMAND "${SACRIFICE_VSWHERE}" -latest -products *
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    OUTPUT_VARIABLE SACRIFICE_VS_PATH OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
  set(ENV{VCPKG_ROOT} "${SACRIFICE_VS_PATH}/VC/vcpkg")
endif()
include("$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
