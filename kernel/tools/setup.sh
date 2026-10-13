#!/bin/sh
set -eu

GKI_ROOT=$(pwd)
REPO_URL="https://github.com/Rem01Gaming/encore_fas"
MODULE_NAME="encore_fas"
MODULE_SUBDIR="kernel"
SRC_DIR="$GKI_ROOT/${MODULE_NAME}"

display_usage() {
	echo "Usage: $0 [--cleanup | <commit-or-tag>]"
	echo "  --cleanup:              Removes what this script previously set up."
	echo "  <commit-or-tag>:        Sets up or updates encore_fas to this commit or tag."
	echo "  -h, --help:             Shows this message."
	echo "  (no args):              Sets up or updates encore_fas to the latest tag."
}

initialize_variables() {
	if test -d "$GKI_ROOT/common/drivers"; then
		DRIVER_DIR="$GKI_ROOT/common/drivers"
	elif test -d "$GKI_ROOT/drivers"; then
		DRIVER_DIR="$GKI_ROOT/drivers"
	else
		echo '[ERROR] "drivers/" directory not found. Run this from a kernel source tree root.'
		exit 127
	fi

	DRIVER_MAKEFILE="$DRIVER_DIR/Makefile"
	DRIVER_KCONFIG="$DRIVER_DIR/Kconfig"
}

perform_cleanup() {
	echo "[+] Cleaning up..."

	if [ -L "$DRIVER_DIR/$MODULE_NAME" ]; then
		rm "$DRIVER_DIR/$MODULE_NAME"
		echo "[-] Symlink removed."
	fi

	if grep -q "$MODULE_NAME" "$DRIVER_MAKEFILE"; then
		sed -i "/$MODULE_NAME/d" "$DRIVER_MAKEFILE"
		echo "[-] Makefile reverted."
	fi

	if grep -q "drivers/$MODULE_NAME/Kconfig" "$DRIVER_KCONFIG"; then
		sed -i "/drivers\\/$MODULE_NAME\\/Kconfig/d" "$DRIVER_KCONFIG"
		echo "[-] Kconfig reverted."
	fi

	if [ -d "$SRC_DIR" ]; then
		git submodule deinit -f "$MODULE_NAME" 2>/dev/null || true
		git rm -f "$MODULE_NAME" 2>/dev/null || rm -rf "$SRC_DIR"
		rm -rf ".git/modules/$MODULE_NAME"
		echo "[-] $MODULE_NAME submodule removed."
	fi
}

setup_module() {
	echo "[+] Setting up $MODULE_NAME..."

	if [ ! -d "$SRC_DIR" ]; then
		git submodule add "$REPO_URL" "$MODULE_NAME"
		echo "[+] Submodule added."
	else
		git submodule update --init --recursive "$MODULE_NAME"
	fi

	cd "$SRC_DIR"

	if [ -f .git/shallow ]; then
		git fetch --unshallow
		echo "[-] Unshallowed."
	fi

	git stash
	echo "[-] Stashed local changes."
	git fetch --tags

	if [ -z "${1-}" ]; then
		TAG=$(git describe --abbrev=0 --tags 2>/dev/null || true)
		if [ -n "$TAG" ]; then
			git checkout "$TAG"
			echo "[-] Checked out latest tag: $TAG."
		else
			git pull
			echo "[-] No tags published yet, using the default branch tip."
		fi
	else
		git checkout "$1"
		echo "[-] Checked out $1."
	fi

	TARGET_DIR="$SRC_DIR/$MODULE_SUBDIR"
	for required in Kconfig Makefile; do
		if [ ! -f "$TARGET_DIR/$required" ]; then
			echo "[ERROR] $TARGET_DIR/$required not found. Nothing was linked or modified."
			exit 1
		fi
	done

	cd "$DRIVER_DIR"
	ln -sfn "$(realpath --relative-to="$DRIVER_DIR" "$TARGET_DIR")" "$MODULE_NAME"
	echo "[+] Symlink created."

	if ! grep -q "$MODULE_NAME" "$DRIVER_MAKEFILE"; then
		printf '\nobj-$(CONFIG_ENCORE_FAS) += %s/\n' "$MODULE_NAME" >>"$DRIVER_MAKEFILE"
		echo "[+] Modified Makefile."
	fi

	if ! grep -q "source \"drivers/$MODULE_NAME/Kconfig\"" "$DRIVER_KCONFIG"; then
		sed -i "/endmenu/i\\source \"drivers/$MODULE_NAME/Kconfig\"" "$DRIVER_KCONFIG"
		echo "[+] Modified Kconfig."
	fi

	echo "[+] Enable CONFIG_ENCORE_FAS in your defconfig to build it in or as a module."
}

if [ "$#" -eq 0 ]; then
	initialize_variables
	setup_module
elif [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
	display_usage
elif [ "$1" = "--cleanup" ]; then
	initialize_variables
	perform_cleanup
else
	initialize_variables
	setup_module "$@"
fi
