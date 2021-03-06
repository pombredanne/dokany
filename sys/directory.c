/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2015 - 2019 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
  Copyright (C) 2017 Google, Inc.
  Copyright (C) 2007 - 2011 Hiroki Asakawa <info@dokan-dev.net>

  http://dokan-dev.github.io

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the Free
Software Foundation; either version 3 of the License, or (at your option) any
later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along
with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "dokan.h"

NTSTATUS
DokanQueryDirectory(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp);

NTSTATUS
DokanNotifyChangeDirectory(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp);

NTSTATUS
DokanDispatchDirectoryControl(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp) {
  NTSTATUS status = STATUS_NOT_IMPLEMENTED;
  PFILE_OBJECT fileObject;
  PIO_STACK_LOCATION irpSp;
  PDokanVCB vcb;

  __try {
    DDbgPrint("==> DokanDirectoryControl\n");

    irpSp = IoGetCurrentIrpStackLocation(Irp);
    fileObject = irpSp->FileObject;

    if (fileObject == NULL) {
      DDbgPrint("   fileObject is NULL\n");
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }

    vcb = DeviceObject->DeviceExtension;
    if (GetIdentifierType(vcb) != VCB ||
        !DokanCheckCCB(vcb->Dcb, fileObject->FsContext2)) {
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }

    DDbgPrint("  ProcessId %lu\n", IoGetRequestorProcessId(Irp));
    DokanPrintFileName(fileObject);

    if (irpSp->MinorFunction == IRP_MN_QUERY_DIRECTORY) {
      status = DokanQueryDirectory(DeviceObject, Irp);

    } else if (irpSp->MinorFunction == IRP_MN_NOTIFY_CHANGE_DIRECTORY) {
      status = DokanNotifyChangeDirectory(DeviceObject, Irp);
    } else {
      DDbgPrint("  invalid minor function\n");
      status = STATUS_INVALID_PARAMETER;
    }

  } __finally {

    DokanCompleteIrpRequest(Irp, status, 0);

    DDbgPrint("<== DokanDirectoryControl\n");
  }

  return status;
}

