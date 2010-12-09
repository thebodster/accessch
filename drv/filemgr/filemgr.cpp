#include "../inc/commonkrnl.h"
#include "../inc/memmgr.h"
#include "../inc/osspec.h"
#include "filestructs.h"

#include "volhlp.h"
#include "volumeflt.h"

#include "filehlp.h"
#include "fileflt.h"

#include "../inc/filemgr.h"

PFLT_FILTER         gFileFilter = NULL;
_tpOnOnload         gUnloadCb = NULL;
FilteringSystem*    gFltSystem = NULL;

NTSTATUS
FLTAPI
Unload (
    __in FLT_FILTER_UNLOAD_FLAGS Flags
    );

void
FLTAPI
ContextCleanup (
    __in PVOID Pool,
    __in FLT_CONTEXT_TYPE ContextType
    );

NTSTATUS
FLTAPI
InstanceSetup (
    __in PCFLT_RELATED_OBJECTS FltObjects,
    __in FLT_INSTANCE_SETUP_FLAGS Flags,
    __in DEVICE_TYPE VolumeDeviceType,
    __in FLT_FILESYSTEM_TYPE VolumeFilesystemType
    );

FLT_PREOP_CALLBACK_STATUS
FLTAPI
PreCreate (
    __inout PFLT_CALLBACK_DATA Data,
    __in PCFLT_RELATED_OBJECTS FltObjects,
    __deref_out_opt PVOID *CompletionContext
    );

FLT_POSTOP_CALLBACK_STATUS
FLTAPI
PostCreate (
    __inout PFLT_CALLBACK_DATA Data,
    __in PCFLT_RELATED_OBJECTS FltObjects,
    __in PVOID CompletionContext,
    __in FLT_POST_OPERATION_FLAGS Flags
    );

FLT_PREOP_CALLBACK_STATUS
FLTAPI
PreCleanup (
    __inout PFLT_CALLBACK_DATA Data,
    __in PCFLT_RELATED_OBJECTS FltObjects,
    __out PVOID *CompletionContext
    );

FLT_POSTOP_CALLBACK_STATUS
FLTAPI
PostWrite (
    __inout PFLT_CALLBACK_DATA Data,
    __in PCFLT_RELATED_OBJECTS FltObjects,
    __in PVOID CompletionContext,
    __in FLT_POST_OPERATION_FLAGS Flags
    );

const FLT_CONTEXT_REGISTRATION ContextRegistration[] = {
    { FLT_INSTANCE_CONTEXT, 0, ContextCleanup, 
        sizeof( InstanceContext ), 'siSA', NULL, NULL, NULL },
    
    { FLT_STREAM_CONTEXT, 0, ContextCleanup,
        sizeof( StreamContext ), 'csSA', NULL, NULL, NULL },
    
    { FLT_STREAMHANDLE_CONTEXT,  0, ContextCleanup,
        sizeof( StreamHandleContext ), 'chSA', NULL, NULL, NULL },
    
    { FLT_VOLUME_CONTEXT, 0, ContextCleanup, sizeof( VolumeContext ),
        'cvSA', NULL, NULL, NULL} ,
    
    { FLT_CONTEXT_END }
};

#define _NO_PAGING  FLTFL_OPERATION_REGISTRATION_SKIP_PAGING_IO

CONST FLT_OPERATION_REGISTRATION Callbacks[] = {
    { IRP_MJ_CREATE,            0,          PreCreate,      PostCreate },
    { IRP_MJ_CLEANUP,           0,          PreCleanup,     NULL },
    { IRP_MJ_WRITE,             _NO_PAGING, NULL,           PostWrite },
    { IRP_MJ_OPERATION_END}
};

FLT_REGISTRATION filterRegistration = {
    sizeof( FLT_REGISTRATION ),                      // Size
    FLT_REGISTRATION_VERSION,                        // Version
    FLTFL_REGISTRATION_DO_NOT_SUPPORT_SERVICE_STOP,  // Flags
    ContextRegistration,                             // Context
    Callbacks,                                       // Operation callbacks
    Unload,                                            //
    InstanceSetup,                                   // InstanceSetup
    NULL,                                            // InstanceQueryTeardown
    NULL,                                            // InstanceTeardownStart
    NULL,                                            // InstanceTeardownComplete
    NULL, NULL,                                      // NameProvider callbacks
    NULL,
#if FLT_MGR_LONGHORN
    NULL,                                            // transaction callback
    NULL                                             //
#endif //FLT_MGR_LONGHORN
};

