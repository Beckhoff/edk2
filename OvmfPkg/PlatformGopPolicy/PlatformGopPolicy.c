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
  return EFI_UNSUPPORTED;
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