NTSTATUS
DokanQueryDirectory(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp) {
  PFILE_OBJECT fileObject;
  PIO_STACK_LOCATION irpSp;
  PDokanVCB vcb;
  PDokanCCB ccb;
  PDokanFCB fcb;
  NTSTATUS status;
  ULONG eventLength;
  PEVENT_CONTEXT eventContext;
  ULONG index;
  BOOLEAN initial;
  ULONG flags = 0;

  irpSp = IoGetCurrentIrpStackLocation(Irp);
  fileObject = irpSp->FileObject;

  vcb = DeviceObject->DeviceExtension;
  if (GetIdentifierType(vcb) != VCB) {
    return STATUS_INVALID_PARAMETER;
  }

  ccb = fileObject->FsContext2;
  if (ccb == NULL) {
    return STATUS_INVALID_PARAMETER;
  }
  ASSERT(ccb != NULL);

  if (irpSp->Flags & SL_INDEX_SPECIFIED) {
    DDbgPrint("  index specified %d\n",
              irpSp->Parameters.QueryDirectory.FileIndex);
  }
  if (irpSp->Flags & SL_RETURN_SINGLE_ENTRY) {
    DDbgPrint("  return single entry\n");
  }
  if (irpSp->Flags & SL_RESTART_SCAN) {
    DDbgPrint("  restart scan\n");
  }
  if (irpSp->Parameters.QueryDirectory.FileName) {
    DDbgPrint("  pattern:%wZ\n", irpSp->Parameters.QueryDirectory.FileName);
  }

  switch (irpSp->Parameters.QueryDirectory.FileInformationClass) {
  case FileDirectoryInformation:
    DDbgPrint("  FileDirectoryInformation\n");
    break;
  case FileIdFullDirectoryInformation:
    DDbgPrint("  FileIdFullDirectoryInformation\n");
    break;
  case FileFullDirectoryInformation:
    DDbgPrint("  FileFullDirectoryInformation\n");
    break;
  case FileNamesInformation:
    DDbgPrint("  FileNamesInformation\n");
    break;
  case FileBothDirectoryInformation:
    DDbgPrint("  FileBothDirectoryInformation\n");
    break;
  case FileIdBothDirectoryInformation:
    DDbgPrint("  FileIdBothDirectoryInformation\n");
    break;
  default:
    DDbgPrint("  unknown FileInfoClass %d\n",
              irpSp->Parameters.QueryDirectory.FileInformationClass);
    break;
  }

  fcb = ccb->Fcb;
  ASSERT(fcb != NULL);

  // make a MDL for UserBuffer that can be used later on another thread context
  if (Irp->MdlAddress == NULL) {
    status = DokanAllocateMdl(Irp, irpSp->Parameters.QueryDirectory.Length);
    if (!NT_SUCCESS(status)) {
      return status;
    }
    flags = DOKAN_MDL_ALLOCATED;
  }

  DokanFCBLockRO(fcb);

  // size of EVENT_CONTEXT is sum of its length and file name length
  eventLength = sizeof(EVENT_CONTEXT) + fcb->FileName.Length;

  initial = (BOOLEAN)(ccb->SearchPattern == NULL &&
                      !(DokanCCBFlagsIsSet(ccb, DOKAN_DIR_MATCH_ALL)));

  // this is an initial query
  if (initial) {
    DDbgPrint("    initial query\n");
    // and search pattern is provided
    if (irpSp->Parameters.QueryDirectory.FileName) {
      // free current search pattern stored in CCB
      if (ccb->SearchPattern)
        ExFreePool(ccb->SearchPattern);

      // the size of search pattern
      ccb->SearchPatternLength =
          irpSp->Parameters.QueryDirectory.FileName->Length;
      ccb->SearchPattern =
          ExAllocatePool(ccb->SearchPatternLength + sizeof(WCHAR));

      if (ccb->SearchPattern == NULL) {
        DokanFCBUnlock(fcb);
        return STATUS_INSUFFICIENT_RESOURCES;
      }

      RtlZeroMemory(ccb->SearchPattern,
                    ccb->SearchPatternLength + sizeof(WCHAR));

      // copy provided search pattern to CCB
      RtlCopyMemory(ccb->SearchPattern,
                    irpSp->Parameters.QueryDirectory.FileName->Buffer,
                    ccb->SearchPatternLength);

    } else {
      DokanCCBFlagsSetBit(ccb, DOKAN_DIR_MATCH_ALL);
    }
  }

  // if search pattern is provided, add the length of it to store pattern
  if (ccb->SearchPattern) {
    eventLength += ccb->SearchPatternLength;
  }

  eventContext = AllocateEventContext(vcb->Dcb, Irp, eventLength, ccb);

  if (eventContext == NULL) {
    DokanFCBUnlock(fcb);
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  eventContext->Context = ccb->UserContext;
  // DDbgPrint("   get Context %X\n", (ULONG)ccb->UserContext);

  // index which specified index-1 th directory entry has been returned
  // this time, 'index'th entry should be returned
  index = 0;

  if (irpSp->Flags & SL_INDEX_SPECIFIED) {
    index = irpSp->Parameters.QueryDirectory.FileIndex;
    DDbgPrint("    using FileIndex %d\n", index);

  } else if (FlagOn(irpSp->Flags, SL_RESTART_SCAN)) {
    DDbgPrint("    SL_RESTART_SCAN\n");
    index = 0;

  } else {
    index = (ULONG)ccb->Context;
    DDbgPrint("    ccb->Context %d\n", index);
  }

  eventContext->Operation.Directory.FileInformationClass =
      irpSp->Parameters.QueryDirectory.FileInformationClass;
  eventContext->Operation.Directory.BufferLength =
      irpSp->Parameters.QueryDirectory.Length; // length of buffer
  eventContext->Operation.Directory.FileIndex =
      index; // directory index which should be returned this time

  // copying file name(directory name)
  eventContext->Operation.Directory.DirectoryNameLength = fcb->FileName.Length;
  RtlCopyMemory(eventContext->Operation.Directory.DirectoryName,
                fcb->FileName.Buffer, fcb->FileName.Length);
  DokanFCBUnlock(fcb);

  // if search pattern is specified, copy it to EventContext
  if (ccb->SearchPatternLength && ccb->SearchPattern) {
    PVOID searchBuffer;

    eventContext->Operation.Directory.SearchPatternLength =
        ccb->SearchPatternLength;
    eventContext->Operation.Directory.SearchPatternOffset =
        eventContext->Operation.Directory.DirectoryNameLength;

    searchBuffer = (PVOID)(
        (SIZE_T)&eventContext->Operation.Directory.SearchPatternBase[0] +
        (SIZE_T)eventContext->Operation.Directory.SearchPatternOffset);

    RtlCopyMemory(searchBuffer, ccb->SearchPattern, ccb->SearchPatternLength);

    DDbgPrint("    ccb->SearchPattern %ws\n", ccb->SearchPattern);
  }

  status = DokanRegisterPendingIrp(DeviceObject, Irp, eventContext, flags);

  return status;
}

NTSTATUS
DokanNotifyChangeDirectory(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp) {
  PDokanCCB ccb;
  PDokanFCB fcb;
  PFILE_OBJECT fileObject;
  PIO_STACK_LOCATION irpSp;
  PDokanVCB vcb;

  DDbgPrint("\tNotifyChangeDirectory\n");

  irpSp = IoGetCurrentIrpStackLocation(Irp);
  fileObject = irpSp->FileObject;

  vcb = DeviceObject->DeviceExtension;
  if (GetIdentifierType(vcb) != VCB) {
    return STATUS_INVALID_PARAMETER;
  }

  ccb = fileObject->FsContext2;
  ASSERT(ccb != NULL);

  fcb = ccb->Fcb;
  ASSERT(fcb != NULL);

  if (!DokanFCBFlagsIsSet(fcb, DOKAN_FILE_DIRECTORY)) {
    return STATUS_INVALID_PARAMETER;
  }

  DDbgPrint("\tFsRtlNotifyFullChangeDirectory FileName %wZ\n", &fcb->FileName);

  DokanFCBLockRO(fcb);
  FsRtlNotifyFullChangeDirectory(
      vcb->NotifySync, &vcb->DirNotifyList, ccb, (PSTRING)&fcb->FileName,
      (irpSp->Flags & SL_WATCH_TREE) ? TRUE : FALSE, FALSE,
      irpSp->Parameters.NotifyDirectory.CompletionFilter, Irp, NULL, NULL);
  DokanFCBUnlock(fcb);

  return STATUS_PENDING;
}

NTSTATUS DokanCompleteDirectoryControl(__in PIRP_ENTRY IrpEntry,
                                       __in PEVENT_INFORMATION EventInfo,
                                       __in BOOLEAN Wait) {
  PIRP irp;
  PIO_STACK_LOCATION irpSp;
  NTSTATUS status = STATUS_SUCCESS;
  ULONG info = 0;
  ULONG bufferLen = 0;
  PVOID buffer = NULL;

  UNREFERENCED_PARAMETER(Wait);

  DDbgPrint("==> DokanCompleteDirectoryControl\n");

  irp = IrpEntry->Irp;
  irpSp = IrpEntry->IrpSp;

  // buffer pointer which points DirecotryInfo
  if (irp->MdlAddress) {
    // DDbgPrint("   use MDL Address\n");
    buffer = MmGetSystemAddressForMdlNormalSafe(irp->MdlAddress);
  } else {
    // DDbgPrint("   use UserBuffer\n");
    buffer = irp->UserBuffer;
  }
  // usable buffer size
  bufferLen = irpSp->Parameters.QueryDirectory.Length;

  // DDbgPrint("  !!Returning DirecotyInfo!!\n");

  // buffer is not specified or short of length
  if (bufferLen == 0 || buffer == NULL || bufferLen < EventInfo->BufferLength) {
    info = 0;
    status = STATUS_INSUFFICIENT_RESOURCES;

  } else {

    PDokanCCB ccb = IrpEntry->FileObject->FsContext2;
    // ULONG     orgLen = irpSp->Parameters.QueryDirectory.Length;

    //
    // set the information received from user mode
    //
    ASSERT(buffer != NULL);

    RtlZeroMemory(buffer, bufferLen);

    // DDbgPrint("   copy DirectoryInfo\n");
    RtlCopyMemory(buffer, EventInfo->Buffer, EventInfo->BufferLength);

    DDbgPrint("    eventInfo->Directory.Index = %lu\n",
              EventInfo->Operation.Directory.Index);
    DDbgPrint("    eventInfo->BufferLength    = %lu\n",
              EventInfo->BufferLength);
    DDbgPrint("    eventInfo->Status = %x (%lu)\n", EventInfo->Status,
              EventInfo->Status);

    // update index which specified n-th directory entry is returned
    // this should be locked before writing?
    ccb->Context = EventInfo->Operation.Directory.Index;

    ccb->UserContext = EventInfo->Context;
    // DDbgPrint("   set Context %X\n", (ULONG)ccb->UserContext);

    // written bytes
    // irpSp->Parameters.QueryDirectory.Length = EventInfo->BufferLength;

    status = EventInfo->Status;

    info = EventInfo->BufferLength;
  }

  if (IrpEntry->Flags & DOKAN_MDL_ALLOCATED) {
    DokanFreeMdl(irp);
    IrpEntry->Flags &= ~DOKAN_MDL_ALLOCATED;
  }

  DokanCompleteIrpRequest(irp, status, info);

  DDbgPrint("<== DokanCompleteDirectoryControl\n");

  return STATUS_SUCCESS;
}
