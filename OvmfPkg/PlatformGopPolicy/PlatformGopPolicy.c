/** @file
  Platform GOP Policy

  GOP drivers calls GetVbtData to get the Video BIOS Table of the Intel Graphics
  Device.

  Copyright (C) 1999 - 2019, Intel Corporation. All rights reserved
  Copyright (C) 2021, Beckhoff Automation GmbH & Co. KG. All rights reserved.<BR>

  This program and the accompanying materials are licensed and made available under
  the terms and conditions of the BSD License that accompanies this distribution.
  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php.

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.


**/

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Protocol/FirmwareVolume2.h>
#include <Protocol/PlatformGopPolicy.h>

#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/PciLib.h>

#include <IndustryStandard/IgdOpRegion.h>
#include <IndustryStandard/Pci22.h>

//
// IGD is always located at 0:2.0
//
#define IGD_PCI_BUS   0
#define IGD_PCI_SLOT  2
#define IGD_PCI_FUNC  0

#define IGD_PCI_ASLS_OFFSET  0xFC

#define IGD_PCI_VENDOR_ID  0x8086

PLATFORM_GOP_POLICY_PROTOCOL  mPlatformGOPPolicy;
VBT_HEADER                    *mVbt;

/**
  The function returns the Platform Lid Status. IBV/OEM can customize this code
  for their specific policy action.

  @param[out] CurrentLidStatus  Current LID Status.

  @retval EFI_UNSUPPORTED       Function isn't implemented yet.

**/
EFI_STATUS
EFIAPI
GetPlatformLidStatus (
  OUT LID_STATUS  *CurrentLidStatus
  )
{
  return EFI_UNSUPPORTED;
}