__checkReturn
NTSTATUS
FLTAPI
Unload (
    __in FLT_FILTER_UNLOAD_FLAGS Flags
    )
{
    if ( !FlagOn(Flags, FLTFL_FILTER_UNLOAD_MANDATORY) )
    {
        /// \todo checks during Unload
        //return STATUS_FLT_DO_NOT_DETACH;
    }

    gUnloadCb();

    FltUnregisterFilter( gFileFilter );
    gFileFilter = NULL;

    return STATUS_SUCCESS;
}

void
FLTAPI
ContextCleanup (
    __in PVOID Pool,
    __in FLT_CONTEXT_TYPE ContextType
    )
{
    switch ( ContextType )
    {
    case FLT_INSTANCE_CONTEXT:
        {
        }
        break;

    case FLT_STREAM_CONTEXT:
        {
            PStreamContext pStreamContext = (PStreamContext) Pool;
            ReleaseContext( (PFLT_CONTEXT*) &pStreamContext->m_InstanceCtx );
            ASSERT ( pStreamContext );
        }
        break;

    case FLT_STREAMHANDLE_CONTEXT:
        {
            PStreamHandleContext pStreamHandleContext = (PStreamHandleContext) Pool;
            ReleaseContext( (PFLT_CONTEXT*) &pStreamHandleContext->m_StreamCtx );
        }
        break;

    case FLT_VOLUME_CONTEXT:
        {
            PVolumeContext pVolumeContext = (PVolumeContext) Pool;
            FREE_POOL( pVolumeContext->m_DeviceId.Buffer );
        }
        break;

    default:
        {
            ASSERT( "cleanup for unknown context!" );
        }
        break;
    }
}


// ----------------------------------------------------------------------------
// file

__checkReturn
FORCEINLINE
BOOLEAN
IsPassThrough (
    __in PCFLT_RELATED_OBJECTS FltObjects,
    __in FLT_POST_OPERATION_FLAGS Flags
    )
{
    if ( FlagOn( Flags, FLTFL_POST_OPERATION_DRAINING ) )
    {
        return TRUE;
    }

    if ( !FltObjects->Instance )
    {
        return TRUE;
    }

    if ( !FltObjects->FileObject )
    {
        return TRUE;
    }

    if ( FlagOn( FltObjects->FileObject->Flags, FO_NAMED_PIPE ) )
    {
        return TRUE;
    }

    PIRP pTopLevelIrp = IoGetTopLevelIrp();
    if ( pTopLevelIrp )
    {
        return TRUE;
    }

    return FALSE;
}

