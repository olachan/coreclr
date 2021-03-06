//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.
//
//*****************************************************************************
// File: MethodIter.cpp

// Iterate through jitted instances of a method.
//*****************************************************************************


#include "common.h"
#include "methoditer.h"


//---------------------------------------------------------------------------------------
// 
// Iterates next MethodDesc. Updates the holder only if the assembly differs from the previous one.
// Caller should not release (i.e. change) the holder explicitly between calls, otherwise collectible 
// assembly might be without a reference and get deallocated (even the native part).
// 
BOOL LoadedMethodDescIterator::Next(
    CollectibleAssemblyHolder<DomainAssembly *> * pDomainAssemblyHolder)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        MODE_PREEMPTIVE;
    }
    CONTRACTL_END
    
    if (!m_fFirstTime)
    {
        // This is the 2nd or more time we called Next().
        
        // If the method + type is not generic, then nothing more to iterate.
        if (!m_mainMD->HasClassOrMethodInstantiation())
        {
            *pDomainAssemblyHolder = NULL;
            return FALSE;
        }
        goto ADVANCE_METHOD;
    }

    m_fFirstTime = FALSE;
    
    // This is the 1st time we've called Next(). must Initialize iterator
    if (m_mainMD == NULL)
    {
		m_mainMD = m_module->LookupMethodDef(m_md);
	}
	
    // note m_mainMD should be sufficiently restored to allow us to get
    // at the method table, flags and token etc.
    if (m_mainMD == NULL)
    {
        *pDomainAssemblyHolder = NULL;
        return FALSE;
    }        

    // Needs to work w/ non-generic methods too.
    // NOTE: this behavior seems odd. We appear to return the non-generic method even if
    // that method doesn't reside in the set of assemblies defined by m_assemblyIterationMode.
    // Presumably all the callers expect or at least cope with this so I'm just commenting without
    // changing anything right now.
    if (!m_mainMD->HasClassOrMethodInstantiation())
    {
        *pDomainAssemblyHolder = NULL;
        return TRUE;
    }

    if (m_assemblyIterationMode == kModeSharedDomainAssemblies)
    {
        // Nothing to do...  m_sharedAssemblyIterator is initialized on construction
    }
    else
    {
        m_assemIterator = m_pAppDomain->IterateAssembliesEx(m_assemIterationFlags);
    }

ADVANCE_ASSEMBLY:
    if (m_assemblyIterationMode == kModeSharedDomainAssemblies)
    {
        if  (!m_sharedAssemblyIterator.Next())
            return FALSE;

        m_sharedModuleIterator = m_sharedAssemblyIterator.GetAssembly()->IterateModules();
    }
    else
    {
        if  (!m_assemIterator.Next(pDomainAssemblyHolder))
        {
            _ASSERTE(*pDomainAssemblyHolder == NULL);
            return FALSE;
        }

        if (m_assemblyIterationMode == kModeUnsharedADAssemblies)
        {
            // We're supposed to ignore shared assemblies, so check for them now
            if ((*pDomainAssemblyHolder)->GetAssembly()->IsDomainNeutral())
            {
                goto ADVANCE_ASSEMBLY;
            }
        }

#ifdef _DEBUG
        dbg_m_pDomainAssembly = *pDomainAssemblyHolder;
#endif //_DEBUG

        m_moduleIterator = (*pDomainAssemblyHolder)->IterateModules(m_moduleIterationFlags);
    }
    
    
ADVANCE_MODULE:
    if (m_assemblyIterationMode == kModeSharedDomainAssemblies)
    {
        if  (!NextSharedModule())
            goto ADVANCE_ASSEMBLY;
    }
    else
    {
        if  (!m_moduleIterator.Next())
            goto ADVANCE_ASSEMBLY;
    }

    if (GetCurrentModule()->IsResource())
        goto ADVANCE_MODULE;
    
    if (m_mainMD->HasClassInstantiation())
    {
        m_typeIterator.Reset();
    }
    else
    {
        m_startedNonGenericType = FALSE;
    }
    
