/*
 * Copyright (c) 2020, Rebecca Cran <rebecca@bsdio.com>
 * Copyright (c) 2008 - 2012, Intel Corporation. All rights reserved.<BR>
 * Copyright (C) 2012, Red Hat, Inc.
 * Copyright (c) 2014, Pluribus Networks, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
#include "AcpiPlatform.h"

#include <Library/BaseMemoryLib.h>
#include <Library/BhyveFwCtlLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/QemuFwCfgLib.h>             // QemuFwCfgFindFile()

#define BHYVE_ACPI_PHYSICAL_ADDRESS  ((UINTN)0x000F2400)
#define BHYVE_BIOS_PHYSICAL_END      ((UINTN)0x00100000)

#pragma pack (1)

typedef struct {
  EFI_ACPI_DESCRIPTION_HEADER    Header;
  UINT64                         Tables[0];
} EFI_ACPI_2_0_EXTENDED_SYSTEM_DESCRIPTION_TABLE;

#pragma pack ()

STATIC
EFI_STATUS
EFIAPI
BhyveGetCpuCount (
  OUT UINT32  *CpuCount
  )
{
  FIRMWARE_CONFIG_ITEM  Item;
  UINTN                 Size;

  if (QemuFwCfgIsAvailable ()) {
    if (EFI_ERROR (QemuFwCfgFindFile ("opt/bhyve/hw.ncpu", &Item, &Size))) {
      return EFI_NOT_FOUND;
    } else if (Size != sizeof (*CpuCount)) {
      return EFI_BAD_BUFFER_SIZE;
    }

    QemuFwCfgSelectItem (Item);
    QemuFwCfgReadBytes (Size, CpuCount);

    return EFI_SUCCESS;
  }

  //
  // QemuFwCfg not available, try BhyveFwCtl.
  //
  Size = sizeof (*CpuCount);
  if (BhyveFwCtlGet ("hw.ncpu", CpuCount, &Size) == RETURN_SUCCESS) {
    return EFI_SUCCESS;
  }

  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
BhyveInstallAcpiMadtTable (
  IN   EFI_ACPI_TABLE_PROTOCOL  *AcpiProtocol,
  IN   VOID                     *AcpiTableBuffer,
  IN   UINTN                    AcpiTableBufferSize,
  OUT  UINTN                    *TableKey
  )
{
  UINT32                                               CpuCount;
  UINTN                                                NewBufferSize;
  EFI_ACPI_1_0_MULTIPLE_APIC_DESCRIPTION_TABLE_HEADER  *Madt;
  EFI_ACPI_1_0_PROCESSOR_LOCAL_APIC_STRUCTURE          *LocalApic;
  EFI_ACPI_1_0_IO_APIC_STRUCTURE                       *IoApic;
  EFI_ACPI_1_0_INTERRUPT_SOURCE_OVERRIDE_STRUCTURE     *Iso;
  VOID                                                 *Ptr;
  UINTN                                                Loop;
  EFI_STATUS                                           Status;

  ASSERT (AcpiTableBufferSize >= sizeof (EFI_ACPI_DESCRIPTION_HEADER));

  // Query the host for the number of vCPUs
  Status = BhyveGetCpuCount (&CpuCount);
  if (!EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "Retrieved CpuCount %d\n", CpuCount));
    ASSERT (CpuCount >= 1);
  } else {
    DEBUG ((DEBUG_INFO, "CpuCount retrieval error\n"));
    CpuCount = 1;
  }

  NewBufferSize = 1                     * sizeof (*Madt) +
                  CpuCount              * sizeof (*LocalApic) +
                  1                     * sizeof (*IoApic) +
                  1                     * sizeof (*Iso);

  Madt = AllocatePool (NewBufferSize);
  if (Madt == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem (&(Madt->Header), AcpiTableBuffer, sizeof (EFI_ACPI_DESCRIPTION_HEADER));
  Madt->Header.Length    = (UINT32)NewBufferSize;
  Madt->LocalApicAddress = 0xFEE00000;
  Madt->Flags            = EFI_ACPI_1_0_PCAT_COMPAT;
  Ptr                    = Madt + 1;

  LocalApic = Ptr;
  for (Loop = 0; Loop < CpuCount; ++Loop) {
    LocalApic->Type            = EFI_ACPI_1_0_PROCESSOR_LOCAL_APIC;
    LocalApic->Length          = sizeof (*LocalApic);
    LocalApic->AcpiProcessorId = (UINT8)Loop;
    LocalApic->ApicId          = (UINT8)Loop;
    LocalApic->Flags           = 1; // enabled
    ++LocalApic;
  }

  Ptr = LocalApic;

  IoApic                   = Ptr;
  IoApic->Type             = EFI_ACPI_1_0_IO_APIC;
  IoApic->Length           = sizeof (*IoApic);
  IoApic->IoApicId         = (UINT8)CpuCount;
  IoApic->Reserved         = EFI_ACPI_RESERVED_BYTE;
  IoApic->IoApicAddress    = 0xFEC00000;
  IoApic->SystemVectorBase = 0x00000000;
  Ptr                      = IoApic + 1;

  //
  // IRQ0 (8254 Timer) => IRQ2 (PIC) Interrupt Source Override Structure
  //
  Iso                              = Ptr;
  Iso->Type                        = EFI_ACPI_1_0_INTERRUPT_SOURCE_OVERRIDE;
  Iso->Length                      = sizeof (*Iso);
  Iso->Bus                         = 0x00; // ISA
  Iso->Source                      = 0x00; // IRQ0
  Iso->GlobalSystemInterruptVector = 0x00000002;
  Iso->Flags                       = 0x0000; // Conforms to specs of the bus
  Ptr                              = Iso + 1;

  ASSERT ((UINTN)((UINT8 *)Ptr - (UINT8 *)Madt) == NewBufferSize);
  Status = InstallAcpiTable (AcpiProtocol, Madt, NewBufferSize, TableKey);

  FreePool (Madt);

  return Status;
}

EFI_STATUS
EFIAPI
BhyveInstallAcpiTable (
  IN   EFI_ACPI_TABLE_PROTOCOL  *AcpiProtocol,
  IN   VOID                     *AcpiTableBuffer,
  IN   UINTN                    AcpiTableBufferSize,
  OUT  UINTN                    *TableKey
  )
{
  EFI_ACPI_DESCRIPTION_HEADER        *Hdr;
  EFI_ACPI_TABLE_INSTALL_ACPI_TABLE  TableInstallFunction;

  Hdr = (EFI_ACPI_DESCRIPTION_HEADER *)AcpiTableBuffer;
  switch (Hdr->Signature) {
    case EFI_ACPI_1_0_APIC_SIGNATURE:
      TableInstallFunction = BhyveInstallAcpiMadtTable;
      break;
    default:
      TableInstallFunction = InstallAcpiTable;
  }

  return TableInstallFunction (
           AcpiProtocol,
           AcpiTableBuffer,
           AcpiTableBufferSize,
           TableKey
           );
}

/**
  Get the address of bhyve's ACPI Root System Description Pointer (RSDP).

  @param  RsdpPtr             Return pointer to RSDP.

  @return EFI_SUCCESS         Bhyve's RSDP successfully found.
  @return EFI_NOT_FOUND       Couldn't find bhyve's RSDP.
  @return EFI_UNSUPPORTED     Revision is lower than 2.
  @return EFI_PROTOCOL_ERROR  Invalid RSDP found.

**/
EFI_STATUS
EFIAPI
BhyveGetAcpiRsdp (
  OUT   EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER  **RsdpPtr
  )
{
  UINTN                                         RsdpAddress;
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER  *Rsdp;

  if (RsdpPtr == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Detect the RSDP
  //
  for (RsdpAddress = BHYVE_ACPI_PHYSICAL_ADDRESS;
       RsdpAddress < BHYVE_BIOS_PHYSICAL_END;
       RsdpAddress += 0x10)
  {
    Rsdp = NUMERIC_VALUE_AS_POINTER (
             EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER,
             RsdpAddress
             );
    if (Rsdp->Signature != EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER_SIGNATURE) {
      continue;
    }

    if (Rsdp->Revision < 2) {
      DEBUG ((DEBUG_INFO, "%a: unsupported RSDP found\n", __FUNCTION__));
      return EFI_UNSUPPORTED;
    }

    //
    // For ACPI 1.0/2.0/3.0 the checksum of first 20 bytes should be 0.
    // For ACPI 2.0/3.0 the checksum of the entire table should be 0.
    //
    UINT8  Sum = CalculateCheckSum8 (
                   (CONST UINT8 *)Rsdp,
                   sizeof (EFI_ACPI_1_0_ROOT_SYSTEM_DESCRIPTION_POINTER)
                   );
    if (Sum != 0) {
      DEBUG ((
        DEBUG_INFO,
        "%a: RSDP header checksum not valid: 0x%02x\n",
        __FUNCTION__,
        Sum
        ));
      return EFI_PROTOCOL_ERROR;
    }

    Sum = CalculateCheckSum8 (
            (CONST UINT8 *)Rsdp,
            sizeof (EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER)
            );
    if (Sum != 0) {
      DEBUG ((
        DEBUG_INFO,
        "%a: RSDP table checksum not valid: 0x%02x\n",
        __FUNCTION__,
        Sum
        ));
      return EFI_PROTOCOL_ERROR;
    }

    //
    // RSDP was found and is valid
    //
    *RsdpPtr = Rsdp;

    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_INFO, "%a: RSDP not found\n", __FUNCTION__));
  return EFI_NOT_FOUND;
}

