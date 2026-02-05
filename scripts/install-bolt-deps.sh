#!/usr/bin/env bash
# Copyright (c) ByteDance Ltd. and/or its affiliates.
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
#
set -euo pipefail

CUR_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null && pwd)"
cd "${CUR_DIR}"

CCI_HOME="${CONAN_HOME:-~/.conan2}/conan-center-index"

if ! command -v conan &> /dev/null; then
  echo "❌ Error: 'conan' command not found."
  exit 1
fi

# Does a shallow checkout of conan-center-index at the given commit id in $1
checkout_conan_center_index() {
  local tag_name="bolt-dev"
  local current_origin_url=""
  local bolt_dev_origin_url="https://github.com/bytedance/conan-center-index.git"

  # check if we are on a Bolt tag
  # for a bolt release, we use the tag name as the conan-center-index tag name
  if git describe --exact-match --tags HEAD > /dev/null 2>&1; then
    tag_name=$(git describe --exact-match --tags HEAD)
  fi

  \rm -rf "${CCI_HOME}"
  mkdir -p "${CCI_HOME}"
  pushd "${CCI_HOME}" > /dev/null
  echo "ℹ️  Cloning conan-center-index at ${tag_name} from ${bolt_dev_origin_url}..."
  git init -q
  git remote add origin https://github.com/bytedance/conan-center-index.git
  git fetch origin --depth 1 ${tag_name}
  git switch -q ${tag_name}
  popd > /dev/null
}

update_conan_remote() {
  local remote_name="$1"
  local remote_url="$2"
  local remote_type="${3:-}"

  echo "⚙️  Configuring remote '${remote_name}'..."
  conan remote remove "${remote_name}" > /dev/null 2>&1 || true

  if [ -n "$remote_type" ]; then
    conan remote add -t "$remote_type" "${remote_name}" "${remote_url}" > /dev/null
  else
    conan remote add "${remote_name}" "${remote_url}" > /dev/null
  fi
}

checkout_conan_center_index

update_conan_remote "bolt-cci-local" "${CCI_HOME}" "local-recipes-index"

update_conan_remote "conancenter" "https://center2.conan.io"

echo "🎉 All done! Conan remotes configured."
