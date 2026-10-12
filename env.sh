# Entorno para compilar Marlin con PlatformIO (ejecutar: source env.sh)
HELLBOT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# PlatformIO instalado localmente (sin tocar el sistema)
export PYTHONPATH="$HELLBOT_ROOT/.tools/local/local/lib/python3.13/dist-packages"
export PATH="$HELLBOT_ROOT/.tools/local/local/bin:$PATH"

# Todo el estado de PlatformIO dentro del workspace
export PLATFORMIO_CORE_DIR="$HELLBOT_ROOT/.pio-home"
export PLATFORMIO_WORKSPACE_DIR="$HELLBOT_ROOT/marlin/.pio"
export PLATFORMIO_PACKAGE_DIR="$PLATFORMIO_WORKSPACE_DIR/packages"
export PLATFORMIO_PLATFORMS_DIR="$PLATFORMIO_WORKSPACE_DIR/platforms"

# Accesos rápidos
alias pio-build='cd "$HELLBOT_ROOT/marlin" && pio run -e mks_robin_nano_v1v2'
alias pio-clean='cd "$HELLBOT_ROOT/marlin" && pio run -e mks_robin_nano_v1v2 -t clean'
