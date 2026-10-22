#!/usr/bin/env bash
# Copyright (c) Facebook, Inc. and its affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# --------------------------------------------------------------------------
# Copyright (c) ByteDance Ltd. and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
#
# This file has been modified by ByteDance Ltd. and/or its affiliates on
# 2025-11-11.
#
# Original file was released under the Apache License 2.0,
# with the full license text available at:
#     http://www.apache.org/licenses/LICENSE-2.0
#
# This modified file is released under the same license.
# --------------------------------------------------------------------------

CUR_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null && pwd)"

set -euo pipefail

OS_TYPE=$(uname -s)
ARCH_TYPE=$(uname -m)
CPP_STANDARD="gnu++23"
CLANG_COMPILER=""
CLANG_LINKER=""

function download_file() {
  local url="$1"
  local dest="$2"

  if command -v wget > /dev/null 2>&1; then
    wget --progress=dot:mega "$url" -O "$dest"
  elif command -v curl > /dev/null 2>&1; then
    echo "wget not found, using curl..."
    curl -fL "$url" -o "$dest"
  else
    echo "❌ Error: Neither 'wget' nor 'curl' found."
    return 1
  fi
}

function check_basic_tools() {
  local all_installed=true
  if command -v wget > /dev/null 2>&1; then
    echo "✅ Already installed wget"
  elif command -v curl > /dev/null 2>&1; then
    echo "wget not found, using curl..."
  else
    echo "❌ Error: Neither 'wget' nor 'curl' found."
    all_installed=false
  fi

  local tools=("make" "cmake" "ninja")

  for tool in "${tools[@]}"; do
    if command -v "$tool" > /dev/null 2>&1; then
      printf "✅ Already installed %s\n" "$tool"
    else
      printf "❌ Not installed %s\n" "$tool"
      all_installed=false
    fi
  done

  if ! $all_installed; then
    if [[ "$OS_TYPE" == "Darwin" ]]; then
      echo "👉 Please install missing tools using brew install wget make cmake ninja"
    else
      echo "👉 Please install missing tools using your package manager (apt/yum)."
    fi
    return 1
  else
    return 0
  fi
}

function check_cpp_standard() {
  local COMPILER_CMD="${CLANG_COMPILER/clang/clang++}"

  if ! command -v "$COMPILER_CMD" > /dev/null 2>&1; then
    echo "❌ Compiler '$COMPILER_CMD' not found."
    return 1
  fi
  local TEST_FILE=$(mktemp /tmp/test_compiler.XXXXXX)
  echo "int main(){ return 0; }" > "$TEST_FILE"

  if "$COMPILER_CMD" -x c++ -std=$CPP_STANDARD -fsyntax-only "$TEST_FILE" > /dev/null 2>&1; then
    echo "✅ Compiler $COMPILER_CMD supports $CPP_STANDARD."
    rm -f "$TEST_FILE"
    return 0
  else
    echo "❌ Compiler ('$COMPILER_CMD') too old, does not support $CPP_STANDARD."
    $COMPILER_CMD --version
    rm -f "$TEST_FILE"
    echo "👉 Please install the latest stable Clang (22 or newer)."
    return 1
  fi
}

function find_installed_clang() {
  CLANG_COMPILER=""
  local search_dirs=()
  IFS=: read -r -a search_dirs <<< "$PATH"
  search_dirs+=(/usr/lib/llvm-*/bin /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin)
  local dir candidate version compiler_bin
  local newest_major=21
  for dir in "${search_dirs[@]}"; do
    for candidate in "${dir:-.}/clang" "${dir:-.}"/clang-[0-9]*; do
      [[ -x "$candidate" ]] || continue
      version=$("$candidate" -dumpversion 2> /dev/null) || continue
      [[ "$version" =~ ^[0-9]+(\.[0-9]+)*$ ]] || continue
      ((${version%%.*} > newest_major)) || continue
      # Use the compiler's installation directory, not an alternatives symlink.
      compiler_bin=$("$candidate" --version | sed -n 's/^InstalledDir: //p')
      [[ -x "$compiler_bin/clang" ]] || continue
      newest_major=${version%%.*}
      CLANG_COMPILER="$compiler_bin/clang"
    done
  done
  [[ -n "$CLANG_COMPILER" ]] || return 1

  local linker_name=ld.lld
  [[ "$OS_TYPE" != "Darwin" ]] || linker_name=ld64.lld
  CLANG_LINKER="$(dirname "$CLANG_COMPILER")/$linker_name"
  if [[ ! -x "$CLANG_LINKER" ]]; then
    CLANG_LINKER=$(command -v "$linker_name-$newest_major" || command -v "$linker_name" || true)
    if [[ -z "$CLANG_LINKER" && "$OS_TYPE" == "Darwin" ]] && command -v brew > /dev/null 2>&1; then
      CLANG_LINKER="$(brew --prefix lld)/bin/$linker_name"
    fi
  fi
  echo "✅ Found Clang $newest_major: $CLANG_COMPILER; skipping LLVM installation."
}

