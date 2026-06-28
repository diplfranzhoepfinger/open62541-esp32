# Override install() and export() for the open62541 ExternalProject build.
# We only need the amalgamation, not a proper CMake install.
macro(install)
endmacro()
macro(export)
endmacro()