__checkReturn
NTSTATUS
FLTAPI
InstanceSetup (
    __in PCFLT_RELATED_OBJECTS FltObjects,
    __in FLT_INSTANCE_SETUP_FLAGS Flags,
    __in DEVICE_TYPE VolumeDeviceType,
    __in FLT_FILESYSTEM_TYPE VolumeFilesystemType
    )
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    PInstanceContext pInstanceCtx = NULL;
    PVolumeContext pVolumeContext = NULL;

    UNREFERENCED_PARAMETER( Flags );

    ASSERT( FltObjects->Filter == gFileFilter );

    if (FLT_FSTYPE_RAW == VolumeFilesystemType)
    {
        return STATUS_FLT_DO_NOT_ATTACH;
    }

    if ( FILE_DEVICE_NETWORK_FILE_SYSTEM == VolumeDeviceType )
    {
        return STATUS_FLT_DO_NOT_ATTACH;
    }

    __try
    {
        status = FltAllocateContext (
            gFileFilter,
            FLT_INSTANCE_CONTEXT,
            sizeof( InstanceContext ),
            NonPagedPool,
            (PFLT_CONTEXT*) &pInstanceCtx
            );

        if ( !NT_SUCCESS( status ) )
        {
            pInstanceCtx = NULL;
            __leave;
        }

        RtlZeroMemory( pInstanceCtx, sizeof( InstanceContext ) );

        status = FltAllocateContext (
            gFileFilter,
            FLT_VOLUME_CONTEXT,
            sizeof( VolumeContext ),
            NonPagedPool,
            (PFLT_CONTEXT*) &pVolumeContext
            );

        if ( !NT_SUCCESS( status ) )
        {
            pVolumeContext = NULL;
            __leave;
        }
        
        RtlZeroMemory( pVolumeContext, sizeof( VolumeContext ) );

        // just for fun
        pInstanceCtx->m_VolumeDeviceType = VolumeDeviceType;
        pInstanceCtx->m_VolumeFilesystemType = VolumeFilesystemType;

        status = FillVolumeProperties( FltObjects, pVolumeContext );
        if ( !NT_SUCCESS( status ) )
        {
            __leave;
        }

        ASSERT( VolumeDeviceType != FILE_DEVICE_NETWORK_FILE_SYSTEM );

        VERDICT Verdict = VERDICT_NOT_FILTERED;
        if ( gFltSystem->IsFiltersExist() )
        {
            VolumeInterceptorContext event (
                FltObjects,
                pInstanceCtx,
                pVolumeContext,
                VOLUME_MINIFILTER,
                OP_VOLUME_ATTACH,
                0,
                PostProcessing
                );

            PARAMS_MASK params2user;
            status = gFltSystem->FilterEvent (
                &event,
                &Verdict,
                &params2user
                );

            if ( NT_SUCCESS( status ) && FlagOn( Verdict, VERDICT_ASK ) )
            {
                status = PortAskUser( &event, params2user, &Verdict );
                if ( NT_SUCCESS( status ) )
                {
                }
            }
        }

        status = FltSetInstanceContext (
            FltObjects->Instance,
            FLT_SET_CONTEXT_KEEP_IF_EXISTS,
            pInstanceCtx,
            NULL
            );

        pVolumeContext->m_Instance = FltObjects->Instance;
        status = FltSetVolumeContext (
            FltObjects->Volume,
            FLT_SET_CONTEXT_KEEP_IF_EXISTS,
            pVolumeContext,
            NULL
            );
    }
    __finally
    {
        ReleaseContext( (PFLT_CONTEXT*) &pInstanceCtx );
        ReleaseContext( (PFLT_CONTEXT*) &pVolumeContext );
    }

    return STATUS_SUCCESS;
}

FLT_PREOP_CALLBACK_STATUS
FLTAPI
PreCreate (
    __inout PFLT_CALLBACK_DATA Data,
    __in PCFLT_RELATED_OBJECTS FltObjects,
    __deref_out_opt PVOID *CompletionContext
    )
{
    FLT_PREOP_CALLBACK_STATUS fltStatus = FLT_PREOP_SUCCESS_NO_CALLBACK;

    // check access by user filename
    __try
    {
        if (FlagOn( Data->Iopb->OperationFlags, SL_OPEN_PAGING_FILE ) )
        {
            __leave;
        }

        if ( IsPassThrough( FltObjects, 0 ) )
        {
            __leave;
        }

        /// \todo skip checks to volume

        *CompletionContext = NULL;
        fltStatus = FLT_PREOP_SUCCESS_WITH_CALLBACK;

        if ( !gFltSystem->IsFiltersExist() )
        {
            __leave;
        }

        VERDICT Verdict = VERDICT_NOT_FILTERED;

        FileInterceptorContext event (
            Data,
            FltObjects,
            NULL,
            FILE_MINIFILTER,
            OP_FILE_CREATE,
            0,
            PreProcessing
            );

        PARAMS_MASK params2user;
        NTSTATUS status = gFltSystem->FilterEvent (
            &event,
            &Verdict,
            &params2user
            );

        if ( NT_SUCCESS( status ) )
        {
            if ( FlagOn( Verdict, VERDICT_ASK ) )
            {
                status = PortAskUser( &event, params2user, &Verdict );
                if ( NT_SUCCESS( status ) )
                {
                    // nothing todo
                }
            }

            if ( FlagOn( Verdict, VERDICT_DENY ) )
            {
                Data->IoStatus.Status = STATUS_ACCESS_DENIED;
                Data->IoStatus.Information = 0;

                fltStatus = FLT_PREOP_COMPLETE;
            }
        }
    }
    __finally
    {
    }

    return fltStatus;
}