function install_clang() {
  if find_installed_clang; then
    if [[ ! -x "$CLANG_LINKER" ]]; then
      echo "❌ Clang is installed, but LLD is missing. Install the matching LLD package."
      return 1
    fi
    return 0
  fi
  if [[ "$OS_TYPE" == "Darwin" ]]; then
    if ! command -v brew > /dev/null 2>&1; then
      echo "❌ Install Homebrew (https://brew.sh) before running setup."
      return 1
    fi
    brew update
    brew install llvm lld
    brew upgrade llvm lld
    CLANG_COMPILER="$(brew --prefix llvm)/bin/clang"
    CLANG_LINKER="$(brew --prefix lld)/bin/ld64.lld"
    export PATH="$(brew --prefix llvm)/bin:$(brew --prefix lld)/bin:$PATH"
  elif command -v apt-get > /dev/null 2>&1; then
    local elevate=()
    if [[ $EUID -ne 0 ]]; then
      elevate=(sudo)
    fi
    "${elevate[@]}" apt-get update
    "${elevate[@]}" apt-get install -y lsb-release wget software-properties-common gnupg

    local llvm_script
    llvm_script=$(mktemp /tmp/bolt-llvm.XXXXXX)
    download_file "https://apt.llvm.org/llvm.sh" "$llvm_script"
    # Follow the official stable release, including future major versions.
    local llvm_version
    llvm_version=$(sed -n 's/^CURRENT_LLVM_STABLE=\([0-9][0-9]*\)$/\1/p' "$llvm_script")
    if [[ ! "$llvm_version" =~ ^[0-9]+$ ]] || ((llvm_version < 22)); then
      rm -f "$llvm_script"
      echo "❌ Cannot determine a supported stable LLVM version (Clang 22+ required)."
      return 1
    fi
    "${elevate[@]}" bash "$llvm_script" "$llvm_version" all
    rm -f "$llvm_script"
    # The official installer's "all" set does not include LLVM BOLT.
    "${elevate[@]}" apt-get install -y "bolt-${llvm_version}"
    CLANG_COMPILER="/usr/lib/llvm-${llvm_version}/bin/clang"
    CLANG_LINKER="/usr/bin/ld.lld-${llvm_version}"
    export PATH="/usr/lib/llvm-${llvm_version}/bin:$PATH"
  else
    echo "❌ Automatic Clang installation supports Debian/Ubuntu and macOS."
    echo "Install the latest stable Clang (22+) and LLD, then follow the manual setup in CONTRIBUTING.md."
    return 1
  fi

  local clang_version
  clang_version=$("$CLANG_COMPILER" -dumpversion)
  if ((${clang_version%%.*} < 22)); then
    echo "❌ Clang 22 or newer is required; found ${clang_version}."
    return 1
  fi
  "$CLANG_COMPILER" --version
}

function runtime_conf_path() {
  local SHELL_CONFIG="$HOME/.bashrc"
  if [[ "$OS_TYPE" == "Darwin" && "$SHELL" == */zsh ]]; then
    SHELL_CONFIG="$HOME/.zshrc"
  elif [[ "$OS_TYPE" == "Darwin" ]]; then
    SHELL_CONFIG="$HOME/.bash_profile"
  fi
  echo "$SHELL_CONFIG"
}

