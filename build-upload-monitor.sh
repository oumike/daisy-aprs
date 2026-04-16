#!/bin/bash

set -euo pipefail

ENV_NAME="heltec-v4-tft"
DEBUG_ENV_NAME="heltec-v4-tft-debug"
ERASE_FIRST=false

for arg in "$@"; do
	case "$arg" in
		--debug|-d)
			ENV_NAME="$DEBUG_ENV_NAME"
			;;
		--erase|-E)
			ERASE_FIRST=true
			;;
		--help|-h)
			echo "Usage: $0 [--debug|-d] [--erase|-E]"
			echo "  --debug, -d   Use debug PlatformIO environment ($DEBUG_ENV_NAME)"
			echo "  --erase, -E   Erase flash before clean build/upload"
			exit 0
			;;
		*)
			echo "Unknown argument: $arg"
			echo "Use --help for usage."
			exit 1
			;;
	esac
done

if [ "$ERASE_FIRST" = true ]; then
	echo "[PIO] Erasing device flash..."
	pio run -e "$ENV_NAME" -t erase
fi

pio run -e "$ENV_NAME" -t fullclean
pio run -e "$ENV_NAME" -t upload
pio run -e "$ENV_NAME" -t monitor
