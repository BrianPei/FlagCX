#!/usr/bin/env bash
FLAGCX_CI_ENV_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=/dev/null
source "$FLAGCX_CI_ENV_DIR/../ci/rdma_static_preflight.sh"

if [[ -f /usr/local/Ascend/ascend-toolkit/set_env.sh ]]; then
  # shellcheck source=/dev/null
  source /usr/local/Ascend/ascend-toolkit/set_env.sh
fi

export DEVICE_HOME=${DEVICE_HOME:-/usr/local/Ascend/ascend-toolkit/latest}
export CCL_HOME=${CCL_HOME:-$DEVICE_HOME}
export MPI_HOME=${MPI_HOME:-/usr/local/mpi}
export PATH="$DEVICE_HOME/bin:${PATH:-}"
export LD_LIBRARY_PATH="$DEVICE_HOME/lib64:/usr/local/Ascend/driver/lib64:${LD_LIBRARY_PATH:-}"

FLAGCX_CI_PROJECT_MAKE_ARGS=(USE_ASCEND=1)
FLAGCX_CI_TEST_MAKE_ARGS=(USE_ASCEND=1)
FLAGCX_CI_ENABLE_IBUC=0
FLAGCX_CI_ENABLE_SHARED_P2P_ENGINE=0
FLAGCX_CI_ENABLE_PXN=0
FLAGCX_CI_INTRA_NP=4
FLAGCX_CI_NODE_NP=2
FLAGCX_CI_RUNNER_NP=4
export NP=4

# One Ascend host provides enough devices to run two logical host contexts
# through MPI MPMD for inter-mode transport coverage.
FLAGCX_CI_NODE1_MPI_ARGS=(
  -x FLAGCX_HOSTID=node0
  -x NCCL_HOSTID=node0
)
FLAGCX_CI_NODE2_MPI_ARGS=(
  -x FLAGCX_HOSTID=node1
  -x NCCL_HOSTID=node1
)

flagcx_ci_configure_suite() {
  case "$1" in
    core|adaptor|device_api_host|p2p|rma|runner|service|symmem) ;;
    *) echo "Ascend CI does not enable suite: $1" >&2; return 1 ;;
  esac
}

flagcx_ci_build_suite_additional() {
  local suite=$1
  local -a args=("${FLAGCX_CI_TEST_MAKE_ARGS[@]}")
  case "$suite" in
    adaptor)
      make -C "$PROJECT_ROOT/test/unittest/device" --jobs="$(nproc)" \
        all "${args[@]}"
      ;;
    device_api_host)
      make -C "$PROJECT_ROOT/test/unittest/kernel" --jobs="$(nproc)" \
        "$PROJECT_ROOT/test/unittest/kernel/build/bin/kernel_unit_tests" \
        "${args[@]}"
      ;;
    core)
      make -C "$PROJECT_ROOT/test/unittest/topo" --jobs="$(nproc)" \
        all "${args[@]}"
      ;;
  esac
}

flagcx_ci_run_suite_additional() {
  local suite=$1
  local -a args=("${FLAGCX_CI_TEST_MAKE_ARGS[@]}")
  case "$suite" in
    adaptor)
      FLAGCX_CI_TEST_LABEL="Ascend device tests (adaptor suite)" \
        "$TEST_RUNNER" make -C "$PROJECT_ROOT/test/unittest/device" run \
        "${args[@]}"
      ;;
    device_api_host)
      FLAGCX_CI_TEST_LABEL="Ascend kernel host tests (device_api_host suite)" \
        "$TEST_RUNNER" make -C "$PROJECT_ROOT/test/unittest/kernel" run-unit \
        "${args[@]}"
      ;;
    core)
      FLAGCX_CI_TEST_LABEL="Ascend topology tests (core suite)" \
        "$TEST_RUNNER" make -C "$PROJECT_ROOT/test/unittest/topo" run-unit \
        "${args[@]}"
      ;;
  esac
}

flagcx_ci_prepare() {
  local suite=$1
  echo "Preparing Ascend environment for CI workload: $suite"
  test -d "$DEVICE_HOME/include"
  test -e /dev/davinci_manager
  test -e /dev/devmm_svm
  test -e /dev/hisi_hdc
  test -x /usr/local/sbin/npu-smi
  test -e /dev/infiniband/rdma_cm
  echo "Ascend host and container device inventory:"
  npu-smi info
  ls -l /dev/davinci* /dev/infiniband
  command -v g++
  if [[ "$suite" == core ]]; then
    flagcx_ci_validate_rdma core
  fi
}

flagcx_ci_validate_rdma() {
  local suite=$1
  flagcx_ci_validate_rdma_static Ascend "$suite" "/sys/class/infiniband/*"
}
