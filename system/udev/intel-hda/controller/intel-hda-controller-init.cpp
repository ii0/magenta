// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <hw/arch_ops.h>
#include <intel-hda-driver-utils/debug-logging.h>

#include "intel-hda-controller.h"
#include "intel-hda-stream.h"
#include "utils.h"

namespace {
static constexpr mx_time_t INTEL_HDA_RESET_HOLD_TIME_NSEC        = 100000;   // Section 5.5.1.2
static constexpr mx_time_t INTEL_HDA_RESET_TIMEOUT_NSEC          = 1000000;  // 1mS Arbitrary
static constexpr mx_time_t INTEL_HDA_RING_BUF_RESET_TIMEOUT_NSEC = 1000000;  // 1mS Arbitrary
static constexpr mx_time_t INTEL_HDA_RESET_POLL_TIMEOUT_NSEC     = 10000;    // 10uS Arbitrary
static constexpr mx_time_t INTEL_HDA_CODEC_DISCOVERY_WAIT_NSEC   = 521000;   // Section 4.3
static constexpr size_t CMD_BUFFER_SIZE = 4096;
}  // anon namespace

mx_status_t IntelHDAController::ResetControllerHW() {
    mx_status_t res;

    // Assert the reset signal and wait for the controller to ack.
    REG_CLR_BITS<uint32_t>(&regs_->gctl, HDA_REG_GCTL_HWINIT);
    hw_rmb();

    res = WaitCondition(INTEL_HDA_RESET_TIMEOUT_NSEC,
                        INTEL_HDA_RESET_POLL_TIMEOUT_NSEC,
                        [](void* r) -> bool {
                           auto regs = reinterpret_cast<hda_registers_t*>(r);
                           return (REG_RD(&regs->gctl) & HDA_REG_GCTL_HWINIT) == 0;
                        },
                        regs_);

    if (res != NO_ERROR)
        goto finished;

    // Wait the spec mandated hold time.
    mx_nanosleep(INTEL_HDA_RESET_HOLD_TIME_NSEC);

    // Deassert the reset signal and wait for the controller to ack.
    REG_SET_BITS<uint32_t>(&regs_->gctl, HDA_REG_GCTL_HWINIT);
    hw_rmb();

    res = WaitCondition(INTEL_HDA_RESET_TIMEOUT_NSEC,
                        INTEL_HDA_RESET_POLL_TIMEOUT_NSEC,
                        [](void* r) -> bool {
                           auto regs = reinterpret_cast<hda_registers_t*>(r);
                           return (REG_RD(&regs->gctl) & HDA_REG_GCTL_HWINIT) != 0;
                        },
                        regs_);

    if (res != NO_ERROR)
        goto finished;

    // Wait the spec mandated discovery time.
    mx_nanosleep(INTEL_HDA_CODEC_DISCOVERY_WAIT_NSEC);

finished:
    if (res == ERR_TIMED_OUT)
        LOG("Timeout during reset\n");

    return res;
}

mx_status_t IntelHDAController::ResetCORBRdPtrLocked() {
    mx_status_t res;

    /* Set the reset bit, then wait for ack from the HW.  See Section 3.3.21 */
    REG_WR(&regs_->corbrp, HDA_REG_CORBRP_RST);
    if ((res = WaitCondition(INTEL_HDA_RING_BUF_RESET_TIMEOUT_NSEC,
                             INTEL_HDA_RESET_POLL_TIMEOUT_NSEC,
                             [](void* r) -> bool {
                                auto regs = reinterpret_cast<hda_registers_t*>(r);
                                return (REG_RD(&regs->corbrp) & HDA_REG_CORBRP_RST) != 0;
                             },
                             regs_)) != NO_ERROR) {
        return res;
    }

    /* Clear the reset bit, then wait for ack */
    REG_WR(&regs_->corbrp, 0u);
    if ((res = WaitCondition(INTEL_HDA_RING_BUF_RESET_TIMEOUT_NSEC,
                             INTEL_HDA_RESET_POLL_TIMEOUT_NSEC,
                             [](void* r) -> bool {
                                auto regs = reinterpret_cast<hda_registers_t*>(r);
                                return (REG_RD(&regs->corbrp) & HDA_REG_CORBRP_RST) == 0;
                             },
                             regs_)) != NO_ERROR) {
        return res;
    }

    return NO_ERROR;
}

