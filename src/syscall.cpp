/*
 * System-Call Interface
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012-2013 Udo Steinberg, Intel Corporation.
 * Copyright (C) 2014 Udo Steinberg, FireEye, Inc.
 * Copyright (C) 2013-2015 Alexander Boettcher, Genode Labs GmbH
 *
 * Copyright (C) 2018 Stefan Hertrampf, Cyberus Technology GmbH.
 * Copyright (C) 2022 Sebastian Eydam, Cyberus Technology GmbH.
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

#include "syscall.hpp"
#include "acpi.hpp"
#include "cpu.hpp"
#include "dmar.hpp"
#include "hip.hpp"
#include "hpet.hpp"
#include "ioapic.hpp"
#include "kp.hpp"
#include "lapic.hpp"
#include "msr.hpp"
#include "pci.hpp"
#include "pt.hpp"
#include "sm.hpp"
#include "stdio.hpp"
#include "suspend.hpp"
#include "utcb.hpp"
#include "vector_info.hpp"
#include "vectors.hpp"

void Ec::sys_finish(Sys_regs::Status status, bool clear_timeout)
{
    if (EXPECT_FALSE(clear_timeout))
        current()->clr_timeout();

    current()->regs.set_status(status);

    ret_user_sysexit();
}

void Ec::sys_finish(Result_void<Sys_regs::Status> result)
{
    sys_finish(EXPECT_TRUE(result.is_ok()) ? Sys_regs::Status::SUCCESS : result.unwrap_err());
}

void Ec::activate()
{
    Ec* ec = this;

    // XXX: Make the loop preemptible
    for (Sc::ctr_link() = 0; ec->partner; ec = ec->partner)
        Sc::ctr_link()++;

    if (EXPECT_FALSE(ec->blocked()))
        ec->block_sc();

    ec->return_to_user();
}

template <bool C> Delegate_result_void Ec::delegate()
{
    Ec* ec = current()->rcap;
    assert(ec);

    Ec* src = C ? ec : current();
    Ec* dst = C ? current() : ec;

    bool user = C || dst->cont == ret_user_sysexit;

    return dst->pd->xfer_items(
        src->pd, user ? dst->utcb->xlt : Crd(0),
        user ? dst->utcb->del : Crd(Crd::MEM, (dst->cont == ret_user_iret ? dst->regs.cr2 : 0) >> PAGE_BITS),
        src->utcb->xfer(), user ? dst->utcb->xfer() : nullptr, src->utcb->ti());
}

template <void (*C)()> void Ec::send_msg()
{
    Exc_regs* r = &current()->regs;

    Pt* pt = capability_cast<Pt>(Space_obj::lookup(current()->evt + r->dst_portal));

    if (EXPECT_FALSE(not pt)) {
        die("PT not found");
    }

    Ec* ec = pt->ec;

    if (EXPECT_FALSE(current()->cpu != ec->xcpu))
        die("PT wrong CPU");

    if (EXPECT_TRUE(!ec->cont)) {
        current()->cont = C;
        current()->set_partner(ec);
        current()->regs.mtd = pt->mtd.val;
        ec->cont = recv_kern;
        ec->regs.set_pt(pt->id);
        ec->regs.set_ip(pt->ip);
        ec->return_to_user();
    }

    ec->help(send_msg<C>);

    die("IPC Timeout");
}

void Ec::sys_call()
{
    Sys_call* s = static_cast<Sys_call*>(current()->sys_regs());
    Pt* pt = capability_cast<Pt>(Space_obj::lookup(s->pt()));

    if (EXPECT_FALSE(not pt)) {
        sys_finish<Sys_regs::BAD_CAP>();
    }

    Ec* ec = pt->ec;

    if (EXPECT_FALSE(current()->cpu != ec->xcpu))
        sys_finish<Sys_regs::BAD_CPU>();

    if (EXPECT_TRUE(!ec->cont)) {
        current()->cont = ret_user_sysexit;
        current()->set_partner(ec);
        ec->cont = recv_user;
        ec->regs.set_pt(pt->id);
        ec->regs.set_ip(pt->ip);
        ec->return_to_user();
    }

    if (EXPECT_TRUE(!(s->flags() & Sys_call::DISABLE_BLOCKING)))
        ec->help(sys_call);

    sys_finish<Sys_regs::COM_TIM>();
}

void Ec::recv_kern()
{
    Ec* ec = current()->rcap;

    bool fpu = false;

    if (ec->cont == ret_user_iret)
        fpu = current()->utcb->load_exc(&ec->regs);
    else if (ec->cont == ret_user_vmresume)
        fpu = current()->utcb->load_vmx(&ec->regs);

    if (EXPECT_FALSE(fpu))
        ec->transfer_fpu(current());

    ret_user_sysexit();
}

void Ec::recv_user()
{
    Ec* ec = current()->rcap;

    ec->utcb->save(current()->utcb.get());

    if (EXPECT_FALSE(ec->utcb->tcnt()))
        delegate<true>().unwrap("Failed to delegate items in recv_user");

    ret_user_sysexit();
}

void Ec::reply(void (*c)(), Sm* sm)
{
    current()->cont = c;

    if (EXPECT_FALSE(current()->glb))
        Sc::schedule(true);

    Ec* ec = current()->rcap;

    if (EXPECT_FALSE(!ec))
        Sc::current()->ec->activate();

    bool clr = ec->clr_partner();

    if (Sc::current()->ec == ec && Sc::current()->last_ref())
        Sc::schedule(true);

    if (sm)
        sm->dn(false, 0, ec, clr);

    if (!clr)
        Sc::current()->ec->activate();

    ec->return_to_user();
}

void Ec::sys_reply()
{
    Sm* sm = nullptr;

    if (Ec* ec = current()->rcap; EXPECT_TRUE(ec)) {

        Sys_reply* r = static_cast<Sys_reply*>(current()->sys_regs());
        if (EXPECT_FALSE(r->sm())) {
            sm = capability_cast<Sm>(Space_obj::lookup(r->sm()));

            if (sm and ec->cont == ret_user_sysexit) {
                ec->cont = sys_call;
            }
        }

        Utcb* src = current()->utcb.get();

        if (EXPECT_FALSE(src->tcnt()))
            delegate<false>().unwrap("Failed to delegate items during reply");

        bool fpu = false;

        if (EXPECT_TRUE(ec->cont == ret_user_sysexit))
            src->save(ec->utcb.get());
        else if (ec->cont == ret_user_iret)
            fpu = src->save_exc(&ec->regs);
        else if (ec->cont == ret_user_vmresume)
            fpu = src->save_vmx(&ec->regs);

        if (EXPECT_FALSE(fpu))
            current()->transfer_fpu(ec);
    }

    reply(nullptr, sm);
}

void Ec::sys_create_pd()
{
    Sys_create_pd* r = static_cast<Sys_create_pd*>(current()->sys_regs());

    trace(TRACE_SYSCALL, "EC:%p SYS_CREATE PD:%#lx", current(), r->sel());

    Capability parent_pd_cap = Space_obj::lookup(r->pd());
    Pd* parent_pd = capability_cast<Pd>(parent_pd_cap, Pd::PERM_OBJ_CREATION);

    if (EXPECT_FALSE(not parent_pd)) {
        trace(TRACE_ERROR, "%s: Non-PD CAP (%#lx)", __func__, r->pd());
        sys_finish<Sys_regs::BAD_CAP>();
    }

    Pd* pd = new Pd(Pd::current(), r->sel(), parent_pd_cap.prm(),
                    (r->is_passthrough() and parent_pd->is_passthrough) ? Pd::IS_PASSTHROUGH : 0);
    if (!Space_obj::insert_root(pd)) {
        trace(TRACE_ERROR, "%s: Non-NULL CAP (%#lx)", __func__, r->sel());
        delete pd;
        sys_finish<Sys_regs::BAD_CAP>();
    }

    Crd crd = r->crd();

    pd->del_crd(Pd::current(), Crd(Crd::OBJ), crd).unwrap("Failed to delegate into newly created PD");

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_create_ec()
{
    Sys_create_ec* r = static_cast<Sys_create_ec*>(current()->sys_regs());

    trace(TRACE_SYSCALL, "EC:%p SYS_CREATE EC:%#lx CPU:%#x UPAGE:%#lx ESP:%#lx EVT:%#x", current(), r->sel(),
          r->cpu(), r->user_page(), r->esp(), r->evt());

    if (EXPECT_FALSE(!Hip::cpu_online(r->cpu()))) {
        trace(TRACE_ERROR, "%s: Invalid CPU (%#x)", __func__, r->cpu());
        sys_finish<Sys_regs::BAD_CPU>();
    }

    if (EXPECT_FALSE(r->is_vcpu() && !(Hip::feature() & Hip::FEAT_VMX))) {
        trace(TRACE_ERROR, "%s: VCPUs not supported", __func__);
        sys_finish<Sys_regs::BAD_FTR>();
    }

    Pd* pd = capability_cast<Pd>(Space_obj::lookup(r->pd()), Pd::PERM_OBJ_CREATION);

    if (EXPECT_FALSE(not pd)) {
        trace(TRACE_ERROR, "%s: Non-PD CAP (%#lx)", __func__, r->pd());
        sys_finish<Sys_regs::BAD_CAP>();
    }

    if (EXPECT_FALSE(r->user_page() >= USER_ADDR || r->user_page() & PAGE_MASK)) {
        trace(TRACE_ERROR, "%s: Invalid UPAGE address (%#lx)", __func__, r->user_page());
        sys_finish<Sys_regs::BAD_PAR>();
    }

    Ec* ec = new Ec(Pd::current(), r->sel(), pd,
                    r->flags() & 1 ? static_cast<void (*)()>(send_msg<ret_user_iret>) : nullptr, r->cpu(),
                    r->evt(), r->user_page(), r->esp(),
                    (r->is_vcpu() ? Ec::CREATE_VCPU : 0) |
                        (r->use_apic_access_page() ? Ec::USE_APIC_ACCESS_PAGE : 0) |
                        (r->map_user_page_in_owner() ? Ec::MAP_USER_PAGE_IN_OWNER : 0));

    if (!Space_obj::insert_root(ec)) {
        trace(TRACE_ERROR, "%s: Non-NULL CAP (%#lx)", __func__, r->sel());
        delete ec;
        sys_finish<Sys_regs::BAD_CAP>();
    }

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_create_sc()
{
    Sys_create_sc* r = static_cast<Sys_create_sc*>(current()->sys_regs());

    trace(TRACE_SYSCALL, "EC:%p SYS_CREATE SC:%#lx EC:%#lx P:%#x Q:%#x", current(), r->sel(), r->ec(),
          r->qpd().prio(), r->qpd().quantum());
    if (Pd* pd_parent = capability_cast<Pd>(Space_obj::lookup(r->pd()), Pd::PERM_OBJ_CREATION);
        EXPECT_FALSE(not pd_parent)) {
        trace(TRACE_ERROR, "%s: Non-PD CAP (%#lx)", __func__, r->pd());
        sys_finish<Sys_regs::BAD_CAP>();
    }

    Ec* ec = capability_cast<Ec>(Space_obj::lookup(r->ec()), Ec::PERM_CREATE_SC);

    if (EXPECT_FALSE(not ec)) {
        trace(TRACE_ERROR, "%s: Non-EC CAP (%#lx)", __func__, r->ec());
        sys_finish<Sys_regs::BAD_CAP>();
    }

    if (EXPECT_FALSE(!ec->glb)) {
        trace(TRACE_ERROR, "%s: Cannot bind SC", __func__);
        sys_finish<Sys_regs::BAD_CAP>();
    }

    if (EXPECT_FALSE(!r->qpd().prio() || !r->qpd().quantum() || (r->qpd().prio() >= NUM_PRIORITIES))) {
        trace(TRACE_ERROR, "%s: Invalid QPD", __func__);
        sys_finish<Sys_regs::BAD_PAR>();
    }

    Sc* sc = new Sc(Pd::current(), r->sel(), ec, ec->cpu, r->qpd().prio(), r->qpd().quantum());
    if (!Space_obj::insert_root(sc)) {
        trace(TRACE_ERROR, "%s: Non-NULL CAP (%#lx)", __func__, r->sel());
        delete sc;
        sys_finish<Sys_regs::BAD_CAP>();
    }

    sc->remote_enqueue();

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_create_pt()
{
    Sys_create_pt* r = static_cast<Sys_create_pt*>(current()->sys_regs());

    trace(TRACE_SYSCALL, "EC:%p SYS_CREATE PT:%#lx EC:%#lx EIP:%#lx", current(), r->sel(), r->ec(), r->eip());

    if (Pd* pd_parent = capability_cast<Pd>(Space_obj::lookup(r->pd()), Pd::PERM_OBJ_CREATION);
        EXPECT_FALSE(not pd_parent)) {
        trace(TRACE_ERROR, "%s: Non-PD CAP (%#lx)", __func__, r->pd());
        sys_finish<Sys_regs::BAD_CAP>();
    }

    Ec* ec = capability_cast<Ec>(Space_obj::lookup(r->ec()), Ec::PERM_CREATE_PT);

    if (EXPECT_FALSE(not ec)) {
        trace(TRACE_ERROR, "%s: Non-EC CAP (%#lx)", __func__, r->ec());
        sys_finish<Sys_regs::BAD_CAP>();
    }

    if (EXPECT_FALSE(ec->glb)) {
        trace(TRACE_ERROR, "%s: Cannot bind PT", __func__);
        sys_finish<Sys_regs::BAD_CAP>();
    }

    Pt* pt = new Pt(Pd::current(), r->sel(), ec, r->mtd(), r->eip());
    if (!Space_obj::insert_root(pt)) {
        trace(TRACE_ERROR, "%s: Non-NULL CAP (%#lx)", __func__, r->sel());
        delete pt;
        sys_finish<Sys_regs::BAD_CAP>();
    }

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_create_sm()
{
    Sys_create_sm* r = static_cast<Sys_create_sm*>(current()->sys_regs());

    trace(TRACE_SYSCALL, "EC:%p SYS_CREATE SM:%#lx CNT:%lu", current(), r->sel(), r->cnt());

    if (Pd* pd_parent = capability_cast<Pd>(Space_obj::lookup(r->pd()), Pd::PERM_OBJ_CREATION);
        EXPECT_FALSE(not pd_parent)) {
        trace(TRACE_ERROR, "%s: Non-PD CAP (%#lx)", __func__, r->pd());
        sys_finish<Sys_regs::BAD_CAP>();
    }

    Sm* sm = new Sm(Pd::current(), r->sel(), r->cnt());

    if (!Space_obj::insert_root(sm)) {
        trace(TRACE_ERROR, "%s: Non-NULL CAP (%#lx)", __func__, r->sel());
        delete sm;
        sys_finish<Sys_regs::BAD_CAP>();
    }

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_create_kp()
{
    Sys_create_kp* r = static_cast<Sys_create_kp*>(current()->sys_regs());

    trace(TRACE_SYSCALL, "EC:%p SYS_CREATE KP:%#lx", current(), r->sel());

    if (Pd* pd_parent = capability_cast<Pd>(Space_obj::lookup(r->pd()), Pd::PERM_OBJ_CREATION);
        EXPECT_FALSE(not pd_parent)) {
        trace(TRACE_ERROR, "%s: Non-PD CAP (%#lx)", __func__, r->pd());
        sys_finish<Sys_regs::BAD_CAP>();
    }

    Kp* kp{new Kp(Pd::current(), r->sel())};

    if (!Space_obj::insert_root(kp)) {
        trace(TRACE_ERROR, "%s: Non-NULL CAP (%#lx)", __func__, r->sel());
        delete kp;
        sys_finish<Sys_regs::BAD_CAP>();
    }

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_create_vcpu()
{
    [[maybe_unused]] Sys_create_vcpu* r = static_cast<Sys_create_vcpu*>(current()->sys_regs());
    trace(TRACE_SYSCALL, "EC:%p SYS_CREATE VCPU: %#lx", current(), r->sel());

    // This feature is still under construction, thus return "invalid feature requested".
    sys_finish<Sys_regs::BAD_FTR>();
}

void Ec::sys_revoke()
{
    Sys_revoke* r = static_cast<Sys_revoke*>(current()->sys_regs());

    trace(TRACE_SYSCALL, "EC:%p SYS_REVOKE", current());

    Pd* pd = Pd::current();

    if (r->remote()) {
        pd = capability_cast<Pd>(Space_obj::lookup(r->pd()));

        if (EXPECT_FALSE(not pd or not pd->add_ref())) {
            trace(TRACE_ERROR, "%s: Bad PD CAP (%#lx)", __func__, r->pd());
            sys_finish<Sys_regs::BAD_CAP>();
        }
    }

    pd->rev_crd(r->crd(), r->self());

    if (r->remote() && pd->del_rcu())
        Rcu::call(pd);

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_pd_ctrl_lookup()
{
    Sys_pd_ctrl_lookup* s = static_cast<Sys_pd_ctrl_lookup*>(current()->sys_regs());

    trace(TRACE_SYSCALL, "EC:%p SYS_LOOKUP T:%d B:%#lx", current(), s->crd().type(), s->crd().base());

    Space* space;
    Mdb* mdb;
    if ((space = Pd::current()->subspace(s->crd().type())) && (mdb = space->tree_lookup(s->crd().base())))
        s->crd() = Crd(s->crd().type(), mdb->node_base, mdb->node_order, mdb->node_attr);
    else
        s->crd() = Crd(0);

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_pd_ctrl_map_access_page()
{
    Sys_pd_ctrl_map_access_page* s = static_cast<Sys_pd_ctrl_map_access_page*>(current()->sys_regs());

    trace(TRACE_SYSCALL, "EC:%p SYS_MAP_ACCESS_PAGE B:%#lx", current(), s->crd().base());

    Pd* pd = Pd::current();
    Crd crd = s->crd();

    void* access_addr = pd->get_access_page();

    assert(pd and access_addr);

    static constexpr mword ord{0};
    static constexpr mword rights{0x3}; // R+W

    if (crd.type() != Crd::MEM or crd.attr() != rights or crd.order() != ord) {
        sys_finish<Sys_regs::BAD_PAR>();
    }

    mword access_addr_phys = Buddy::ptr_to_phys(access_addr);

    auto cleanup{pd->ept.update({crd.base() << PAGE_BITS, access_addr_phys,
                                 Ept::PTE_R | Ept::PTE_W | Ept::PTE_I | (6 /* WB */ << Ept::PTE_MT_SHIFT),
                                 PAGE_BITS})};

    // XXX Check whether TLB needs to be invalidated.
    cleanup.ignore_tlb_flush();

    sys_finish<Sys_regs::SUCCESS>();
}