ADVANCE_TYPE:
    if (m_mainMD->HasClassInstantiation())
    {
        if (!GetCurrentModule()->GetAvailableParamTypes()->FindNext(&m_typeIterator, &m_typeIteratorEntry))
            goto ADVANCE_MODULE;
        if (CORCOMPILE_IS_POINTER_TAGGED(m_typeIteratorEntry->GetTypeHandle().AsTAddr()))
            goto ADVANCE_TYPE;

        //if (m_typeIteratorEntry->data != TypeHandle(m_mainMD->GetMethodTable()))
        //    goto ADVANCE_TYPE;
        
        // When looking up the AvailableParamTypes table we have to be really careful since
        // the entries may be unrestored, and may have all sorts of encoded tokens in them.
        // Similar logic occurs in the Lookup function for that table.  We will clean this 
        // up in Whidbey Beta2.
        TypeHandle th = m_typeIteratorEntry->GetTypeHandle();
        
        if (th.IsEncodedFixup())
            goto ADVANCE_TYPE;
        
        if (th.IsTypeDesc())
            goto ADVANCE_TYPE;

        MethodTable *pMT = th.AsMethodTable();

        if (!pMT->IsRestored())
            goto ADVANCE_TYPE;

        // Check the class token 
        if (pMT->GetTypeDefRid() != m_mainMD->GetMethodTable()->GetTypeDefRid())
            goto ADVANCE_TYPE;

        // Check the module is correct
        if (pMT->GetModule() != m_module)
            goto ADVANCE_TYPE;
    }
    else if (m_startedNonGenericType)
    {
        goto ADVANCE_MODULE;
    }
    else
    {
        m_startedNonGenericType = TRUE;
    }
    
    if (m_mainMD->HasMethodInstantiation())
    {
        m_methodIterator.Reset();
    }
    else
    {
        m_startedNonGenericMethod = FALSE;
    }
    
ADVANCE_METHOD:
    if (m_mainMD->HasMethodInstantiation())
    {
        if (!GetCurrentModule()->GetInstMethodHashTable()->FindNext(&m_methodIterator, &m_methodIteratorEntry))
            goto ADVANCE_TYPE;
        if (CORCOMPILE_IS_POINTER_TAGGED(dac_cast<TADDR>(m_methodIteratorEntry->GetMethod())))
            goto ADVANCE_METHOD;
        if (!m_methodIteratorEntry->GetMethod()->IsRestored())
            goto ADVANCE_METHOD;
        if (m_methodIteratorEntry->GetMethod()->GetModule() != m_module)
            goto ADVANCE_METHOD;
        if (m_methodIteratorEntry->GetMethod()->GetMemberDef() != m_md)
            goto ADVANCE_METHOD;
    }
    else if (m_startedNonGenericMethod)
    {
        goto ADVANCE_TYPE;
    }
    else
    {
        m_startedNonGenericMethod = TRUE;
    }
    
    // Note: We don't need to keep the assembly alive in DAC - see code:CollectibleAssemblyHolder#CAH_DAC
#ifndef DACCESS_COMPILE
    _ASSERTE_MSG(
        ((m_assemblyIterationMode == kModeSharedDomainAssemblies) ||
        (*pDomainAssemblyHolder == dbg_m_pDomainAssembly)),
        "Caller probably modified the assembly holder, which he shouldn't - see method comment.");
#endif //DACCESS_COMPILE
    
    return TRUE;
} // LoadedMethodDescIterator::Next


Module * LoadedMethodDescIterator::GetCurrentModule()
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        MODE_ANY;
    }
    CONTRACTL_END

    if (m_assemblyIterationMode == kModeSharedDomainAssemblies)
    {
        return m_sharedModuleIterator.GetModule();
    }
    return m_moduleIterator.GetLoadedModule();
}