mx_status_t IntelHDAController::SetupPCIDevice(mx_device_t* pci_dev) {
    mx_status_t res;

    if (pci_dev == nullptr)
        return ERR_INVALID_ARGS;

    // Have we already been set up?
    if (pci_dev_ != nullptr) {
        LOG("Device already initialized!\n");
        return ERR_BAD_STATE;
    }

    DEBUG_ASSERT(irq_handle_  == MX_HANDLE_INVALID);
    DEBUG_ASSERT(regs_handle_ == MX_HANDLE_INVALID);
    DEBUG_ASSERT(pci_proto_   == nullptr);

    pci_dev_ = pci_dev;

    // Fetch our BDF address and use it to generate our debug tag.
    uint32_t bdf_addr;
    if (GetDevProperty(pci_dev_, BIND_PCI_BDF_ADDR, &bdf_addr)) {
        snprintf(debug_tag_, sizeof(debug_tag_), "IHDA Controller %02x:%02x.%01x",
                BIND_PCI_BDF_UNPACK_BUS(bdf_addr),
                BIND_PCI_BDF_UNPACK_DEV(bdf_addr),
                BIND_PCI_BDF_UNPACK_FUNC(bdf_addr));
    } else {
        snprintf(debug_tag_, sizeof(debug_tag_), "IHDA Controller (unknown BDF)");
    }

    // The device had better be a PCI device, or we are very confused.
    res = device_get_protocol(pci_dev_, MX_PROTOCOL_PCI, reinterpret_cast<void**>(&pci_proto_));
    if (res != NO_ERROR) {
        LOG("PCI device does not support PCI protocol! (res %d)\n", res);
        return res;
    }

    // Claim the device.
    DEBUG_ASSERT(pci_proto_ != nullptr);
    res = pci_proto_->claim_device(pci_dev_);
    if (res != NO_ERROR) {
        LOG("Failed to claim PCI device! (res %d)\n", res);
        return res;
    }

    // Configure our IRQ mode and map our IRQ handle.  Try to use MSI, but if
    // that fails, fall back on legacy IRQs.
    res = pci_proto_->set_irq_mode(pci_dev_, MX_PCIE_IRQ_MODE_MSI, 1);
    if (res != NO_ERROR) {
        res = pci_proto_->set_irq_mode(pci_dev_, MX_PCIE_IRQ_MODE_LEGACY, 1);
        if (res != NO_ERROR) {
            LOG("Failed to set IRQ mode (%d)!\n", res);
            return res;
        } else {
            LOG("Falling back on legacy IRQ mode!\n");
            msi_irq_ = false;
        }
    } else {
        msi_irq_ = true;
    }

    DEBUG_ASSERT(irq_handle_ == MX_HANDLE_INVALID);
    res = pci_proto_->map_interrupt(pci_dev_, 0, &irq_handle_);
    if (res != NO_ERROR) {
        LOG("Failed to map IRQ! (res %d)\n", res);
        return res;
    }

    // Map in the registers located at BAR 0.  Make sure that they are the size
    // we expect them to be.
    DEBUG_ASSERT(regs_handle_ == MX_HANDLE_INVALID);
    uint64_t reg_window_size;
    hda_all_registers_t* all_regs;
    res = pci_proto_->map_mmio(pci_dev_,
                               0,
                               MX_CACHE_POLICY_UNCACHED_DEVICE,
                               reinterpret_cast<void**>(&all_regs),
                               &reg_window_size,
                               &regs_handle_);
    if (res != NO_ERROR) {
        LOG("Error attempting to map registers (res %d)\n", res);
        return res;
    }

    if (sizeof(*all_regs) != reg_window_size) {
        LOG("Bad register window size (expected 0x%zx got 0x%" PRIx64 ")\n",
            sizeof(*all_regs), reg_window_size);
        return ERR_INVALID_ARGS;
    }

    // Enable Bus Mastering so we can DMA data and receive MSIs
    res = pci_proto_->enable_bus_master(pci_dev_, true);
    if (res != NO_ERROR) {
        LOG("Failed to enable PCI bus mastering!\n");
        return res;
    }

    regs_ = &all_regs->regs;

    return NO_ERROR;
}