__checkReturn
BOOLEAN
IsSkipPostCreate (
     __in PFLT_CALLBACK_DATA Data,
     __in PCFLT_RELATED_OBJECTS FltObjects,
     __in FLT_POST_OPERATION_FLAGS Flags
    )
{
    if ( STATUS_REPARSE == Data->IoStatus.Status )
    {
        // skip reparse op
        return TRUE;
    }

    if ( !NT_SUCCESS( Data->IoStatus.Status ) )
    {
        // skip failed op
        return TRUE;
    }
    
    if ( IsPassThrough( FltObjects, Flags ) )
    {
        // wrong state
        return TRUE;
    }

    if ( FlagOn( FltObjects->FileObject->Flags,  FO_VOLUME_OPEN ) )
    {
        // volume open
        return TRUE;
    }

    return FALSE;
}

__checkReturn
FLT_POSTOP_CALLBACK_STATUS
FLTAPI
PostCreate (
    __inout PFLT_CALLBACK_DATA Data,
    __in PCFLT_RELATED_OBJECTS FltObjects,
    __in PVOID CompletionContext,
    __in FLT_POST_OPERATION_FLAGS Flags
    )
{
    UNREFERENCED_PARAMETER( CompletionContext );

    FLT_POSTOP_CALLBACK_STATUS fltStatus = FLT_POSTOP_FINISHED_PROCESSING;

    /// \todo access to volume - generate access event

    PStreamHandleContext pStreamHandleContext = NULL;

    __try
    {
        NTSTATUS status;

        if ( IsSkipPostCreate( Data, FltObjects, Flags ) )
        {
            __leave;
        }

        status = GenerateStreamHandleContext (
            gFileFilter,
            FltObjects,
            &pStreamHandleContext
            );

        if ( !NT_SUCCESS( status ) )
        {
            pStreamHandleContext = NULL;

            __leave;
        }

        if ( IsPrefetchEcpPresent( gFileFilter, Data ) )
        {
            SetFlag (
                pStreamHandleContext->m_Flags,
                _STREAM_H_FLAGS_ECPPREF
                );

            __leave;
        }

        if ( !gFltSystem->IsFiltersExist() )
        {
            __leave;
        }

        VERDICT Verdict = VERDICT_NOT_FILTERED;
        FileInterceptorContext event (
            Data,
            FltObjects,
            pStreamHandleContext->m_StreamCtx,
            FILE_MINIFILTER,
            OP_FILE_CREATE,
            0,
            PostProcessing
            );

        PARAMS_MASK params2user;
        status = gFltSystem->FilterEvent (
            &event,
            &Verdict,
            &params2user
            );

        if ( NT_SUCCESS( status ) )
        {
            if ( FlagOn( Verdict, VERDICT_ASK ) )
            {
               status = PortAskUser( &event, params2user, &Verdict );
                if ( NT_SUCCESS( status ) )
                {
                    // nothing todo
                }
            }

            if ( FlagOn( Verdict, VERDICT_DENY ) )
            {
                Data->IoStatus.Status = STATUS_ACCESS_DENIED;
                Data->IoStatus.Information = 0;

                if ( FlagOn( FltObjects->FileObject->Flags, FO_HANDLE_CREATED ) )
                {
                    // file already has handle(s)!
                    // skip blocking
                }
                else
                {
                    FltCancelFileOpen (
                        FltObjects->Instance,
                        FltObjects->FileObject
                        );
                }
            }
            else
            {
                if ( FlagOn( Verdict, VERDICT_CACHE1 ) )
                {
                    event.SetCache1();
                }
            }
        }
    }
    __finally
    {
        ReleaseContext( (PFLT_CONTEXT*) &pStreamHandleContext );
    }

    return fltStatus;
}