static Sys_regs::Status to_syscall_status(Delegate_error del_error)
{
    switch (del_error.error_type) {
    case Delegate_error::type::OUT_OF_MEMORY:
        return Sys_regs::Status::OOM;
    case Delegate_error::type::INVALID_MAPPING:
        return Sys_regs::Status::BAD_PAR;
    }

    UNREACHED;
}

void Ec::sys_pd_ctrl_delegate()
{
    Sys_pd_ctrl_delegate* s = static_cast<Sys_pd_ctrl_delegate*>(current()->sys_regs());
    Xfer xfer = s->xfer();

    trace(TRACE_SYSCALL, "EC:%p SYS_DELEGATE SRC:%#lx DST:%#lx FLAGS:%#lx", current, s->src_pd(), s->dst_pd(),
          xfer.flags());

    Pd* src_pd = capability_cast<Pd>(Space_obj::lookup(s->src_pd()));
    Pd* dst_pd = capability_cast<Pd>(Space_obj::lookup(s->dst_pd()));

    if (EXPECT_FALSE(not(src_pd and dst_pd))) {
        trace(TRACE_ERROR, "%s: Bad PD CAP SRC:%#lx DST:%#lx", __func__, s->src_pd(), s->dst_pd());
        sys_finish<Sys_regs::BAD_CAP>();
    }

    sys_finish(dst_pd->xfer_item(src_pd, s->dst_crd(), s->dst_crd(), xfer)
                   .map([s](Xfer x) -> monostate {
                       s->set_xfer(x);
                       return {};
                   })
                   .map_err([](Delegate_error e) { return to_syscall_status(e.error_type); }));
}