mx_status_t IntelHDAController::SetupStreamDescriptors() {
    mxtl::AutoLock stream_pool_lock(&stream_pool_lock_);

    // Sanity check our stream counts.
    uint16_t gcap;
    unsigned int input_stream_cnt, output_stream_cnt, bidir_stream_cnt, total_stream_cnt;
    gcap              = REG_RD(&regs_->gcap);
    input_stream_cnt  = HDA_REG_GCAP_ISS(gcap);
    output_stream_cnt = HDA_REG_GCAP_OSS(gcap);
    bidir_stream_cnt  = HDA_REG_GCAP_BSS(gcap);
    total_stream_cnt  = input_stream_cnt + output_stream_cnt + bidir_stream_cnt;

    static_assert(IntelHDAStream::MAX_STREAMS_PER_CONTROLLER == countof(regs_->stream_desc),
                  "Max stream count mismatch!");

    if (!total_stream_cnt || (total_stream_cnt > countof(regs_->stream_desc))) {
        LOG("Invalid stream counts in GCAP register (In %u Out %u Bidir %u; Max %zu)\n",
            input_stream_cnt, output_stream_cnt, bidir_stream_cnt, countof(regs_->stream_desc));
        return ERR_INTERNAL;
    }

    // Allocate and map storage for our buffer descriptor lists.
    //
    // TODO(johngro) : Relax this restriction.  Individual BDLs need to be
    // contiguous in physical memory (and non-swap-able) but the overall
    // allocation does not need to be.
    uint32_t bdl_size, total_bdl_size;

    bdl_size       = sizeof(IntelHDABDLEntry) * IntelHDAStream::MAX_BDL_LENGTH;
    total_bdl_size = bdl_size * total_stream_cnt;

    mx_status_t res = bdl_mem_.Allocate(total_bdl_size);
    if (res != NO_ERROR) {
        LOG("Failed to allocate %u bytes of contiguous physical memory for "
            "buffer descriptor lists!  (res %d)\n", total_bdl_size, res);
        return res;
    }

    // Map the memory in so that we can access it.
    res = bdl_mem_.Map();
    if (res != NO_ERROR) {
        LOG("Failed to map BDL memory!  (res %d)\n", res);
        return res;
    }

    // Allocate our stream descriptors and populate our free lists.
    for (uint32_t i = 0, bdl_off = 0; i < total_stream_cnt; ++i, bdl_off += bdl_size) {
        uint16_t stream_id = static_cast<uint16_t>(i + 1);
        auto type = (i < input_stream_cnt)
                  ? IntelHDAStream::Type::INPUT
                  : ((i < input_stream_cnt + output_stream_cnt)
                  ? IntelHDAStream::Type::OUTPUT
                  : IntelHDAStream::Type::BIDIR);

        AllocChecker ac;
        auto stream = mxtl::AdoptRef(new (&ac) IntelHDAStream(type,
                                                              stream_id,
                                                              &regs_->stream_desc[i],
                                                              bdl_mem_.phys() + bdl_off,
                                                              bdl_mem_.virt() + bdl_off));

        if (!ac.check()) {
            LOG("Failed to allocate IntelHDAStream %hu/%u!\n", stream_id, total_stream_cnt);
            return ERR_NO_MEMORY;
        }

        DEBUG_ASSERT(i < countof(all_streams_));
        DEBUG_ASSERT(all_streams_[i] == nullptr);
        all_streams_[i] = stream;

        ReturnStreamLocked(mxtl::move(stream));
    }

    return NO_ERROR;
}

mx_status_t IntelHDAController::SetupCommandBufferSize(uint8_t* size_reg,
                                                       unsigned int* entry_count) {
    // Note: this method takes advantage of the fact that the TX and RX ring
    // buffer size register bitfield definitions are identical.
    uint8_t tmp = REG_RD(size_reg);
    uint8_t cmd;

    if (tmp & HDA_REG_CORBSIZE_CAP_256ENT) {
        *entry_count = 256;
        cmd = HDA_REG_CORBSIZE_CFG_256ENT;
    } else if (tmp & HDA_REG_CORBSIZE_CAP_16ENT) {
        *entry_count = 16;
        cmd = HDA_REG_CORBSIZE_CFG_16ENT;
    } else if (tmp & HDA_REG_CORBSIZE_CAP_2ENT) {
        *entry_count = 2;
        cmd = HDA_REG_CORBSIZE_CFG_2ENT;
    } else {
        LOG("Invalid ring buffer capabilities! (0x%02x)\n", tmp);
        return ERR_BAD_STATE;
    }

    REG_WR(size_reg, cmd);
    return NO_ERROR;
}