BOOL LoadedMethodDescIterator::NextSharedModule()
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        MODE_PREEMPTIVE;
    }
    CONTRACTL_END

    _ASSERTE(m_assemblyIterationMode == kModeSharedDomainAssemblies);

    while (m_sharedModuleIterator.Next())
    {
        // NOTE: If this code is to be shared with the dbgapi, the dbgapi
        // will probably want to substitute its own test for "loadedness"
        // here.
#ifdef PROFILING_SUPPORTED
        Module * pModule = m_sharedModuleIterator.GetModule();
        if (!pModule->IsProfilerNotified())
            continue;
#endif // PROFILING_SUPPORTED

        // If we made it this far, pModule is suitable for iterating over
        return TRUE;
    }
    return FALSE;
}

MethodDesc *LoadedMethodDescIterator::Current()
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        PRECONDITION(CheckPointer(m_mainMD));
    }
    CONTRACTL_END
    
    
    if (m_mainMD->HasMethodInstantiation())
    {
        _ASSERTE(m_methodIteratorEntry);
        return m_methodIteratorEntry->GetMethod();
    }
    
    if (!m_mainMD->HasClassInstantiation())
    {   
        // No Method or Class instantiation,then it's not generic.
        return m_mainMD;
    }
    
    MethodTable *pMT = m_typeIteratorEntry->GetTypeHandle().GetMethodTable();
    PREFIX_ASSUME(pMT != NULL);
    _ASSERTE(pMT);
    
    return pMT->GetMethodDescForSlot(m_mainMD->GetSlot());
}

// Initialize the iterator. It will cover generics + prejitted;
// but it is not EnC aware.
void
LoadedMethodDescIterator::Start(
    AppDomain * pAppDomain, 
    Module *pModule,
    mdMethodDef md,
    AssemblyIterationMode assemblyIterationMode,
    AssemblyIterationFlags assemblyIterationFlags,
    ModuleIterationOption moduleIterationFlags)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        PRECONDITION(CheckPointer(pModule));
        PRECONDITION(CheckPointer(pAppDomain, NULL_OK));
    }
    CONTRACTL_END;

    // Specifying different assembly/module iteration flags has only been tested for UnsharedADAssemblies mode so far.
    // It probably doesn't work as you would expect in other modes. In particular the shared assembly iterator
    // doesn't use flags, and the logic in this iterator does a hard-coded filter that roughly matches the unshared
    // mode if you had specified these flags:
    // Assembly: Loading | Loaded | Execution
    // Module: kModIterIncludeAvailableToProfilers
    _ASSERTE((assemblyIterationMode == kModeUnsharedADAssemblies) ||
        (assemblyIterationFlags == (AssemblyIterationFlags)(kIncludeLoaded | kIncludeExecution)));
    _ASSERTE((assemblyIterationMode == kModeUnsharedADAssemblies) ||
        (moduleIterationFlags == kModIterIncludeLoaded));

    m_assemblyIterationMode = assemblyIterationMode;
    m_assemIterationFlags = assemblyIterationFlags;
    m_moduleIterationFlags = moduleIterationFlags;
    m_mainMD = NULL;
    m_module = pModule;
    m_md = md;
    m_pAppDomain = pAppDomain;
    m_fFirstTime = TRUE;

    // If we're not iterating through the SharedDomain, caller must specify the
    // pAppDomain to search.
    _ASSERTE((assemblyIterationMode == kModeSharedDomainAssemblies) || (pAppDomain != NULL));
    _ASSERTE(TypeFromToken(m_md) == mdtMethodDef);
}

// This is special init for DAC only
// @TODO:: change it to dac compile only. 
void
LoadedMethodDescIterator::Start(
    AppDomain     *pAppDomain, 
    Module          *pModule,
    mdMethodDef     md,
    MethodDesc      *pMethodDesc)
{
    Start(pAppDomain, pModule, md, kModeAllADAssemblies);
    m_mainMD = pMethodDesc;
}

LoadedMethodDescIterator::LoadedMethodDescIterator(void)
{
    LIMITED_METHOD_CONTRACT;
    m_mainMD = NULL;
    m_module = NULL;
    m_md = mdTokenNil;
    m_pAppDomain = NULL;
}
