/*
 * RenderSystem.cpp
 *
 * Copyright (c) 2015 Lukas Hermanns. All rights reserved.
 * Licensed under the terms of the BSD 3-Clause license (see LICENSE.txt).
 */

#include "../Platform/Module.h"
#include "../Core/CoreUtils.h"
#include "../Core/StringUtils.h"
#include "../Core/Assertion.h"
#include "../Core/Exception.h"
#include "../Core/StringUtils.h"
#include "RenderTargetUtils.h"
#include <LLGL/Platform/Platform.h>
#include <LLGL/Utils/ForRange.h>
#include <LLGL/Format.h>
#include <LLGL/ImageFlags.h>
#include <LLGL/Constants.h>
#include <LLGL/Log.h>
#include "BuildID.h"

#include <LLGL/RenderSystem.h>
#include "RenderSystemRegistry.h"
#include <string>
#include <unordered_map>

#include "../Core/PrintfUtils.h"

#ifdef LLGL_ENABLE_DEBUG_LAYER
#   include "DebugLayer/DbgRenderSystem.h"
#endif

#include <LLGL/Platform/Platform.h>
#ifdef LLGL_OS_ANDROID
#   include "../Platform/Android/AndroidApp.h"
#endif

#include "ModuleInterface.h"


namespace LLGL
{


/* ----- Render system ----- */

struct RenderSystem::Pimpl
{
    int                     rendererID  = 0;
    std::string             name;
    bool                    hasInfo     = false;
    RendererInfo            info;
    bool                    hasCaps     = false;
    RenderingCapabilities   caps;
    Report                  report;
};


RenderSystem::RenderSystem() :
    pimpl_ { new Pimpl{} }
{
}

RenderSystem::~RenderSystem()
{
    delete pimpl_;
}

std::vector<std::string> RenderSystem::FindModules()
{
    #if LLGL_BUILD_STATIC_LIB
    return StaticModules::GetStaticModules();
    #else
    return RenderSystemModule::FindModules();
    #endif
}

RenderSystemPtr RenderSystem::Load(const RenderSystemDescriptor& renderSystemDesc, Report* report)
{
    /* Initialize mobile specific states */
    #if defined LLGL_OS_ANDROID

    AndroidApp::Get().Initialize(renderSystemDesc.androidApp);

    #endif

    #if LLGL_BUILD_STATIC_LIB

    /* Allocate render system */
    RenderSystemPtr renderSystem{ StaticModules::AllocRenderSystem(renderSystemDesc) };
    if (renderSystem == nullptr)
        return ReportException(report, "failed to allocate render system from module: %s", renderSystemDesc.moduleName.c_str());

    if (renderSystemDesc.debugger != nullptr)
    {
        #if LLGL_ENABLE_DEBUG_LAYER

        /* Create debug layer render system */
        renderSystem = RenderSystemPtr{ new DbgRenderSystem{ std::move(renderSystem), renderSystemDesc.debugger } };

        #else

        if (report != nullptr)
            report->Errorf("LLGL was not compiled with debug layer support");

        #endif // /LLGL_ENABLE_DEBUG_LAYER
    }

    renderSystem->pimpl_->name          = StaticModules::GetRendererName(renderSystemDesc.moduleName);
    renderSystem->pimpl_->rendererID    = StaticModules::GetRendererID(renderSystemDesc.moduleName);

    /* Return new render system and unique pointer */
    return renderSystem;

    #else // LLGL_BUILD_STATIC_LIB

    /* Load render system module */
    RenderSystemModule* module = RenderSystemRegistry::Get().LoadModule(renderSystemDesc.moduleName.c_str(), report);
    if (module == nullptr)
        return nullptr;

    /*
    Verify build ID from render system module to detect a module,
    that has compiled with a different compiler (type, version, debug/release mode etc.)
    */
    if (module->BuildID() != LLGL_BUILD_ID)
        return ReportException(report, "build ID mismatch in render system module");

    #if LLGL_EXCEPTIONS_SUPPORTED
    try
    #endif
    {
        /* Allocate render system */
        RenderSystemPtr renderSystem = module->AllocRenderSystem(renderSystemDesc, report);

        if (renderSystem)
        {
            if (renderSystemDesc.debugger != nullptr)
            {
                #if LLGL_ENABLE_DEBUG_LAYER

                /* Create debug layer render system */
                if ((renderSystemDesc.flags & RenderSystemFlags::DebugBreakOnError) != 0)
                    renderSystemDesc.debugger->SetBreakOnError(true);

                renderSystem = RenderSystemPtr{ new DbgRenderSystem{ std::move(renderSystem), renderSystemDesc.debugger } };

                #else

                if (report != nullptr)
                    report->Errorf("LLGL was not compiled with debug layer support");

                #endif // /LLGL_ENABLE_DEBUG_LAYER
            }

            renderSystem->pimpl_->name          = module->RendererName();
            renderSystem->pimpl_->rendererID    = module->RendererID();

            /* Link render system to module */
            RenderSystemRegistry::Get().RegisterRenderSystem(renderSystem.get(), module);
        }

        return renderSystem;
    }
    #if LLGL_EXCEPTIONS_SUPPORTED
    catch (const std::exception& e)
    {
        /* Throw with new exception, otherwise the exception's v-table will be corrupted since it's part of the module */
        return ReportException(report, e.what());
    }
    #endif // /LLGL_EXCEPTIONS_SUPPORTED

    #endif // /LLGL_BUILD_STATIC_LIB
}

void RenderSystem::Unload(RenderSystemPtr&& renderSystem)
{
    if (RenderSystem* renderSystemRef = renderSystem.get())
    {
        /* Delete render system first, then release module */
        renderSystem.reset();
        RenderSystemRegistry::Get().UnregisterRenderSystem(renderSystemRef);
    }
}

int RenderSystem::GetRendererID() const
{
    return pimpl_->rendererID;
}

const char* RenderSystem::GetName() const
{
    return pimpl_->name.c_str();
}

const RendererInfo& RenderSystem::GetRendererInfo()
{
    if (!pimpl_->hasInfo)
    {
        if (QueryRendererDetails(&(pimpl_->info), nullptr))
            pimpl_->hasInfo = true;
    }
    return pimpl_->info;
}

const RenderingCapabilities& RenderSystem::GetRenderingCaps()
{
    if (!pimpl_->hasCaps)
    {
        if (QueryRendererDetails(nullptr, &(pimpl_->caps)))
            pimpl_->hasCaps = true;
    }
    return pimpl_->caps;
}

const Report* RenderSystem::GetReport() const
{
    return (pimpl_->report ? &(pimpl_->report) : nullptr);
}


/*
 * ======= Protected: =======
 */

Report& RenderSystem::GetMutableReport()
{
    return pimpl_->report;
}

void RenderSystem::Errorf(const char* format, ...)
{
    std::string report;
    LLGL_STRING_PRINTF(report, format);
    GetMutableReport().Reset(std::move(report), true);
}

void RenderSystem::AssertCreateBuffer(const BufferDescriptor& bufferDesc, std::uint64_t maxSize)
{
    LLGL_ASSERT(
        (bufferDesc.size <= maxSize),
        "buffer descriptor with size of 0x%016" PRIX64 " exceeded limit of 0x%016" PRIX64, bufferDesc.size, maxSize
    );

    /* Validate binding flags */
    const long validBindFlags =
    (
        BindFlags::VertexBuffer         |
        BindFlags::IndexBuffer          |
        BindFlags::ConstantBuffer       |
        BindFlags::Sampled              |
        BindFlags::Storage              |
        BindFlags::StreamOutputBuffer   |
        BindFlags::IndirectBuffer       |
        BindFlags::CopySrc              |
        BindFlags::CopyDst
    );

    LLGL_ASSERT(
        ((bufferDesc.bindFlags & (~validBindFlags)) == 0),
        "buffer descriptor with invalid binding flags 0x%08X",
        static_cast<unsigned>(bufferDesc.bindFlags)
    );
}

static void AssertCreateResourceArrayCommon(std::uint32_t numResources, void* const * resourceArray, const char* resourceName)
{
    /* Validate number of buffers */
    LLGL_ASSERT(!(numResources == 0), "cannot create %s array with zero elements", resourceName);

    /* Validate array pointer */
    LLGL_ASSERT(!(resourceArray == nullptr), "cannot create %s array with null pointer for array", resourceName);

    /* Validate pointers in array */
    for_range(i, numResources)
        LLGL_ASSERT(!(resourceArray[i] == nullptr), "cannot create %s array with null pointer for array element [%u]", resourceName, i);
}

void RenderSystem::AssertCreateBufferArray(std::uint32_t numBuffers, Buffer* const * bufferArray)
{
    /* Validate common resource array parameters */
    AssertCreateResourceArrayCommon(numBuffers, reinterpret_cast<void* const*>(bufferArray), "buffer");
}

void RenderSystem::AssertCreateShader(const ShaderDescriptor& shaderDesc)
{
    LLGL_ASSERT(
        !(shaderDesc.source == nullptr),
        "cannot create shader with <source> being a null pointer"
    );
    LLGL_ASSERT(
        !(shaderDesc.sourceType == ShaderSourceType::BinaryBuffer && shaderDesc.sourceSize == 0),
        "cannot create shader from binary buffer with <sourceSize> being zero"
    );
}


} // /namespace LLGL



// ================================================================================