mx_status_t IntelHDAController::SetupCommandBuffer() {
    mxtl::AutoLock corb_lock(&corb_lock_);
    mxtl::AutoLock rirb_lock(&rirb_lock_);
    mx_status_t res;

    // Allocate our command buffer memory and map it into our address space.
    // Even the largest buffers permissible should fit within a single 4k page.
    static_assert(CMD_BUFFER_SIZE >= (HDA_CORB_MAX_BYTES + HDA_RIRB_MAX_BYTES),
                  "CMD_BUFFER_SIZE to small to hold CORB and RIRB buffers!");
    res = cmd_buf_mem_.Allocate(CMD_BUFFER_SIZE);
    if (res != NO_ERROR) {
        LOG("Failed to allocate %zu bytes for CORB/RIRB command buffers! (res %d)\n",
            CMD_BUFFER_SIZE, res);
        return res;
    }

    // Now map it so we have access as well.
    res = cmd_buf_mem_.Map();
    if (res != NO_ERROR) {
        LOG("Failed to map CORB/RIRB command buffer (res %d)\n", res);
        return res;
    }

    // Start by making sure that the output and response ring buffers are being
    // held in the stopped state
    REG_WR(&regs_->corbctl, 0u);
    REG_WR(&regs_->rirbctl, 0u);

    // Reset the read and write pointers for both ring buffers
    REG_WR(&regs_->corbwp, 0u);
    res = ResetCORBRdPtrLocked();
    if (res != NO_ERROR)
        return res;

    // Note; the HW does not expose a Response Input Ring Buffer Read Pointer,
    // we have to maintain our own.
    rirb_rd_ptr_ = 0;
    REG_WR(&regs_->rirbwp, HDA_REG_RIRBWP_RST);

    // Physical memory for the CORB/RIRB should already have been allocated at
    // this point
    DEBUG_ASSERT(cmd_buf_mem_.virt() != 0);

    // Determine the ring buffer sizes.  If there are options, make them as
    // large as possible.
    res = SetupCommandBufferSize(&regs_->corbsize, &corb_entry_count_);
    if (res != NO_ERROR)
        return res;

    res = SetupCommandBufferSize(&regs_->rirbsize, &rirb_entry_count_);
    if (res != NO_ERROR)
        return res;

    // Stash these so we don't have to constantly recalculate then
    corb_mask_ = corb_entry_count_ - 1u;
    rirb_mask_ = rirb_entry_count_ - 1u;
    corb_max_in_flight_ = rirb_mask_ > RIRB_RESERVED_RESPONSE_SLOTS
                        ? rirb_mask_ - RIRB_RESERVED_RESPONSE_SLOTS
                        : 1;
    corb_max_in_flight_ = mxtl::min(corb_max_in_flight_, corb_mask_);

    // Program the base address registers for the TX/RX ring buffers, and set up
    // the virtual pointers to the ring buffer entries.
    uint64_t cmd_buf_paddr64 = static_cast<uint64_t>(cmd_buf_mem_.phys());

    // TODO(johngro) : If the controller does not support 64 bit phys
    // addressing, we need to make sure to get a page from low memory to use for
    // our command buffers.
    bool gcap_64bit_ok = HDA_REG_GCAP_64OK(REG_RD(&regs_->gcap));
    if ((cmd_buf_paddr64 >> 32) && !gcap_64bit_ok) {
        LOG("Intel HDA controller does not support 64-bit physical addressing!\n");
        return ERR_NOT_SUPPORTED;
    }

    // Section 4.4.1.1; corb ring buffer base address must be 128 byte aligned.
    DEBUG_ASSERT(!(cmd_buf_paddr64 & 0x7F));
    REG_WR(&regs_->corblbase, ((uint32_t)(cmd_buf_paddr64 & 0xFFFFFFFF)));
    REG_WR(&regs_->corbubase, ((uint32_t)(cmd_buf_paddr64 >> 32)));
    corb_ = reinterpret_cast<CodecCommand*>(cmd_buf_mem_.virt());

    cmd_buf_paddr64 += HDA_CORB_MAX_BYTES;

    // Section 4.4.2.2; rirb ring buffer base address must be 128 byte aligned.
    DEBUG_ASSERT(!(cmd_buf_paddr64 & 0x7F));
    REG_WR(&regs_->rirblbase, ((uint32_t)(cmd_buf_paddr64 & 0xFFFFFFFF)));
    REG_WR(&regs_->rirbubase, ((uint32_t)(cmd_buf_paddr64 >> 32)));
    rirb_ = reinterpret_cast<CodecResponse*>(cmd_buf_mem_.virt() + HDA_CORB_MAX_BYTES);

    // Make sure our current view of the space available in the CORB is up-to-date.
    ComputeCORBSpaceLocked();

    // Set the response interrupt count threshold.  The RIRB IRQ will fire any
    // time all of the SDATA_IN lines stop having codec responses to transmit,
    // or when RINTCNT responses have been received, whichever happens
    // first.  We would like to batch up responses to minimize IRQ load, but we
    // also need to make sure to...
    // 1) Not configure the threshold to be larger than the available space in
    //    the ring buffer.
    // 2) Reserve some space (if we can) at the end of the ring buffer so the
    //    hardware has space to write while we are servicing our IRQ.  If we
    //    reserve no space, then the ring buffer is going to fill up and
    //    potentially overflow before we can get in there and process responses.
    unsigned int thresh = rirb_entry_count_ - 1u;
    if (thresh > RIRB_RESERVED_RESPONSE_SLOTS)
        thresh -= RIRB_RESERVED_RESPONSE_SLOTS;
    DEBUG_ASSERT(thresh);
    REG_WR(&regs_->rintcnt, thresh);

    // Clear out any lingering interrupt status
    REG_WR(&regs_->corbsts, HDA_REG_CORBSTS_MEI);
    REG_WR(&regs_->rirbsts, OR(HDA_REG_RIRBSTS_INTFL, HDA_REG_RIRBSTS_OIS));

    // Enable the TX/RX IRQs and DMA engines.
    REG_WR(&regs_->corbctl, OR(HDA_REG_CORBCTL_MEIE, HDA_REG_CORBCTL_DMA_EN));
    REG_WR(&regs_->rirbctl, OR(OR(HDA_REG_RIRBCTL_INTCTL, HDA_REG_RIRBCTL_DMA_EN),
                               HDA_REG_RIRBCTL_OIC));

    return NO_ERROR;
}

