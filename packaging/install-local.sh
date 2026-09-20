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
config_home=${XDG_CONFIG_HOME:-${HOME}/.config}
plugin_source=${repo_root}/plugin/nirisnap
plugin_link=${config_home}/noctalia/plugins/nirisnap
registry=${config_home}/noctalia/plugins.json
keybind_source=${script_dir}/nirisnap-keybinds.kdl
keybind_target=${prefix}/share/nirisnap/nirisnap-keybinds.kdl
packages=(base-devel cmake ninja pkgconf qt6-base layer-shell-qt grim wl-clipboard tesseract tesseract-data-eng libnotify)

announce() { printf '==> %s\n' "$*"; }

for path in "${repo_root}/CMakeLists.txt" "${repo_root}/nirisnap.desktop" \
            "${plugin_source}/manifest.json" "${keybind_source}"; do
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

python3 - "${registry}" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
if path.exists():
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or not isinstance(data.get("states", {}), dict):
        raise SystemExit(f"Invalid Noctalia plugin registry: {path}")
PY

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
printf '%s\n' "Will not edit Noctalia settings.json or Niri config."

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
for command in cmake ninja python3 qs niri; do
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

announce "Registering Noctalia plugin"
python3 - "${registry}" <<'PY'
import json
import os
import stat
import sys
from pathlib import Path

path = Path(sys.argv[1])
if path.exists():
    data = json.loads(path.read_text(encoding="utf-8"))
    mode = stat.S_IMODE(path.stat().st_mode)
else:
    data = {"sources": [], "states": {}, "version": 2}
    mode = 0o644

data.setdefault("states", {})["nirisnap"] = {"enabled": True, "sourceUrl": "local"}
data.setdefault("version", 2)
path.parent.mkdir(parents=True, exist_ok=True)
tmp = path.with_suffix(path.suffix + ".tmp")
tmp.write_text(json.dumps(data, indent=4) + "\n", encoding="utf-8")
os.chmod(tmp, mode)
os.replace(tmp, path)
PY

if ! qs --help >/dev/null 2>&1; then
  printf '%s\n' \
    "warning: qs failed to start (often Qt_6_PRIVATE_API after a qt6 upgrade)." \
    "Do not reinstall the same noctalia-qs package. Rebuild it against current Qt." \
    "See README.md: If qs fails after a Qt upgrade."
fi

cat <<EOF

Installed Nirisnap. Next steps:
  1. Confirm qs works: qs --version
  2. Restart Noctalia: qs kill -c noctalia-shell && qs -d -c noctalia-shell
  3. Add Nirisnap in Noctalia Settings > Bar > Widgets.
  4. Required Niri bind: in ~/.config/niri/cfg/keybinds.kdl replace
     CTRL+Shift+1 { screenshot; } with the spawn line in ${keybind_target}.
     See README.md step 4. Then: niri validate
EOF
