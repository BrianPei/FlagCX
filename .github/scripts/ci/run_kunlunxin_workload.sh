#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 || "$1" != "torch-api" ]]; then
  echo "Usage: $0 torch-api" >&2
  exit 2
fi

workload=$1
project_root=${GITHUB_WORKSPACE:-$(git rev-parse --show-toplevel)}
: "${FLAGCX_CI_SET_ENV:?The platform configuration must provide set_env}"
source "$project_root/$FLAGCX_CI_SET_ENV"

flagcx_ci_prepare "$workload"
if [[ "${FLAGCX_CI_XPU_TOOLCHAIN_AVAILABLE:-0}" != 1 ]]; then
  echo "KunlunXin Torch API tests require the XTDK clang++ toolchain and headers" >&2
  exit 1
fi

export XTDK_HOME=$(flagcx_ci_find_xpu_toolchain)
export FLAGCX_ADAPTOR=klx
export JSON_INCLUDE_DIR=${JSON_INCLUDE_DIR:-/usr/include}
export CUDA_VISIBLE_DEVICES=0,1,2,3
export FLAGCX_DEBUG=${FLAGCX_DEBUG:-INFO}
export FLAGCX_DEBUG_SUBSYS=${FLAGCX_DEBUG_SUBSYS:-ALL}

python3 - <<'PY'
import torch

print("torch", torch.__version__, "devices", torch.cuda.device_count())
assert torch.cuda.is_available(), "KunlunXin Torch runtime reports no available devices"
assert torch.cuda.device_count() >= 4, "Torch API tests require four visible KunlunXin devices"
PY

python3 -m pip install 'setuptools_scm>=8'
(
  cd "$project_root"
  export TORCH_DEVICE_BACKEND_AUTOLOAD=0
  python3 setup.py build_ext --inplace
)

export PYTHONPATH="$project_root/src${PYTHONPATH:+:$PYTHONPATH}"
bash "$project_root/test/script/torch_api_test.sh"