/**
  The function returns the Video Bios Table size and address.

  @param[out] VbtAddress  Physical Address of Video BIOS Table

  @param[out] VbtSize     Size of Video BIOS Table

  @retval EFI_STATUS      Successfully returned Video BIOS Table address and
                          size.

**/
EFI_STATUS
EFIAPI
GetVbtData (
  OUT EFI_PHYSICAL_ADDRESS  *VbtAddress,
  OUT UINT32                *VbtSize
  )
{
  EFI_STATUS              Status;
  IGD_OPREGION_STRUCTURE  *OpRegion;
  VBT_HEADER              *Vbt;
  UINT8                   CheckSum = 0;
  UINT16                  VersionMajor;
  UINT16                  VersionMinor;
  UINT32                  VbtSizeMax = 0;
  UINT16                  VendorId;
  UINT32                  ClassCode;

  //
  // Check if vendor is Intel
  //
  VendorId = PciRead16 (
               PCI_LIB_ADDRESS (
                 IGD_PCI_BUS,
                 IGD_PCI_SLOT,
                 IGD_PCI_FUNC,
                 PCI_VENDOR_ID_OFFSET
                 )
               );
  if (VendorId == 0xFFFF) {
    return EFI_NOT_FOUND;
  } else if (VendorId != IGD_PCI_VENDOR_ID) {
    return EFI_UNSUPPORTED;
  }

  //
  // Check if class is display device
  //
  ClassCode = PciRead32 (
                PCI_LIB_ADDRESS (
                  IGD_PCI_BUS,
                  IGD_PCI_SLOT,
                  IGD_PCI_FUNC,
                  PCI_CLASSCODE_OFFSET & ~0x3
                  )
                );
  if ((((ClassCode >> 24) & 0xFF) != PCI_CLASS_DISPLAY) ||
      (((ClassCode >> 16) & 0xFF) != PCI_CLASS_DISPLAY_VGA) ||
      (((ClassCode >>  8) & 0xFF) != PCI_IF_VGA_VGA))
  {
    return EFI_UNSUPPORTED;
  }

  //
  // Get OpRegion.
  //
  OpRegion = (IGD_OPREGION_STRUCTURE *)(UINTN)PciRead32 (
                                                PCI_LIB_ADDRESS (
                                                  0,
                                                  2,
                                                  0,
                                                  IGD_PCI_ASLS_OFFSET
                                                  )
                                                );
  if (OpRegion == NULL) {
    DEBUG ((
      EFI_D_ERROR,
      "%a: Failed to get OpRegion address\n",
      __FUNCTION__
      ));
  }

  //
  // Check if Intel Graphics Device has an OpRegion.
  //
  if (!OpRegion) {
    DEBUG ((EFI_D_ERROR, "%a: No OpRegion found\n", __FUNCTION__));
    return EFI_UNSUPPORTED;
  }

  //
  // Check OpRegion signature.
  //
  if (CompareMem (OpRegion->Header.SIGN, IGD_OPREGION_HEADER_SIGN, sizeof (OpRegion->Header.SIGN)) != 0) {
    DEBUG ((
      EFI_D_ERROR,
      "%a: Invalid OpRegion signature (%a), expected %a\n",
      __FUNCTION__,
      OpRegion->Header.SIGN,
      IGD_OPREGION_HEADER_SIGN
      ));
    return EFI_INVALID_PARAMETER;
  }

  VersionMajor = OpRegion->Header.OVER >> 24;
  VersionMinor = OpRegion->Header.OVER >> 16 & 0xFF;

  //
  // Only OpRegion v2.0 and higher is supported.
  //
  if (VersionMajor < 2) {
    DEBUG ((
      EFI_D_ERROR,
      "%a: Unsupported OpRegion version %d.%d\n",
      __FUNCTION__,
      VersionMajor,
      VersionMinor
      ));
    return EFI_UNSUPPORTED;
  }

  //
  // If the size of the Video BIOS Table larger then 6K, the Video BIOS Table is
  // stored outside the OpRegion. In that case, the address and size of the
  // Video BIOS Table are stored in MailBox3 (RVDA and RVDS). When OpRegion v2.0
  // is used, RVDA is a physical address. In a virtual environment this physical
  // address is invalid. For that reason, we do not support a Video BIOS Table
  // which is larger than 6K for OpRegion v2.0.
  //
  if ((VersionMajor == 2) &&
      (VersionMinor == 0) &&
      OpRegion->MBox3.RVDA &&
      OpRegion->MBox3.RVDS)
  {
    DEBUG ((
      EFI_D_ERROR,
      "%a: VBT larger than 6K is unsupported for OpRegion v2.0\n",
      __FUNCTION__
      ));
    return EFI_UNSUPPORTED;
  }

  //
  // Get Video BIOS Table size.
  //
  VbtSizeMax = IGD_OPREGION_VBT_SIZE_6K;
  if (OpRegion->MBox3.RVDA && OpRegion->MBox3.RVDS) {
    Vbt        = (VBT_HEADER *)((UINT8 *)OpRegion + OpRegion->MBox3.RVDA);
    VbtSizeMax = *VbtSize;
  } else {
    Vbt        = (VBT_HEADER *)&OpRegion->MBox4;
    VbtSizeMax = IGD_OPREGION_VBT_SIZE_6K;
  }

  //
  // Free old Video BIOS Table.
  //
  if (mVbt) {
    Status = gBS->FreePages (
                    (EFI_PHYSICAL_ADDRESS)mVbt,
                    EFI_SIZE_TO_PAGES (VbtSizeMax)
                    );
  }

  //
  // Allocate Video BIOS Table.
  //
  mVbt   = (VBT_HEADER *)(SIZE_4GB - 1);
  Status = gBS->AllocatePages (
                  AllocateMaxAddress,
                  EfiReservedMemoryType,
                  EFI_SIZE_TO_PAGES (VbtSizeMax),
                  (EFI_PHYSICAL_ADDRESS *)&mVbt
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      EFI_D_ERROR,
      "%a: AllocatePages failed for VBT size 0x%x status %d\n",
      __FUNCTION__,
      VbtSizeMax,
      Status
      ));
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Copy Video BIOS Table to allocated space.
  //
  ZeroMem ((VOID *)mVbt, VbtSizeMax);
  CopyMem ((VOID *)mVbt, (VOID *)Vbt, Vbt->Table_Size);

  //
  // Fix checksum.
  //
  for (UINT32 i = 0; i < mVbt->Table_Size; i++) {
    CheckSum = (CheckSum + ((UINT8 *)mVbt)[i]) & 0xFF;
  }

  mVbt->Checksum += (0x100 - CheckSum);

  //
  // Return Video BIOS Table address and size.
  //
  *VbtAddress = (EFI_PHYSICAL_ADDRESS)mVbt;
  *VbtSize    = mVbt->Table_Size;
  DEBUG ((
    DEBUG_INFO,
    "%a: VBT Version %d size 0x%x\n",
    __FUNCTION__,
    ((VBT_BIOS_DATA_HEADER *)((VOID *)mVbt + mVbt->Bios_Data_Offset))->BDB_Version,
    mVbt->Table_Size
    ));

  return EFI_SUCCESS;
}

/**
  Entry point for the Platform GOP Policy Driver.

  @param[in] ImageHandle  Image handle of this driver.

  @param[in] SystemTable  Global system service table.

  @retval EFI_SUCCESS     Initialization complete.

  @return                 Error codes propagated from underlying functions.
**/
EFI_STATUS
EFIAPI
PlatformGOPPolicyEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )

{
  EFI_STATUS  Status;

  gBS = SystemTable->BootServices;

  mPlatformGOPPolicy.Revision             = PLATFORM_GOP_POLICY_PROTOCOL_REVISION_01;
  mPlatformGOPPolicy.GetPlatformLidStatus = GetPlatformLidStatus;
  mPlatformGOPPolicy.GetVbtData           = GetVbtData;

  //
  // Install protocol to allow access to this Policy.
  //
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ImageHandle,
                  &gPlatformGOPPolicyGuid,
                  &mPlatformGOPPolicy,
                  NULL
                  );

  return Status;
}