/**
  Get bhyve's ACPI tables from the RSDP. And install bhyve's ACPI tables
  into the RSDT/XSDT using InstallAcpiTable.

  @param  AcpiProtocol        Protocol instance pointer.

  @return EFI_SUCCESS         All tables were successfully inserted.
  @return EFI_UNSUPPORTED     Bhyve's ACPI tables doesn't include a XSDT.
  @return EFI_PROTOCOL_ERROR  Invalid XSDT found.

  @return                     Error codes propagated from underlying functions.
**/
EFI_STATUS
EFIAPI
InstallBhyveTables (
  IN   EFI_ACPI_TABLE_PROTOCOL  *AcpiProtocol
  )
{
  EFI_STATUS                                    Status;
  UINTN                                         TableHandle;
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER  *Rsdp;
  EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE  *Facs;
  EFI_ACPI_DESCRIPTION_HEADER                   *Dsdt;

  Rsdp = NULL;
  Facs = NULL;
  Dsdt = NULL;

  //
  // Try to find bhyve ACPI tables
  //
  Status = BhyveGetAcpiRsdp (&Rsdp);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "%a: can't get RSDP (%r)\n", __FUNCTION__, Status));
    return Status;
  }

  //
  // Bhyve should always provide a XSDT
  //
  EFI_ACPI_2_0_EXTENDED_SYSTEM_DESCRIPTION_TABLE *CONST  Xsdt =
    NUMERIC_VALUE_AS_POINTER (
      EFI_ACPI_2_0_EXTENDED_SYSTEM_DESCRIPTION_TABLE,
      Rsdp->XsdtAddress
      );

  if (Xsdt == NULL) {
    DEBUG ((DEBUG_INFO, "%a: XSDT not found\n", __FUNCTION__));
    return EFI_UNSUPPORTED;
  }

  if (Xsdt->Header.Length < sizeof (EFI_ACPI_DESCRIPTION_HEADER)) {
    DEBUG ((DEBUG_INFO, "%a: invalid XSDT length\n", __FUNCTION__));
    return EFI_PROTOCOL_ERROR;
  }

  //
  // Install ACPI tables
  //
  CONST UINTN  NumberOfTableEntries =
    (Xsdt->Header.Length - sizeof (Xsdt->Header)) / sizeof (UINT64);

  for (UINTN Index = 0; Index < NumberOfTableEntries; Index++) {
    EFI_ACPI_DESCRIPTION_HEADER *CONST  CurrentTable =
      NUMERIC_VALUE_AS_POINTER (
        EFI_ACPI_DESCRIPTION_HEADER,
        Xsdt->Tables[Index]
        );
    Status = AcpiProtocol->InstallAcpiTable (
                             AcpiProtocol,
                             CurrentTable,
                             CurrentTable->Length,
                             &TableHandle
                             );
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_INFO,
        "%a: failed to install ACPI table %c%c%c%c (%r)\n",
        __FUNCTION__,
        NUMERIC_VALUE_AS_POINTER (UINT8, CurrentTable->Signature)[0],
        NUMERIC_VALUE_AS_POINTER (UINT8, CurrentTable->Signature)[1],
        NUMERIC_VALUE_AS_POINTER (UINT8, CurrentTable->Signature)[2],
        NUMERIC_VALUE_AS_POINTER (UINT8, CurrentTable->Signature)[3],
        Status
        ));
      return Status;
    }

    if (CurrentTable->Signature == EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE_SIGNATURE) {
      EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *CONST  Fadt =
        (EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *CONST)CurrentTable;
      if (Fadt->XFirmwareCtrl) {
        Facs = NUMERIC_VALUE_AS_POINTER (
                 EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE,
                 Fadt->XFirmwareCtrl
                 );
      } else {
        Facs = NUMERIC_VALUE_AS_POINTER (
                 EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE,
                 Fadt->FirmwareCtrl
                 );
      }

      if (Fadt->XDsdt) {
        Dsdt = NUMERIC_VALUE_AS_POINTER (
                 EFI_ACPI_DESCRIPTION_HEADER,
                 Fadt->XDsdt
                 );
      } else {
        Dsdt = NUMERIC_VALUE_AS_POINTER (
                 EFI_ACPI_DESCRIPTION_HEADER,
                 Fadt->Dsdt
                 );
      }
    }
  }

  //
  // Install FACS
  //
  if (Facs != NULL) {
    Status = AcpiProtocol->InstallAcpiTable (
                             AcpiProtocol,
                             Facs,
                             Facs->Length,
                             &TableHandle
                             );
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_INFO,
        "%a: failed to install FACS (%r)\n",
        __FUNCTION__,
        Status
        ));
      return Status;
    }
  }

  //
  // Install DSDT
  // If it's not found, something bad happened. Don't continue execution.
  //
  if (Dsdt == NULL) {
    DEBUG ((DEBUG_ERROR, "%a: failed to find DSDT\n", __FUNCTION__));
    CpuDeadLoop ();
  }

  Status = AcpiProtocol->InstallAcpiTable (
                           AcpiProtocol,
                           Dsdt,
                           Dsdt->Length,
                           &TableHandle
                           );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_INFO,
      "%a: failed to install DSDT (%r)\n",
      __FUNCTION__,
      Status
      ));
    return Status;
  }

  return EFI_SUCCESS;
}
