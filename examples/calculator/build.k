project "calculator" version="0.1.0" {
    include "Console"
    include "Arithmetic"

    target "executable" entry="Console/src/main.co"
    target "library" name="arithmetic-lib" module="Arithmetic"
}