void Ec::sys_pd_ctrl_msr_access()
{
    Sys_pd_ctrl_msr_access* s = static_cast<Sys_pd_ctrl_msr_access*>(current()->sys_regs());
    bool success = false;

    if (EXPECT_FALSE(not Pd::current()->is_passthrough)) {
        trace(TRACE_ERROR, "%s: PD without passthrough permission accessed MSRs", __func__);
        sys_finish<Sys_regs::BAD_CAP>();
    }

    if (s->is_write()) {
        success = Msr::user_write(Msr::Register(s->msr_index()), s->msr_value());
    } else {
        uint64 value = 0;

        success = Msr::user_read(Msr::Register(s->msr_index()), value);
        s->set_msr_value(value);
    }

    if (success) {
        sys_finish<Sys_regs::SUCCESS>();
    } else {
        sys_finish<Sys_regs::BAD_PAR>();
    }
}

void Ec::sys_pd_ctrl()
{
    Sys_pd_ctrl* s = static_cast<Sys_pd_ctrl*>(current()->sys_regs());
    switch (s->op()) {
    case Sys_pd_ctrl::LOOKUP: {
        sys_pd_ctrl_lookup();
    }
    case Sys_pd_ctrl::MAP_ACCESS_PAGE: {
        sys_pd_ctrl_map_access_page();
    }
    case Sys_pd_ctrl::DELEGATE: {
        sys_pd_ctrl_delegate();
    }
    case Sys_pd_ctrl::MSR_ACCESS: {
        sys_pd_ctrl_msr_access();
    }
    };

    sys_finish<Sys_regs::BAD_PAR>();
}

