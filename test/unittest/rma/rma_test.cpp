#include "rma_test.hpp"
#include "adaptor.h"
#include "comm.h"
#include "sym_heap.h"
#include <cstring>

namespace {

bool allRanksReady(bool localReady) {
  int local = localReady ? 1 : 0;
  int global = 0;
  MPI_Allreduce(&local, &global, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD);
  return global != 0;
}

} // namespace

// Static member definitions
flagcxDeviceHandle_t RmaTest::devHandle = nullptr;
flagcxComm_t RmaTest::comm = nullptr;
flagcxStream_t RmaTest::stream = nullptr;
void *RmaTest::dataBuff = nullptr;
void *RmaTest::signalBuff = nullptr;
flagcxWindow_t RmaTest::dataWin = nullptr;
size_t RmaTest::size = 0;
size_t RmaTest::signalSize = 0;
bool RmaTest::dataRmaAvailable = false;
const char *RmaTest::dataRmaSkipReason = "Data RMA setup not completed";
bool RmaTest::signalRmaAvailable = false;
const char *RmaTest::signalRmaSkipReason = "Signal RMA setup not completed";

void RmaTest::SetUpTestSuite() {
  int rank, nranks;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &nranks);

  size = RMA_TEST_SIZE;
  signalSize = sizeof(uint64_t) * nranks;
  dataRmaAvailable = false;
  dataRmaSkipReason = "Data RMA setup not completed";
  signalRmaAvailable = false;
  signalRmaSkipReason = "Signal RMA setup not completed";

  int localMode = requireIpc == forceNet ? 0 : (requireIpc ? 1 : 2);
  int minMode = 0;
  int maxMode = 0;
  MPI_Allreduce(&localMode, &minMode, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD);
  MPI_Allreduce(&localMode, &maxMode, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
  if (localMode == 0 || minMode != maxMode) {
    dataRmaSkipReason =
        "All ranks must select the same single RMA transport mode";
    return;
  }
  if (rank == 0)
    std::printf("RMA transport under test: %s\n", requireIpc ? "IPC" : "NET");

  flagcxResult_t res = flagcxDeviceHandleInit(&devHandle);
  int numDevices = 0;
  bool localDeviceReady = res == flagcxSuccess && devHandle != nullptr;
  if (localDeviceReady) {
    res = devHandle->getDeviceCount(&numDevices);
    localDeviceReady = res == flagcxSuccess && numDevices > 0;
  }
  if (localDeviceReady) {
    res = devHandle->setDevice(rank % numDevices);
    localDeviceReady = res == flagcxSuccess;
  }
  if (!allRanksReady(localDeviceReady)) {
    dataRmaSkipReason =
        "Device adaptor initialization failed on at least one rank";
    return;
  }

  flagcxUniqueId uniqueId;
  bool localUniqueIdReady = true;
  if (rank == 0) {
    res = flagcxGetUniqueId(&uniqueId);
    localUniqueIdReady = res == flagcxSuccess;
  }
  if (!allRanksReady(localUniqueIdReady)) {
    dataRmaSkipReason = "Unique ID creation failed";
    return;
  }
  MPI_Bcast((void *)&uniqueId, sizeof(flagcxUniqueId), MPI_BYTE, 0,
            MPI_COMM_WORLD);

  flagcxResult_t res = flagcxCommInitRank(&comm, nranks, &uniqueId, rank);
  if (res != flagcxSuccess) {
    comm = nullptr;
    dataRmaSkipReason = "Communicator initialization failed";
    return;
  }

  // Skip setup if hetero comm not available
  if (comm == nullptr || comm->heteroComm == nullptr) {
    dataRmaSkipReason = "Hetero communicator not available";
    return;
  }

  if (comm->heteroComm->rmaProxy == nullptr) {
    dataRmaSkipReason = "RMA proxy not available";
    return;
  }

  if (comm->heteroComm->netAdaptor == nullptr ||
      comm->heteroComm->netAdaptor->iput == nullptr ||
      comm->heteroComm->netAdaptor->iget == nullptr) {
    dataRmaSkipReason = "Net adaptor does not support data RMA";
    return;
  }

  devHandle->streamCreate(&stream);

  // Allocate and register data buffer
  flagcxMemAlloc(&dataBuff, size);
  devHandle->deviceMemset(dataBuff, 0, size, flagcxMemDevice, nullptr);

  res = flagcxCommWindowRegister(comm, dataBuff, size, &dataWin,
                                 FLAGCX_WIN_COLL_SYMMETRIC);
  const int localNetworkMrReady = res == flagcxSuccess && dataWin != nullptr &&
                                  dataWin->isSymmetricDefault &&
                                  dataWin->defaultBase != nullptr &&
                                  dataWin->defaultBase->mrIndex >= 0;
  int allNetworkMrsReady = 0;
  MPI_Allreduce(&localNetworkMrReady, &allNetworkMrsReady, 1, MPI_INT, MPI_MIN,
                MPI_COMM_WORLD);
  if (!allNetworkMrsReady) {
    dataRmaSkipReason = "Symmetric window has no registered network MR";
    FAIL() << "RMA setup requires a network-registered symmetric window on "
              "every rank (result="
           << res << ", window=" << dataWin << ", mrIndex="
           << ((dataWin != nullptr && dataWin->defaultBase != nullptr)
                   ? dataWin->defaultBase->mrIndex
                   : -1)
           << ")";
  }

  // A network-registered data window is sufficient for flagcxGet tests. Do
  // not make those tests depend on the optional signal-buffer path below.
  dataRmaAvailable = true;
  dataRmaSkipReason = nullptr;

  if (comm->heteroComm->netAdaptor->iputSignal == nullptr) {
    signalRmaSkipReason = "Net adaptor does not support one-sided signals";
    return;
  }

  const bool localNetworkMrReady = dataWin->defaultBase->mrIndex >= 0;
  const bool allNetworkMrsReady = allRanksReady(localNetworkMrReady);
  const bool localNetworkGetReady =
      comm->heteroComm->netAdaptor != nullptr &&
      comm->heteroComm->netAdaptor->iget != nullptr;
  const bool allNetworkGetsReady = allRanksReady(localNetworkGetReady);
  networkRmaAvailable = allNetworkMrsReady && allNetworkGetsReady;

  if (!requireIpc) {
    if (!allNetworkMrsReady) {
      dataRmaSkipReason =
          "Symmetric window has no registered network MR on at least one rank";
      return;
    }
    if (!allNetworkGetsReady) {
      dataRmaSkipReason =
          "RDMA Get is unavailable on at least one rank's net adaptor";
      return;
    }
    dataRmaAvailable = true;
    dataRmaSkipReason = nullptr;
  } else {
    int peer = nranks == 2 ? 1 - rank : -1;
    bool peerIsLocal =
        peer >= 0 && comm->heteroComm->rankToNode != nullptr &&
        comm->heteroComm->rankToNode[peer] == comm->heteroComm->node;
    void *peerData = nullptr;
    bool localDataIpcReady =
        peerIsLocal && dataWin->defaultBase->ipcSlot >= 0 &&
        flagcxSymWindowResolveIpcPeerPtr(comm->heteroComm, dataWin->defaultBase,
                                         peer, 0, size,
                                         &peerData) == flagcxSuccess &&
        peerData != nullptr;
    ipcRmaAvailable = allRanksReady(localDataIpcReady);
    if (!ipcRmaAvailable) {
      dataRmaSkipReason = "RMA IPC mode requires a resolved peer data mapping";
      return;
    }
    dataRmaAvailable = true;
    dataRmaSkipReason = nullptr;
  }

  bool localSignalCapable =
      deviceAdaptor != nullptr && deviceAdaptor->streamWaitValue64 != nullptr;
  if (requireIpc) {
    localSignalCapable =
        localSignalCapable && deviceAdaptor->streamWriteValue64 != nullptr;
  } else {
    localSignalCapable = localSignalCapable &&
                         comm->heteroComm->netAdaptor != nullptr &&
                         comm->heteroComm->netAdaptor->iputSignal != nullptr;
  }
  const bool allSignalsCapable = allRanksReady(localSignalCapable);

  // Allocate signal buffers before entering collective registration so every
  // rank either participates or exits at the same phase.
  res = flagcxMemAlloc(&signalBuff, signalSize);
  if (res != flagcxSuccess || signalBuff == nullptr) {
    signalBuff = nullptr;
    signalRmaSkipReason = "Signal buffer allocation is not supported";
    return;
  }

  res = flagcxOneSideSignalRegister(comm, signalBuff, signalSize,
                                    FLAGCX_PTR_CUDA);
  if (res != flagcxSuccess) {
    flagcxMemFree(signalBuff);
    signalBuff = nullptr;
    signalRmaSkipReason = "Signal buffer registration is not supported";
    return;
  }

  signalRmaAvailable = true;
  signalRmaSkipReason = nullptr;
}

void RmaTest::TearDownTestSuite() {
  if (devHandle == nullptr)
    return;

  if (dataWin) {
    flagcxCommWindowDeregister(comm, dataWin);
    dataWin = nullptr;
  }

  if (signalBuff && comm && comm->heteroComm) {
    flagcxOneSideSignalDeregister(comm);
  }
  if (signalBuff) {
    flagcxMemFree(signalBuff);
    signalBuff = nullptr;
  }

  if (dataBuff) {
    flagcxMemFree(dataBuff);
    dataBuff = nullptr;
  }

  if (stream) {
    devHandle->streamDestroy(stream);
    stream = nullptr;
  }

  if (comm) {
    flagcxCommDestroy(comm);
    comm = nullptr;
  }

  flagcxDeviceHandleFree(devHandle);
  devHandle = nullptr;
}

void RmaTest::SetUp() {
  FlagCXTest::SetUp();
  if (!dataRmaAvailable) {
    GTEST_SKIP() << dataRmaSkipReason;
  }
  if (dataWin == nullptr) {
    GTEST_SKIP() << "Net adaptor does not support one-sided ops (iput/iget)";
  }
}

bool RmaTest::hasHeteroComm() const {
  return comm != nullptr && comm->heteroComm != nullptr;
}