__checkReturn
FLT_PREOP_CALLBACK_STATUS
FLTAPI
PreCleanup (
    __inout PFLT_CALLBACK_DATA Data,
    __in PCFLT_RELATED_OBJECTS FltObjects,
    __out PVOID *CompletionContext
    )
{
    NTSTATUS status;
    UNREFERENCED_PARAMETER( CompletionContext );

    FLT_PREOP_CALLBACK_STATUS fltStatus = FLT_PREOP_SUCCESS_NO_CALLBACK;

    PStreamHandleContext pStreamHandleContext = NULL;
    
    __try
    {
        status = GetStreamHandleContext (
            FltObjects,
            &pStreamHandleContext
            );

        if ( !NT_SUCCESS( status ) )
        {
            pStreamHandleContext = NULL;

            __leave;
        }

        if ( FlagOn( pStreamHandleContext->m_Flags, _STREAM_H_FLAGS_ECPPREF ) )
        {
            __leave;
        }

        if ( !gFltSystem->IsFiltersExist() )
        {
            __leave;
        }

        VERDICT Verdict = VERDICT_NOT_FILTERED;
        FileInterceptorContext event (
            Data,
            FltObjects,
            pStreamHandleContext->m_StreamCtx,
            FILE_MINIFILTER,
            OP_FILE_CLEANUP,
            0,
            PreProcessing
            );

        PARAMS_MASK params2user;
        status = gFltSystem->FilterEvent (
            &event,
            &Verdict,
            &params2user
            );

        if ( NT_SUCCESS( status ) && FlagOn( Verdict, VERDICT_ASK ) )
        {
            status = PortAskUser( &event, params2user, &Verdict );
            if ( NT_SUCCESS( status ) )
            {
                if (
                    !FlagOn( Verdict, VERDICT_DENY)
                    &&
                    FlagOn( Verdict, VERDICT_CACHE1 )
                    )
                {
                    event.SetCache1();
                }
            }
        }
    }
    __finally
    {
        ReleaseContext( (PFLT_CONTEXT*) &pStreamHandleContext );
    }

    return fltStatus;
}

__checkReturn
BOOLEAN
IsSkipPostWrite (
    __in PFLT_CALLBACK_DATA Data,
    __in PCFLT_RELATED_OBJECTS FltObjects,
    __in FLT_POST_OPERATION_FLAGS Flags
    )
{
    if ( FlagOn( Flags, FLTFL_POST_OPERATION_DRAINING ) )
    {
        return TRUE;
    }

    if ( !FltObjects->Instance )
    {
        return TRUE;
    }

    if ( !FltObjects->FileObject )
    {
        return TRUE;
    }

    if ( FlagOn( FltObjects->FileObject->Flags, FO_NAMED_PIPE ) )
    {
        return TRUE;
    }

    if ( !Data->IoStatus.Information )
    {
        return TRUE;
    }

    return FALSE;
}

__checkReturn
FLT_POSTOP_CALLBACK_STATUS
FLTAPI
PostWrite (
    __inout PFLT_CALLBACK_DATA Data,
    __in PCFLT_RELATED_OBJECTS FltObjects,
    __in PVOID CompletionContext,
    __in FLT_POST_OPERATION_FLAGS Flags
    )
{
    UNREFERENCED_PARAMETER( CompletionContext );

    if ( !NT_SUCCESS( Data->IoStatus.Status ) )
    {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }
    
    if ( IsSkipPostWrite( Data, FltObjects, Flags ) )
    {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    if ( FlagOn( Data->Iopb->IrpFlags, IRP_PAGING_IO ) )
    {
        //! \todo ��������� MM ������
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    PStreamContext pStreamContext = NULL;

    __try
    {
        NTSTATUS status = GenerateStreamContext (
            gFileFilter,
            FltObjects,
            &pStreamContext
            );

        if ( NT_SUCCESS( status ) )
        {
            InterlockedIncrement( &pStreamContext->m_WriteCount );
            InterlockedAnd( &pStreamContext->m_Flags, ~_STREAM_FLAGS_CASHE1 );
            InterlockedOr( &pStreamContext->m_Flags, _STREAM_FLAGS_MODIFIED );
        }
    }
    __finally
    {
        ReleaseContext( (PFLT_CONTEXT*) &pStreamContext );
    }

    return FLT_POSTOP_FINISHED_PROCESSING;
}

//////////////////////////////////////////////////////////////////////////
__checkReturn
NTSTATUS
FileMgrInit (
    __in PDRIVER_OBJECT DriverObject,
    __in _tpOnOnload UnloadCb,
    __in FilteringSystem* FltSystem
    )
{
    gUnloadCb = UnloadCb;

    NTSTATUS status = FltRegisterFilter (
        DriverObject,
        (PFLT_REGISTRATION) &filterRegistration,
        &gFileFilter
        );

    return status;
}

__checkReturn
NTSTATUS
FileMgrStart (
    )
{
   NTSTATUS status = FltStartFiltering( gFileFilter );

   return status;
}
 
PFLT_FILTER
FileMgrGetFltFilter (
    )
{
    return gFileFilter;
}