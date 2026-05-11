#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

HELTEC_ENV_NAME="heltec-v4-tft"
HELTEC_DEBUG_ENV_NAME="heltec-v4-tft-debug"
DEBUG_BUILD=false
ENV_NAME=""
ERASE_FIRST=false

has_env() {
	local env_name="$1"
	grep -q "^\[env:${env_name}\]" platformio.ini
}

show_usage() {
	echo "Usage: $0 [--debug|-d] [--erase|-E]"
	echo "  --debug, -d   Use debug variant for selected device"
	echo "  --erase, -E   Erase flash before clean build/upload"
}

for arg in "$@"; do
	case "$arg" in
		--debug|-d)
			DEBUG_BUILD=true
			;;
		--erase|-E)
			ERASE_FIRST=true
			;;
		--help|-h)
			show_usage
			exit 0
			;;
		*)
			echo "Unknown argument: $arg"
			show_usage
			exit 1
			;;
	esac
done

if [ "$DEBUG_BUILD" = true ]; then
	ENV_NAME="$HELTEC_DEBUG_ENV_NAME"
else
	ENV_NAME="$HELTEC_ENV_NAME"
fi

if ! has_env "$ENV_NAME"; then
	echo "Environment '$ENV_NAME' not found in platformio.ini"
	exit 1
fi

echo "[PIO] Using environment: $ENV_NAME"

if [ "$ERASE_FIRST" = true ]; then
	echo "[PIO] Erasing device flash..."
	pio run -e "$ENV_NAME" -t erase
fi

echo "[PIO] Full clean ($ENV_NAME)..."
pio run -e "$ENV_NAME" -t fullclean

echo "[PIO] Upload ($ENV_NAME)..."
pio run -e "$ENV_NAME" -t upload

echo "[PIO] Monitor ($ENV_NAME)..."
pio run -e "$ENV_NAME" -t monitor