void Ec::sys_ec_ctrl()
{
    Sys_ec_ctrl* r = static_cast<Sys_ec_ctrl*>(current()->sys_regs());

    switch (r->op()) {
    case Sys_ec_ctrl::RECALL: {
        Ec* ec = capability_cast<Ec>(Space_obj::lookup(r->ec()), Ec::PERM_EC_CTRL);

        if (EXPECT_FALSE(not ec)) {
            trace(TRACE_ERROR, "%s: Bad EC CAP (%#lx)", __func__, r->ec());
            sys_finish<Sys_regs::BAD_CAP>();
        }

        if (!(ec->regs.hazard() & HZD_RECALL)) {

            ec->regs.set_hazard(HZD_RECALL);

            if (Cpu::id() != ec->cpu && Ec::remote(ec->cpu) == ec)
                Lapic::send_ipi(ec->cpu, VEC_IPI_RKE);
        }
        break;
    }

    default:
        sys_finish<Sys_regs::BAD_PAR>();
    }

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_sc_ctrl()
{
    Sys_sc_ctrl* r = static_cast<Sys_sc_ctrl*>(current()->sys_regs());
    Sc* sc = capability_cast<Sc>(Space_obj::lookup(r->sc()), Sc::PERM_SC_CTRL);

    if (EXPECT_FALSE(not sc)) {
        trace(TRACE_ERROR, "%s: Bad SC CAP (%#lx)", __func__, r->sc());
        sys_finish<Sys_regs::BAD_CAP>();
    }

    r->set_time((sc->time * 1000) / Lapic::freq_tsc);

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_pt_ctrl()
{
    Sys_pt_ctrl* r = static_cast<Sys_pt_ctrl*>(current()->sys_regs());
    Pt* pt = capability_cast<Pt>(Space_obj::lookup(r->pt()), Pt::PERM_CTRL);

    if (EXPECT_FALSE(not pt)) {
        trace(TRACE_ERROR, "%s: Bad PT CAP (%#lx)", __func__, r->pt());
        sys_finish<Sys_regs::BAD_CAP>();
    }

    pt->set_id(r->id());

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_sm_ctrl()
{
    Sys_sm_ctrl* r = static_cast<Sys_sm_ctrl*>(current()->sys_regs());
    Sm* sm = capability_cast<Sm>(Space_obj::lookup(r->sm()), 1U << r->op());

    if (EXPECT_FALSE(not sm)) {
        trace(TRACE_ERROR, "%s: Bad SM CAP (%#lx)", __func__, r->sm());
        sys_finish<Sys_regs::BAD_CAP>();
    }

    switch (r->op()) {

    case Sys_sm_ctrl::Sm_operation::Up:
        sm->up();
        break;

    case Sys_sm_ctrl::Sm_operation::Down:
        current()->cont = Ec::sys_finish<Sys_regs::SUCCESS, true>;
        sm->dn(r->zc(), r->time());
        break;
    }

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_kp_ctrl_map()
{
    Sys_kp_ctrl_map* r = static_cast<Sys_kp_ctrl_map*>(current()->sys_regs());

    trace(TRACE_SYSCALL, "EC:%p SYS_KP_CTRL_MAP KP:%#lx DST-PD:%#lx DST-ADDR:%#lx", current, r->kp(),
          r->dst_pd(), r->dst_addr());

    Kp* kp = capability_cast<Kp>(Space_obj::lookup(r->kp()), Kp::PERM_KP_CTRL);
    if (EXPECT_FALSE(not kp)) {
        trace(TRACE_ERROR, "%s: Bad KP CAP (%#lx)", __func__, r->kp());
        sys_finish<Sys_regs::BAD_CAP>();
    }

    Pd* user_pd = capability_cast<Pd>(Space_obj::lookup(r->dst_pd()));
    if (EXPECT_FALSE(not user_pd)) {
        trace(TRACE_ERROR, "%s: Bad PD CAP: %#lx", __func__, r->dst_pd());
        sys_finish<Sys_regs::BAD_CAP>();
    }

    if (EXPECT_TRUE(kp->add_user_mapping(user_pd, r->dst_addr()))) {
        sys_finish<Sys_regs::SUCCESS>();
    }

    sys_finish<Sys_regs::BAD_PAR>();
}

void Ec::sys_kp_ctrl_unmap()
{
    Sys_kp_ctrl_unmap* r = static_cast<Sys_kp_ctrl_unmap*>(current()->sys_regs());
    trace(TRACE_SYSCALL, "EC:%p SYS_KP_CTRL_MAP KP:%#lx", current, r->kp());

    Kp* kp = capability_cast<Kp>(Space_obj::lookup(r->kp()), Kp::PERM_KP_CTRL);
    if (EXPECT_FALSE(not kp)) {
        trace(TRACE_ERROR, "%s: Bad KP CAP (%#lx)", __func__, r->kp());
        sys_finish<Sys_regs::BAD_CAP>();
    }

    if (EXPECT_TRUE(kp->remove_user_mapping())) {
        sys_finish<Sys_regs::SUCCESS>();
    }

    sys_finish<Sys_regs::BAD_PAR>();
}

void Ec::sys_kp_ctrl()
{
    Sys_kp_ctrl* r = static_cast<Sys_kp_ctrl*>(current()->sys_regs());

    switch (r->op()) {
    case Sys_kp_ctrl::MAP: {
        sys_kp_ctrl_map();
    }
    case Sys_kp_ctrl::UNMAP: {
        sys_kp_ctrl_unmap();
    }
    };

    sys_finish<Sys_regs::BAD_PAR>();
}

void Ec::sys_assign_pci()
{
    Sys_assign_pci* r = static_cast<Sys_assign_pci*>(current()->sys_regs());
    Pd* pd = capability_cast<Pd>(Space_obj::lookup(r->pd()));

    if (EXPECT_FALSE(not pd)) {
        trace(TRACE_ERROR, "%s: Non-PD CAP (%#lx)", __func__, r->pd());
        sys_finish<Sys_regs::BAD_CAP>();
    }

    Paddr phys;
    unsigned rid;
    if (EXPECT_FALSE(!pd->Space_mem::lookup(r->dev(), &phys) || (rid = Pci::phys_to_rid(phys)) == ~0U)) {
        trace(TRACE_ERROR, "%s: Non-DEV CAP (%#lx)", __func__, r->dev());
        sys_finish<Sys_regs::BAD_DEV>();
    }

    Dmar* dmar = Pci::find_dmar(r->hnt());
    if (EXPECT_FALSE(!dmar)) {
        trace(TRACE_ERROR, "%s: Invalid Hint (%#lx)", __func__, r->hnt());
        sys_finish<Sys_regs::BAD_DEV>();
    }

    dmar->assign(rid, pd);

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_machine_ctrl()
{
    Sys_machine_ctrl* r = static_cast<Sys_machine_ctrl*>(current()->sys_regs());

    if (EXPECT_FALSE(not Pd::current()->is_passthrough)) {
        trace(TRACE_ERROR, "%s: PD without passthrough permission called machine_ctrl", __func__);
        sys_finish<Sys_regs::BAD_CAP>();
    }

    switch (r->op()) {
    case Sys_machine_ctrl::SUSPEND:
        sys_machine_ctrl_suspend();
    case Sys_machine_ctrl::UPDATE_MICROCODE:
        sys_machine_ctrl_update_microcode();

    default:
        sys_finish<Sys_regs::BAD_PAR>();
    }
}

void Ec::sys_machine_ctrl_suspend()
{
    Sys_machine_ctrl_suspend* r = static_cast<Sys_machine_ctrl_suspend*>(current()->sys_regs());

    r->set_waking_vector(Acpi::get_waking_vector(), Sys_machine_ctrl_suspend::mode::REAL_MODE);

    // In case of a successful suspend below, we will not return from the
    // suspend call.
    current()->cont = sys_finish<Sys_regs::SUCCESS>;

    Suspend::suspend(r->slp_typa(), r->slp_typb());

    // Something went wrong.
    sys_finish<Sys_regs::BAD_PAR>();
}

void Ec::sys_machine_ctrl_update_microcode()
{
    Sys_machine_ctrl_update_microcode* r =
        static_cast<Sys_machine_ctrl_update_microcode*>(current()->sys_regs());

    // Hpt::remap has a limit on how much memory is guaranteed to be accessible.
    // To avoid kernel pagefaults, require the size to be less than that.
    if (EXPECT_FALSE(r->size() > Hpt::remap_guaranteed_size)) {
        trace(TRACE_ERROR, "%s: Microcode update too large (%#x)", __func__, r->size());
        sys_finish<Sys_regs::BAD_PAR>();
    }

    // The userspace mapping describes the start of the microcode update BLOB,
    // but the WRMSR instruction expects a pointer to the payload, which starts
    // at offset 48.
    auto kernel_addr{reinterpret_cast<mword>(Hpt::remap(r->update_address(), false)) + 48};
    Msr::write_safe(Msr::IA32_BIOS_UPDT_TRIG, kernel_addr);

    // Microcode loads may expose new CPU features.
    Cpu::update_features();

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_irq_ctrl()
{
    Sys_irq_ctrl* r = static_cast<Sys_irq_ctrl*>(current()->sys_regs());

    if (EXPECT_FALSE(not Pd::current()->is_passthrough)) {
        trace(TRACE_ERROR, "%s: PD without passthrough permission called irq_ctrl", __func__);
        sys_finish<Sys_regs::BAD_CAP>();
    }

    switch (r->op()) {
    case Sys_irq_ctrl::CONFIGURE_VECTOR:
        sys_irq_ctrl_configure_vector();
    case Sys_irq_ctrl::ASSIGN_IOAPIC_PIN:
        sys_irq_ctrl_assign_ioapic_pin();
    case Sys_irq_ctrl::MASK_IOAPIC_PIN:
        sys_irq_ctrl_mask_ioapic_pin();
    case Sys_irq_ctrl::ASSIGN_MSI:
        sys_irq_ctrl_assign_msi();
    case Sys_irq_ctrl::ASSIGN_LVT:
        sys_irq_ctrl_assign_lvt();
    case Sys_irq_ctrl::MASK_LVT:
        sys_irq_ctrl_mask_lvt();

    default:
        // This is currently not reachable, because the above cases are exhaustive, but this can change when
        // we remove cases or the op() parameter gets more bits.
        sys_finish<Sys_regs::BAD_PAR>();
    }
}

// Perform input validation on CPU and vector numbers.
//
// Directly returns to userspace with the appropriate error if the validation fails.
static void sys_irq_ctrl_check_vector_cpu(const char* func, uint16 cpu, uint8 vector)
{
    if (EXPECT_FALSE(vector >= NUM_USER_VECTORS)) {
        trace(TRACE_ERROR, "%s: Invalid interrupt vector (%u)", func, vector);
        Ec::sys_finish<Sys_regs::BAD_PAR>();
    }

    if (EXPECT_FALSE(!Hip::cpu_online(cpu))) {
        trace(TRACE_ERROR, "%s: Invalid CPU (%#x)", func, cpu);
        Ec::sys_finish<Sys_regs::BAD_CPU>();
    }
}

void Ec::sys_irq_ctrl_configure_vector()
{
    Sys_irq_ctrl_configure_vector* r = static_cast<Sys_irq_ctrl_configure_vector*>(current()->sys_regs());

    sys_irq_ctrl_check_vector_cpu(__func__, r->cpu(), r->vector());

    Sm* sm = capability_cast<Sm>(Space_obj::lookup(r->sm()));
    Kp* kp = capability_cast<Kp>(Space_obj::lookup(r->kp()));

    Vector_info new_vector_info;

    if (not sm and not kp) {
        new_vector_info = Vector_info::disabled();

        if (Dmar::ire()) {
            Dmar::clear_irt(Dmar::irt_index(r->cpu(), r->vector()));
        }
    } else {
        if (EXPECT_FALSE(not sm)) {
            trace(TRACE_ERROR, "%s: Non-SM CAP (%#lx)", __func__, r->sm());
            sys_finish<Sys_regs::BAD_CAP>();
        }

        if (EXPECT_FALSE(not kp)) {
            trace(TRACE_ERROR, "%s: Non-KP CAP (%#lx)", __func__, r->kp());
            sys_finish<Sys_regs::BAD_CAP>();
        }

        new_vector_info = Vector_info{kp, r->kp_bit(), sm};
    }

    auto& vector_info{Locked_vector_info::per_vector_info[r->cpu()][r->vector()]};

    if (vector_info->set(new_vector_info)) {
        sys_finish<Sys_regs::SUCCESS>();
    } else {
        // This error code is appropriate, because we can only end up here if the capability reference from
        // the capability space went away after we checked it above.
        Ec::sys_finish<Sys_regs::BAD_CAP>();
    }
}

void Ec::sys_irq_ctrl_assign_ioapic_pin()
{
    Sys_irq_ctrl_assign_ioapic_pin* r = static_cast<Sys_irq_ctrl_assign_ioapic_pin*>(current()->sys_regs());

    sys_irq_ctrl_check_vector_cpu(__func__, r->cpu(), r->vector());

    auto& opt_ioapic{Ioapic::by_id(r->ioapic_id())};

    if (not opt_ioapic.has_value() or r->ioapic_pin() >= opt_ioapic->pin_count()) {
        sys_finish<Sys_regs::BAD_PAR>();
    }

    if (r->level()) {
        Locked_vector_info::per_vector_info[r->cpu()][r->vector()]->set_level_triggered_ioapic_source(
            {r->ioapic_id(), r->ioapic_pin()});
    } else {
        Locked_vector_info::per_vector_info[r->cpu()][r->vector()]->clear_level_triggered_ioapic_source();
    }

    uint32 const aid{Cpu::apic_id[r->cpu()]};

    if (Dmar::ire()) {
        uint16 const irt_index{Dmar::irt_index(r->cpu(), r->vector())};

        Dmar::set_irt(irt_index, opt_ioapic->get_rid(), aid, VEC_USER + r->vector(), r->level());
        opt_ioapic->set_irt_entry_remappable(r->ioapic_pin(), irt_index, VEC_USER + r->vector(), r->level(),
                                             r->active_low());
    } else {
        opt_ioapic->set_irt_entry_compatibility(r->ioapic_pin(), aid, VEC_USER + r->vector(), r->level(),
                                                r->active_low());
    }

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_irq_ctrl_mask_ioapic_pin()
{
    Sys_irq_ctrl_mask_ioapic_pin* r = static_cast<Sys_irq_ctrl_mask_ioapic_pin*>(current()->sys_regs());
    auto& opt_ioapic{Ioapic::by_id(r->ioapic_id())};

    if (not opt_ioapic.has_value() or r->ioapic_pin() >= opt_ioapic->pin_count()) {
        sys_finish<Sys_regs::BAD_PAR>();
    }

    // The user can unmask pins that have not been previously configured. This is benign, because in this
    // case the IOAPIC RTEs are invalid and no interrupt will arive. Also when the IOMMU is enabled, there
    // will not be an IOMMU RTE for the given pin.
    opt_ioapic->set_mask(r->ioapic_pin(), r->mask());
    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_irq_ctrl_assign_msi()
{
    Sys_irq_ctrl_assign_msi* r = static_cast<Sys_irq_ctrl_assign_msi*>(current()->sys_regs());

    sys_irq_ctrl_check_vector_cpu(__func__, r->cpu(), r->vector());

    Paddr phys;
    unsigned rid = 0;
    if (EXPECT_FALSE((!Pd::current()->Space_mem::lookup(r->dev(), &phys) ||
                      ((rid = Pci::phys_to_rid(phys)) == ~0U && (rid = Hpet::phys_to_rid(phys)) == ~0U)))) {
        trace(TRACE_ERROR, "%s: Non-DEV CAP (%#lx)", __func__, r->dev());
        sys_finish<Sys_regs::BAD_DEV>();
    }

    uint32 const aid{Cpu::apic_id[r->cpu()]};

    uint32 msi_addr;
    uint32 msi_data;

    if (Dmar::ire()) {
        uint16 const irt_index{Dmar::irt_index(r->cpu(), r->vector())};

        msi_addr = 0xfee00000 | (1U << 4) | ((0x7fff & irt_index) << 5) | ((irt_index >> 15) << 2);
        msi_data = 0;

        Dmar::set_irt(irt_index, rid, aid, VEC_USER + r->vector(), false /* edge */);
    } else {
        msi_addr = 0xfee00000 | (aid << 12);
        msi_data = VEC_USER + r->vector();
    }

    r->set_msi(msi_addr, msi_data);
    sys_finish<Sys_regs::SUCCESS>();
}

static void sys_irq_ctrl_check_lvt_entry(const char* func, Sys_irq_ctrl::lvt_entry lvt_entry)
{
    // We only allow the thermal sensor interrupt to be configured.
    //
    // The Intel SDM implies that when there are less than 5 LVT entries that the thermal entry is not
    // there. See Vol. 3 Section 10.4.8 "Local APIC Version Register" and 10.5.1 "Local Vector Table".
    if (lvt_entry != Sys_irq_ctrl::LVT_THERM || Lapic::lvt_max() < 5) {
        trace(TRACE_ERROR, "%s: Invalid LVT entry (%u)", func, lvt_entry);
        Ec::sys_finish<Sys_regs::BAD_PAR>();
    }
}

void Ec::sys_irq_ctrl_assign_lvt()
{
    Sys_irq_ctrl_assign_lvt* r = static_cast<Sys_irq_ctrl_assign_lvt*>(current()->sys_regs());

    // Checking the CPU is a bit wasteful here, but it's better than duplicating the input validation check
    // for the vector.
    sys_irq_ctrl_check_vector_cpu(__func__, current()->cpu, r->vector());
    sys_irq_ctrl_check_lvt_entry(__func__, r->lvt_entry());

    Lapic::set_therm_vector(VEC_USER + r->vector());

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_irq_ctrl_mask_lvt()
{
    Sys_irq_ctrl_mask_lvt* r = static_cast<Sys_irq_ctrl_mask_lvt*>(current()->sys_regs());

    sys_irq_ctrl_check_lvt_entry(__func__, r->lvt_entry());

    Lapic::set_therm_mask(r->mask());

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_vcpu_ctrl_run()
{
    [[maybe_unused]] Sys_vcpu_ctrl_run* r = static_cast<Sys_vcpu_ctrl_run*>(current()->sys_regs());
    trace(TRACE_SYSCALL, "EC:%p, SYS_VCPU_CTRL_RUN VCPU: %#lx", current(), r->sel());

    // This feature is still under construction, thus return "invalid feature requested".
    sys_finish<Sys_regs::BAD_FTR>();
}

void Ec::sys_vcpu_ctrl_poke()
{
    [[maybe_unused]] Sys_vcpu_ctrl_poke* r = static_cast<Sys_vcpu_ctrl_poke*>(current()->sys_regs());
    trace(TRACE_SYSCALL, "EC:%p, SYS_VCPU_CTRL_POKE VCPU: %#lx", current(), r->sel());

    // This feature is still under construction, thus return "invalid feature requested".
    sys_finish<Sys_regs::BAD_FTR>();
}

void Ec::sys_vcpu_ctrl()
{
    Sys_vcpu_ctrl* r = static_cast<Sys_vcpu_ctrl*>(current()->sys_regs());

    switch (r->op()) {
    case Sys_vcpu_ctrl::RUN: {
        sys_vcpu_ctrl_run();
    }
    case Sys_vcpu_ctrl::POKE: {
        sys_vcpu_ctrl_poke();
    }
    };

    sys_finish<Sys_regs::BAD_PAR>();
}

void Ec::syscall_handler()
{
    // System call handler functions are all marked noreturn.

    switch (current()->sys_regs()->id()) {
    case hypercall_id::HC_CALL:
        sys_call();
    case hypercall_id::HC_REPLY:
        sys_reply();
    case hypercall_id::HC_REVOKE:
        sys_revoke();

    case hypercall_id::HC_ASSIGN_PCI:
        sys_assign_pci();

    case hypercall_id::HC_CREATE_PD:
        sys_create_pd();
    case hypercall_id::HC_CREATE_EC:
        sys_create_ec();
    case hypercall_id::HC_CREATE_SC:
        sys_create_sc();
    case hypercall_id::HC_CREATE_PT:
        sys_create_pt();
    case hypercall_id::HC_CREATE_SM:
        sys_create_sm();
    case hypercall_id::HC_CREATE_KP:
        sys_create_kp();
    case hypercall_id::HC_CREATE_VCPU:
        sys_create_vcpu();

    case hypercall_id::HC_PD_CTRL:
        sys_pd_ctrl();
    case hypercall_id::HC_EC_CTRL:
        sys_ec_ctrl();
    case hypercall_id::HC_SC_CTRL:
        sys_sc_ctrl();
    case hypercall_id::HC_PT_CTRL:
        sys_pt_ctrl();
    case hypercall_id::HC_SM_CTRL:
        sys_sm_ctrl();
    case hypercall_id::HC_KP_CTRL:
        sys_kp_ctrl();
    case hypercall_id::HC_VCPU_CTRL:
        sys_vcpu_ctrl();

    case hypercall_id::HC_MACHINE_CTRL:
        sys_machine_ctrl();
    case hypercall_id::HC_IRQ_CTRL:
        sys_irq_ctrl();

    default:
        Ec::sys_finish<Sys_regs::BAD_HYP>();
    }
}

template void Ec::sys_finish<Sys_regs::COM_ABT>();
template void Ec::send_msg<Ec::ret_user_vmresume>();
template void Ec::send_msg<Ec::ret_user_iret>();