function set_default_llvm() {
  local llvm_bin
  llvm_bin=$(dirname "$CLANG_COMPILER")
  local linker_bin
  linker_bin=$(dirname "$CLANG_LINKER")

  if [[ "$OS_TYPE" == "Darwin" ]]; then
    local shell_config
    shell_config=$(runtime_conf_path)
    local path_line
    printf -v path_line 'export PATH=%q:%q:$PATH' "$llvm_bin" "$linker_bin"
    touch "$shell_config"
    if ! grep -Fxq "$path_line" "$shell_config"; then
      printf '\n%s\n' "$path_line" >> "$shell_config"
    fi
  else
    local elevate=()
    if [[ $EUID -ne 0 ]]; then
      elevate=(sudo)
    fi
    local version
    version=$("$CLANG_COMPILER" -dumpversion)
    local tool
    for tool in clang clang++ lld ld.lld; do
      "${elevate[@]}" update-alternatives --install "/usr/bin/$tool" "$tool" \
        "$llvm_bin/$tool" "${version%%.*}"
      "${elevate[@]}" update-alternatives --set "$tool" "$llvm_bin/$tool"
    done
  fi

  export PATH="$llvm_bin:$linker_bin:$PATH"
  hash -r
}

function install_python_dep() {
  local SHELL_CONFIG=$(runtime_conf_path)
  local CONDA_INSTALL_DIR="$HOME/miniconda3"
  if [ -d $CONDA_INSTALL_DIR ]; then
    echo "Conda has been installed"
  else
    echo "Installing conda"
    MINICONDA_URL_BASE="https://repo.anaconda.com/miniconda"
    MINICONDA_VERSION="py310_23.1.0-1"
    if [[ "$OS_TYPE" == "Darwin" ]]; then
      if [[ "$ARCH_TYPE" == "arm64" ]]; then
        MINICONDA_URL="${MINICONDA_URL_BASE}/Miniconda3-${MINICONDA_VERSION}-MacOSX-arm64.sh"
      else
        MINICONDA_URL="${MINICONDA_URL_BASE}/Miniconda3-${MINICONDA_VERSION}-MacOSX-x86_64.sh"
      fi
    else
      MINICONDA_URL="${MINICONDA_URL_BASE}/Miniconda3-${MINICONDA_VERSION}-Linux-$(arch).sh"
    fi

    download_file "${MINICONDA_URL}" /tmp/miniconda.sh
    chmod +x /tmp/miniconda.sh && /tmp/miniconda.sh -b -u -p $CONDA_INSTALL_DIR && rm -f /tmp/miniconda.sh
    echo "export PATH=$CONDA_INSTALL_DIR/bin:\$PATH" >> "$SHELL_CONFIG"
  fi
  # Use the same environment for installation and all subsequent setup steps.
  export PATH="$CONDA_INSTALL_DIR/bin:$PATH"
  # shellcheck source=/dev/null
  $CONDA_INSTALL_DIR/bin/pip install --upgrade pip || true
  $CONDA_INSTALL_DIR/bin/pip install -r "${CUR_DIR}/../requirements.txt"
}

function check_conan() {
  # as a conan extension, add (libc, version) as conan settings.
  PATH="$(dirname "$CLANG_COMPILER"):$PATH" python "${CUR_DIR}/configure-conan-profile.py" \
    --compiler=clang --libcxx=libc++ --linker="$CLANG_LINKER"
}

function install_git_hooks() {
  # ensure pre-commit has been installed
  if ! command -v "pre-commit" > /dev/null 2>&1; then
    echo "❌ Expect pre-commit has been installed"
    return 1
  fi
  # install hooks
  pushd "${CUR_DIR}/../" > /dev/null
  if [ -f ".pre-commit-config.yaml" ]; then
    pre-commit install
    echo "✅ Installing all git hooks successfully"
    popd > /dev/null
    return 0
  fi
  # error handling
  echo "❌ Expect .pre-commit-config.yaml exists in path ${CUR_DIR}/../"
  popd > /dev/null
  return 1
}

check_basic_tools
install_clang
set_default_llvm
check_cpp_standard
install_python_dep
check_conan
echo "All required tools have been installed!"

install_bolt_deps_script="${CUR_DIR}/install-bolt-deps.sh"
if [ -f "${install_bolt_deps_script}" ]; then
  bash "${install_bolt_deps_script}" "$@"
else
  echo "⚠️  Warning: ${install_bolt_deps_script} not found, skipping."
fi

install_git_hooks

echo "💡💡💡 Please execute 'source $(runtime_conf_path)' to activate your env! 💡💡💡"
