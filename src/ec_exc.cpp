/*
 * Execution Context
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012 Udo Steinberg, Intel Corporation.
 * Copyright (C) 2019 Julian Stecklina, Cyberus Technology GmbH.
 *
 * This file is part of the Hedron hypervisor.
 *
 * Hedron is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Hedron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License version 2 for more details.
 */

#include "counter.hpp"
#include "ec.hpp"
#include "extern.hpp"
#include "gdt.hpp"
#include "mca.hpp"

void Ec::load_fpu()
{
    // The idle EC never switches to user space and we do not use the FPU inside the kernel. Thus we can
    // skip loading or saving the FPU-state in case this EC is an idle EC to improve the performance.
    if (not is_idle_ec()) {
        fpu.load();
    }
}

void Ec::save_fpu()
{
    // See comment in Ec::load_fpu.
    if (not is_idle_ec()) {
        fpu.save();
    }
}

void Ec::transfer_fpu(Ec* from_ec)
{
    if (from_ec == this) {
        return;
    }

    from_ec->save_fpu();
    load_fpu();
}

bool Ec::handle_exc_gp(Exc_regs* r)
{
    if (fixup(r)) {
        return true;
    }

    if (Cpu::hazard() & HZD_TR) {
        Cpu::hazard() &= ~HZD_TR;

        // The VM exit has re-set the TR segment limit to 0x67. This breaks the
        // IO permission bitmap. Restore the correct value.
        Gdt::unbusy_tss();
        Tss::load();
        return true;
    }

    return false;
}

bool Ec::handle_exc_pf(Exc_regs* r)
{
    mword addr = r->cr2;

    if (r->err & Hpt::ERR_U)
        return false;

    // Kernel fault in OBJ space
    if (addr >= SPC_LOCAL_OBJ) {
        Space_obj::page_fault(addr, r->err);
        return true;
    }

    die("#PF (kernel)", r);
}

void Ec::fixup_nmi_user_trap()
{
    // We recognized the NMI and don't need to trap on the next exit to user space unless we get another NMI.

    // Restore the whole GDT so IRET can return to user space.
    Gdt::load();

    // Restore HOST_SEL_CS to be able to call VMRESUME.
    Vmcs::write(Vmcs::HOST_SEL_CS, SEL_KERN_CODE);
}

void Ec::do_early_nmi_work()
{
    // This function is called in the NMI handler (and maybe also somewhere else) and thus there are certain
    // things that must not be done in this function:
    //   - we must not access any locks or mutexes
    //   - we must not access any kernel data structures that are not atomically updated
    //
    // Keep in mind that NMIs may interrupt the kernel code at an arbitrary position. You can find my
    // information about the NMI handling in Ec::handle_exc_altstack.

    // Tell the shootdown code that we received the interrupt. We have to get to the actual shootdown
    // before we execute any user/guest code, but we can already acknowledge the shootdown.
    Atomic::add(Counter::tlb_shootdown(), static_cast<uint16>(1));
}

void Ec::do_deferred_nmi_work()
{
    // Here we are doing the work that we cannot unconditionally do inside the NMI handler. This function
    // should only be called by
    // - the NMI handler, if we were running in user space while receiving the NMI
    // - Ec::maybe_handle_deferred_nmi_work, which is called if we received an exception that may be caused by
    // the NMI handler
    // - Vcpu::handle_exception, if a VM exit was caused by an NMI
    //
    // All these cases have in common that we know that we are not holding any locks or that we interrupted
    // the kernel while manipulating some data structures and thus most actions are safe.
    //
    // We have to keep in mind though that we may not run on the normal kernel stack, but on the NMI stack.
    //
    // The caller of this function has to make sure that we can access CPU-local data.

    assert_slow(Cpulocal::is_initialized());

    // Handle a stale TLB.
    if (Pd::current()->Space_mem::stale_host_tlb.chk(Cpu::id())) {
        Pd::current()->Space_mem::stale_host_tlb.clr(Cpu::id());
        Hpt::flush();
    }
}

