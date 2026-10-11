#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID} -eq 0 ]]; then
  echo "Refusing to run as root; sudo is used only for CachyOS/Arch packages." >&2
  exit 1
fi

check=false
case ${1:-} in
  '') ;;
  --check) check=true ;;
  *) echo "Usage: $0 [--check]" >&2; exit 2 ;;
esac

[[ -f /etc/arch-release ]] || {
  echo "Unsupported system: this installer targets CachyOS/Arch." >&2
  exit 1
}

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "${script_dir}/.." && pwd)
prefix=${NIRISNAP_PREFIX:-${HOME}/.local}
build_dir=${XDG_CACHE_HOME:-${HOME}/.cache}/nirisnap/build
data_home=${XDG_DATA_HOME:-${HOME}/.local/share}
plugin_source=${repo_root}/plugin/nirisnap
plugin_link=${data_home}/noctalia/plugins/nirisnap
keybind_source=${script_dir}/nirisnap-keybinds.kdl
keybind_target=${prefix}/share/nirisnap/nirisnap-keybinds.kdl
packages=(base-devel cmake ninja pkgconf qt6-base layer-shell-qt grim wl-clipboard tesseract tesseract-data-eng libnotify)

announce() { printf '==> %s\n' "$*"; }

for path in "${repo_root}/CMakeLists.txt" "${repo_root}/nirisnap.desktop" \
            "${plugin_source}/plugin.toml" "${keybind_source}"; do
  [[ -f ${path} ]] || { echo "Missing source file: ${path}" >&2; exit 1; }
done

if [[ -L ${plugin_link} ]]; then
  [[ $(readlink -f -- "${plugin_link}") == "${plugin_source}" ]] || {
    echo "Refusing to replace plugin link: ${plugin_link}" >&2
    exit 1
  }
elif [[ -e ${plugin_link} || -L ${plugin_link} ]]; then
  echo "Refusing to replace plugin path: ${plugin_link}" >&2
  exit 1
fi

install_paths=(
  "${prefix}/bin/nirisnap"
  "${prefix}/share/applications/nirisnap.desktop"
  "${prefix}/share/licenses/nirisnap/Neucha-OFL.txt"
  "${keybind_target}"
)
for path in "${install_paths[@]}"; do
  [[ ! -e ${path} && ! -L ${path} ]] || {
    echo "Refusing to overwrite existing install path: ${path}" >&2
    exit 1
  }
done

announce "CachyOS/Arch packages: ${packages[*]}"
announce "Build directory: ${build_dir}"
announce "Install prefix: ${prefix}"
announce "Noctalia plugin: ${plugin_link} -> ${plugin_source}"
announce "Niri snippet: ${keybind_target}"
printf '%s\n' "Will not edit Noctalia config/state or Niri config; enable the v5 plugin explicitly."

if "${check}"; then
  announce "Preflight passed"
  printf 'Would run: /usr/bin/sudo -- /usr/bin/pacman -S --needed'
  printf ' %q' "${packages[@]}"
  printf '\n'
  exit 0
fi

[[ -x /usr/bin/sudo && -x /usr/bin/pacman ]] || {
  echo "/usr/bin/sudo or /usr/bin/pacman not found." >&2
  exit 1
}
for command in cmake ninja python3 noctalia niri; do
  command -v "${command}" >/dev/null || { echo "${command} not found." >&2; exit 1; }
done

announce "Installing build and runtime dependencies"
/usr/bin/sudo -- /usr/bin/pacman -S --needed "${packages[@]}"

announce "Building Nirisnap"
cmake -S "${repo_root}" -B "${build_dir}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${prefix}"
cmake --build "${build_dir}" --parallel
QT_QPA_PLATFORM=offscreen "${build_dir}/nirisnap-smoke" "${build_dir}/nirisnap-smoke-output"

announce "Installing Nirisnap"
cmake --install "${build_dir}"
mkdir -p "$(dirname -- "${keybind_target}")" "$(dirname -- "${plugin_link}")"
cp -- "${keybind_source}" "${keybind_target}"
ln -s "${plugin_source}" "${plugin_link}"

cat <<EOF

Installed Nirisnap. Next steps:
  1. Reload Noctalia: noctalia msg config-reload
  2. Enable plugin: noctalia msg plugins enable itc-steve/nirisnap
  3. Add itc-steve/nirisnap:capture in Noctalia Settings > Bar > Widgets.
  4. Required Niri bind: in ~/.config/niri/cfg/keybinds.kdl replace
     CTRL+Shift+1 { screenshot; } with the spawn line in ${keybind_target}.
     See README.md step 4. Then: niri validate
EOF