mx_status_t IntelHDAController::InitInternal(mx_device_t* pci_dev) {
    mx_status_t res;

    res = SetupPCIDevice(pci_dev);
    if (res != NO_ERROR)
        return res;

    // Check our hardware version
    uint8_t major, minor;
    major = REG_RD(&regs_->vmaj);
    minor = REG_RD(&regs_->vmin);

    if ((1 != major) || (0 != minor)) {
        LOG("Unexpected HW revision %d.%d!\n", major, minor);
        return ERR_NOT_SUPPORTED;
    }

    // Completely reset the hardware
    res = ResetControllerHW();
    if (res != NO_ERROR)
        return res;

    // Allocate and set up our stream descriptors.
    res = SetupStreamDescriptors();
    if (res != NO_ERROR)
        return res;

    // Allocate and set up the codec communication ring buffers (CORB/RIRB)
    res = SetupCommandBuffer();
    if (res != NO_ERROR)
        return res;

    // Generate a device name and initialize our device structure
    snprintf(debug_tag_, sizeof(debug_tag_), "intel-hda-%03u", id());
    device_init(&dev_node_, driver_, debug_tag_, &CONTROLLER_DEVICE_THUNKS);
    dev_node_.protocol_id  = MX_PROTOCOL_IHDA;
    dev_node_.protocol_ops = nullptr;

    // Start the IRQ thread.
    int musl_res;
    musl_res = thrd_create_with_name(
            &irq_thread_,
            [](void* ctx) -> int { return static_cast<IntelHDAController*>(ctx)->IRQThread(); },
            this,
            dev_name());

    if (musl_res < 0) {
        LOG("Failed create IRQ thread! (res = %d)\n", musl_res);
        SetState(State::SHUT_DOWN);
        return ERR_INTERNAL;
    }

    irq_thread_started_ = true;

    // Publish our device.  If something goes wrong, shut down our IRQ thread
    // immediately.  Otherwise, transition to the OPERATING state and signal the
    // IRQ thread so it can begin to look for (and publish) codecs.
    //
    // TODO(johngro): We are making an assumption here about the threading
    // behavior of the device driver framework.  In particular, we are assuming
    // that Unbind will never be called after the device has been published, but
    // before Bind has unbound all the way up to the framework.  If this *can*
    // happen, then we have a race condition which would proceed as follows.
    //
    // 1) Device is published (device_add below)
    // 2) Before SetState (below) Unbind is called, which triggers a transition
    //    to SHUTTING_DOWN and wakes up the IRQ thread..
    // 3) Before the IRQ thread wakes up and exits, the SetState (below)
    //    transitions to OPERATING.
    // 4) The IRQ thread is now operating, but should be shut down.
    //
    // At some point, we need to verify the threading assumptions being made
    // here.  If they are not valid, this needs to be revisited and hardened.

    // Put an unmanaged reference to ourselves in the device node we are about
    // to publish.  Only perform an manual AddRef if we succeed in publishing
    // our device.
    dev_node_.ctx = this;
    res = device_add(&dev_node_, pci_dev_);
    if (res == NO_ERROR) {
        this->AddRef();
        SetState(State::OPERATING);
        WakeupIRQThread();
    } else {
        dev_node_.ctx = nullptr;
    }

    return res;
}

mx_status_t IntelHDAController::Init(mx_device_t* pci_dev) {
    mx_status_t res = InitInternal(pci_dev);

    if (res != NO_ERROR)
        DeviceShutdown();

    return res;
}