void Ec::maybe_handle_deferred_nmi_work(Exc_regs* r)
{
    if (EXPECT_TRUE(r->vec != Cpu::EXC_GP)) {
        // To handle an NMI we always generate a #GP, thus if we are currently not handling a #GP, we can
        // return.
        return;
    }

    // The exception occured when we tried to execute an IRET.
    const bool exc_on_iret_to_user{r->cs == SEL_KERN_CODE and static_cast<int64>(r->rip) < 0 and
                                   r->rip == reinterpret_cast<mword>(&iret_to_user)};

    if (exc_on_iret_to_user) {
        // ret_user_iret does a swapgs before executing the IRET. Thus here we have to swapgs again in
        // order to handle the deferred work.
        swapgs();

        // Fix our state so we are able to return to user space.
        fixup_nmi_user_trap();
    }

    assert(Cpulocal::is_initialized());
    assert(Cpulocal::has_valid_stack());

    // At this point, it is safe again to interact with the rest of the kernel, because we restored CPU-local
    // memory.

    if (not exc_on_iret_to_user) {
        return;
    }

    // We have deferred work from an earlier NMI.
    do_deferred_nmi_work();

    // If we interrupted the kernel, the RIP for this #GP points to the IRET instruction after any
    // swapgs. When we return to that iret, we would have to swapgs again to return GS_BASE and
    // KERNEL_GS_BASE to its intended values.
    //
    // It's easier to call ret_user_iret, because this also does the hazard checking that we want.
    Ec::ret_user_iret();
}

void Ec::handle_exc(Exc_regs* r)
{
    // WARNING: When we enter here, it is NOT SAFE to use CPU-local memory until we handled any deferred NMI
    // work by calling maybe_handle_deferred_nmi_work. This function will not return when the reason for the
    // exception was pending work from the NMI.
    maybe_handle_deferred_nmi_work(r);

    // If we get here, CPU-local memory is initialized and kernel data structures can be accessed.
    assert_slow(Cpulocal::is_initialized());
    assert_slow(Cpulocal::has_valid_stack());
    assert_slow(r->vec == r->dst_portal);

    switch (r->vec) {

    case Cpu::EXC_GP:
        if (handle_exc_gp(r))
            return;
        break;

    case Cpu::EXC_PF:
        if (handle_exc_pf(r))
            return;
        break;

    case Cpu::EXC_MC:
        Mca::vector();
        break;
    }

    if (r->user())
        send_msg<ret_user_iret>();

    die("EXC", r);
}

void Ec::handle_exc_altstack(Exc_regs* r)
{
    // When we enter here, the GS base and KERNEL_GS_BASE MSR are not set up for kernel use. We restore GS
    // base and leave KERNEL_GS_BASE as is.

    const mword old_gs_base{rdgsbase()};

    // This means we can use CPU-local variables, but not exit from this handler via any path that expects
    // SWAPGS to work. Also the register state has only been saved on the current stack. This means any return
    // from this interrupt must happen via a return from this function.
    //
    // If we interrupted the kernel, we could have interrupted the kernel at any point. It could have been
    // holding an spinlock to modify a data structure. So grabbing spinlocks here is not safe.

    Cpulocal::restore_for_nmi();

    switch (r->vec) {
    case Cpu::EXC_NMI:

        do_early_nmi_work();
        if (r->user() /* from userspace */) {
            // Cpulocal::restore_for_nmi has changed GS_BASE, thus we have to restore the old_gs_base and then
            // call swapgs() to make GS_BASE/GS_BASE_KERNEL look like the kernel.
            wrgsbase(old_gs_base);
            swapgs();

            // We came from user space, thus the whole GDT must be loaded.
            assert_slow(Gdt::store().limit == Gdt::limit());

            // At this point the GS base must have the correct value. Otherwise do_deferred_nmi_work can't do
            // its work.
            assert_slow(Cpulocal::is_initialized());

            // We came from user space, thus we can do the deferred work here.
            do_deferred_nmi_work();

            // We will go back to user space, thus we have to swapgs again.
            swapgs();
        } else {
            // We interrupted the kernel. The next exit to userspace needs to fault so we can check hazards.

            // If we receive the NMI while the RIP points to the 'hlt' in our idle handler, we have to bump
            // the RIP. Otherwise, the NMI destroys the sti-blocking and we could receive an interrupt between
            // the 'sti' and the 'hlt' and thus may go to sleep even though the interrupt would need
            // processing.
            const bool nmi_on_idle_hlt{r->cs == SEL_KERN_CODE and static_cast<int64>(r->rip) < 0 and
                                       r->rip == reinterpret_cast<mword>(&idle_hlt)};
            if (nmi_on_idle_hlt) {
                r->rip += 1;
            }

            // IRET to userspace faults when the userspace code selector is beyond the GDT limit.
            Gdt::load_kernel_only();

            // A null selector in CS will cause a VM entry failure.
            if (Cpu::feature(Cpu::FEAT_VMX) and Vmcs::current()) {
                Vmcs::write(Vmcs::HOST_SEL_CS, 0);
            }

            // We return to the kernel.
            wrgsbase(old_gs_base);
        }
        break;

    case Cpu::EXC_DF:
        panic("Received Double Fault at on CPU %u at RIP %#lx", Cpu::id(), r->rip);
        break;

    default:
        panic("Unexpected interrupt received: %ld at RIP %#lx", r->vec, r->rip);
        break;
    }
}
