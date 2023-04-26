/*
 * Advanced Configuration and Power Interface (ACPI)
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012-2013 Udo Steinberg, Intel Corporation.
 * Copyright (C) 2014 Udo Steinberg, FireEye, Inc.
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

#include "acpi_dmar.hpp"
#include "cmdline.hpp"
#include "dmar.hpp"
#include "hip.hpp"
#include "hpet.hpp"
#include "ioapic.hpp"
#include "pci.hpp"
#include "pd.hpp"
#include "stdio.hpp"

void Acpi_dmar::parse() const
{
    Dmar* dmar = new Dmar(static_cast<Paddr>(phys));

    if (flags & 1)
        Pci::claim_all(dmar);

    for (Acpi_scope const* s = scope;
         s < reinterpret_cast<Acpi_scope*>(reinterpret_cast<mword>(this) + length);
         s = reinterpret_cast<Acpi_scope*>(reinterpret_cast<mword>(s) + s->length)) {

        switch (s->type) {
        case 1 ... 2:
            if (not Pci::claim_dev(dmar, s->rid())) {
                trace(TRACE_ERROR, "Failed to claim PCI device %#x", s->rid());
            }
            break;
        case 3:
            // See Acpi_table_madt::parse_ioapic. On systems with broken IOAPIC IDs in the MADT, we see them
            // in the DMAR table as well.
            if (not Ioapic::claim_dev(s->rid(), s->id & Ioapic::ID_MASK)) {
                trace(TRACE_ERROR, "Failed to claim IOAPIC %#x with RID %#x", s->id, s->rid());
            }
            break;
        case 4:
            if (not Hpet::claim_dev(s->rid(), s->id)) {
                trace(TRACE_ERROR, "Failed to claim HPET %#x with RID %#x", s->id, s->rid());
            }
            break;
        }
    }
}

void Acpi_rmrr::parse() const
{
    for (uint64 hpa = base & ~PAGE_MASK; hpa < limit; hpa += PAGE_SIZE) {
        Pd::kern->dpt.update({hpa, hpa, Dpt::PTE_R | Dpt::PTE_W, PAGE_BITS});
    }

    for (Acpi_scope const* s = scope;
         s < reinterpret_cast<Acpi_scope*>(reinterpret_cast<mword>(this) + length);
         s = reinterpret_cast<Acpi_scope*>(reinterpret_cast<mword>(s) + s->length)) {

        Dmar* dmar = nullptr;

        switch (s->type) {
        case 1:
            dmar = Pci::find_dmar(s->rid());
            break;
        }

        if (dmar)
            dmar->assign(s->rid(), &Pd::kern);
    }
}

void Acpi_table_dmar::parse() const
{
    if (true)
        return;

    for (Acpi_remap const* r = remap;
         r < reinterpret_cast<Acpi_remap*>(reinterpret_cast<mword>(this) + length);
         r = reinterpret_cast<Acpi_remap*>(reinterpret_cast<mword>(r) + r->length)) {
        switch (r->type) {
        case Acpi_remap::DMAR:
            static_cast<Acpi_dmar const*>(r)->parse();
            break;
        case Acpi_remap::RMRR:
            static_cast<Acpi_rmrr const*>(r)->parse();
            break;
        }
    }

    Dmar::enable(flags);

    Hip::set_feature(Hip::FEAT_IOMMU);
}
