#pragma once
#include "PrimitiveViewRelevance.h"
#include "RenderResource.h"
#include "RenderingThread.h"
#include "PrimitiveSceneProxy.h"
#include "Containers/ResourceArray.h"
#include "EngineGlobals.h"
#include "VertexFactory.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "LocalVertexFactory.h"
#include "Engine/Engine.h"
#include "SceneManagement.h"
#include "DynamicMeshBuilder.h"
#include "StaticMeshResources.h"
#include "MeshMaterialShader.h"
#include "ShaderParameters.h"
#include "RHIUtilities.h"

#include "MeshMaterialShader.h"

/* Helper function that initializes a render resource if it's not initialized, or updates it otherwise*/
static inline void InitOrUpdateResource(FRenderResource* Resource)
{
	if (!Resource->IsInitialized())
	{
		Resource->InitResource();
	}
	else
	{
		Resource->UpdateRHI();
	}
}

/*
 * Helper function that initializes the vertex buffers of the vertex factory's Data member from the static mesh vertex buffers
 * We're using this so we can initialize only the data that we're interested in.
*/
static void InitVertexFactoryData(FLocalVertexFactory* VertexFactory, FStaticMeshVertexBuffers* VertexBuffers)
{
	VertexBuffers->StaticMeshVertexBuffer.SetVertexTangents(0, FVector3f(1, 0, 0), FVector3f(0, 1, 0), FVector3f(0, 0, 1));
	VertexBuffers->StaticMeshVertexBuffer.SetVertexUV(0, 0, FVector2f(0, 0));
	int NumTexCoords = 1;
	int LightMapIndex = 0;

	ENQUEUE_RENDER_COMMAND(StaticMeshVertexBuffersLegacyInit)(
		[VertexFactory, VertexBuffers, LightMapIndex](FRHICommandListImmediate& RHICmdList)
		{
			//Initialize or update the RHI vertex buffers
			InitOrUpdateResource(&VertexBuffers->PositionVertexBuffer);
			InitOrUpdateResource(&VertexBuffers->StaticMeshVertexBuffer);
			InitOrUpdateResource(&VertexBuffers->ColorVertexBuffer);

			//Use the RHI vertex buffers to create the needed Vertex stream components in an FDataType instance, and then set it as the data of the vertex factory
			FLocalVertexFactory::FDataType Data;
			VertexBuffers->PositionVertexBuffer.BindPositionVertexBuffer(VertexFactory, Data);
			VertexBuffers->StaticMeshVertexBuffer.BindTangentVertexBuffer(VertexFactory, Data);
			VertexBuffers->StaticMeshVertexBuffer.BindPackedTexCoordVertexBuffer(VertexFactory, Data);
			VertexBuffers->StaticMeshVertexBuffer.BindLightMapVertexBuffer(VertexFactory, Data, LightMapIndex);
			VertexBuffers->ColorVertexBuffer.BindColorVertexBuffer(VertexFactory, Data);
			VertexFactory->SetData(Data);

			//Initalize the vertex factory using the data that we just set, this will call the InitRHI() method that we implemented in out vertex factory
			InitOrUpdateResource(VertexFactory);
		});
}


///////////////////////////////////////////////////////////////////////
// The Deform Mesh Component Mesh Section Proxy
/*
 * Stores the render thread data that it is needed to render one mesh section
 1 Vertex Data: Each mesh section creates an instance of the vertex factory(vertex streams and declarations), also each mesh section owns an index buffer
 2 Material : Contains a pointer to the material that will be used to render this section
 3 Other Data: Visibility, and the maximum vertex index.
*/
///////////////////////////////////////////////////////////////////////
class FDeformMeshSectionProxy
{
public:
	////////////////////////////////////////////////////////
	/* Material applied to this section */
	UMaterialInterface* Material;
	/* Index buffer for this section */
	FRawStaticIndexBuffer IndexBuffer;
	/* Vertex factory instance for this section */
	FLocalVertexFactory VertexFactory;
	/* Whether this section is currently visible */
	bool bSectionVisible;
	/* Max vertix index is an info that is needed when rendering the mesh, so we cache it here so we don't have to pointer chase it later*/
	uint32 MaxVertexIndex;

	/* For each section, we'll create a vertex factory to store the per-instance mesh data*/
	FDeformMeshSectionProxy(ERHIFeatureLevel::Type InFeatureLevel)
		: Material(NULL)
		, VertexFactory(InFeatureLevel, "FDeformMeshSectionProxy")
		, bSectionVisible(true)
	{}
};

///////////////////////////////////////////////////////////////////////
// The Deform Mesh Component Scene Proxy
/*
 * Encapsulates the render thread data of the Deform Mesh Component
 * Read more about scene proxies: https://docs.unrealengine.com/en-US/API/Runtime/Engine/FPrimitiveSceneProxy/index.html
*/
///////////////////////////////////////////////////////////////////////
class FDeformMeshSceneProxy final : public FPrimitiveSceneProxy
{
public:
	SIZE_T GetTypeHash() const override
	{
		static size_t UniquePointer;
		return reinterpret_cast<size_t>(&UniquePointer);
	}

	/* On construction of the Scene proxy, we'll copy all the needed data from the game thread mesh sections to create the needed render thread mesh sections' proxies*/
	/* We'll also create the structured buffer that will contain the deform transforms of all the sections*/
	FDeformMeshSceneProxy(UDeformMeshComponent* Component)
		: FPrimitiveSceneProxy(Component)
		, MaterialRelevance(Component->GetMaterialRelevance(GetScene().GetFeatureLevel()))
	{
		// Copy each section
		const uint16 NumSections = Component->DeformMeshSections.Num();

		//Initialize the array of trnasforms and the array of mesh sections proxies
		DeformTransforms.AddZeroed(NumSections);
		Sections.AddZeroed(NumSections);

		for (uint16 SectionIdx = 0; SectionIdx < NumSections; SectionIdx++)
		{
			const FDeformMeshSection& SrcSection = Component->DeformMeshSections[SectionIdx];
			{
				//Create a new mesh section proxy
				FDeformMeshSectionProxy* NewSection = new FDeformMeshSectionProxy(GetScene().GetFeatureLevel());

				//Get the needed data from the static mesh of the mesh section
				//We're assuming that there's only one LOD
				auto& LODResource = SrcSection.StaticMesh->GetRenderData()->LODResources[0];

				FLocalVertexFactory* VertexFactory = &NewSection->VertexFactory;
				InitVertexFactoryData(VertexFactory, &(LODResource.VertexBuffers));

				//Copy the indices from the static mesh index buffer and use it to initialize the mesh section proxy's index buffer
				{
					TArray<uint32> tmp_indices;
					LODResource.IndexBuffer.GetCopy(tmp_indices);
					NewSection->IndexBuffer.AppendIndices(tmp_indices.GetData(), tmp_indices.Num());
					//Initialize the render resource
					BeginInitResource(&NewSection->IndexBuffer);
				}

				//Fill the array of transforms with the transform matrix from each section
				DeformTransforms[SectionIdx] = SrcSection.DeformTransform;

				//Set the max vertex index for this mesh section
				NewSection->MaxVertexIndex = LODResource.VertexBuffers.PositionVertexBuffer.GetNumVertices() - 1;

				//Get the material of this section
				NewSection->Material = Component->GetMaterial(SectionIdx);

				if (NewSection->Material == NULL)
				{
					NewSection->Material = UMaterial::GetDefaultMaterial(MD_Surface);
				}

				// Copy visibility info
				NewSection->bSectionVisible = SrcSection.bSectionVisible;

				// Save ref to new section
				Sections[SectionIdx] = NewSection;
			}
		}

		//Create the structured buffer only if we have at least one section
		//if(NumSections > 0)
		//{
		//	///////////////////////////////////////////////////////////////
		//	//// CREATING THE STRUCTURED BUFFER FOR THE DEFORM TRANSFORMS OF ALL THE SECTIONS
		//	//We'll use one structured buffer for all the mesh sections of the component

		//	//We first create a resource array to use it in the create info for initializing the structured buffer on creation
		//	TResourceArray<FMatrix>* ResourceArray = new TResourceArray<FMatrix>(true);
		//	FRHIResourceCreateInfo CreateInfo(TEXT("DeformMesh_TransformsSB"));
		//	ResourceArray->Append(DeformTransforms);
		//	CreateInfo.ResourceArray = ResourceArray;

		//	DeformTransformsSB = RHICreateStructuredBuffer(sizeof(FMatrix), NumSections * sizeof(FMatrix), BUF_ShaderResource, CreateInfo);
		//	bDeformTransformsDirty = false;
		//	///////////////////////////////////////////////////////////////
		//	//// CREATING AN SRV FOR THE STRUCTUED BUFFER SO WA CAN USE IT AS A SHADER RESOURCE PARAMETER AND BIND IT TO THE VERTEX FACTORY
		//	DeformTransformsSRV = RHICreateShaderResourceView(DeformTransformsSB);

		//	///////////////////////////////////////////////////////////////
		//}
	}

	virtual ~FDeformMeshSceneProxy()
	{
		//For each section , release the render resources
		for (FDeformMeshSectionProxy* Section : Sections)
		{
			if (Section != nullptr)
			{
				Section->IndexBuffer.ReleaseResource();
				Section->VertexFactory.ReleaseResource();
				delete Section;
			}
		}

		//Release the structured buffer and the SRV
		DeformTransformsSB.SafeRelease();
		DeformTransformsSRV.SafeRelease();
	}


	/* Update the transforms structured buffer using the array of deform transform, this will update the array on the GPU*/
	void UpdateDeformTransformsSB_RenderThread()
	{
		check(IsInRenderingThread());
		//Update the structured buffer only if it needs update
		if (bDeformTransformsDirty && DeformTransformsSB)
		{
			void* StructuredBufferData = RHILockBuffer(DeformTransformsSB, 0, DeformTransforms.Num() * sizeof(FMatrix), RLM_WriteOnly);
			FMemory::Memcpy(StructuredBufferData, DeformTransforms.GetData(), DeformTransforms.Num() * sizeof(FMatrix));
			RHIUnlockBuffer(DeformTransformsSB);
			bDeformTransformsDirty = false;
		}
	}

	/* Update the deform transform that is being used to deform this mesh section, this will just update this section's entry in the CPU array*/
	void UpdateDeformTransform_RenderThread(int32 SectionIndex, FMatrix Transform)
	{
		check(IsInRenderingThread());
		if (SectionIndex < Sections.Num() &&
			Sections[SectionIndex] != nullptr)
		{
			DeformTransforms[SectionIndex] = Transform;
			//Mark as dirty
			bDeformTransformsDirty = true;
		}
	}

	/* Update the mesh section's visibility*/
	void SetSectionVisibility_RenderThread(int32 SectionIndex, bool bNewVisibility)
	{
		check(IsInRenderingThread());

		if (SectionIndex < Sections.Num() &&
			Sections[SectionIndex] != nullptr)
		{
			Sections[SectionIndex]->bSectionVisible = bNewVisibility;
		}
	}

	/* Given the scene views and the visibility map, we add to the collector the relevant dynamic meshes that need to be rendered by this component*/
	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override
	{
		// Set up wireframe material (if needed)
		const bool bWireframe = AllowDebugViewmodes() && ViewFamily.EngineShowFlags.Wireframe;

		FColoredMaterialRenderProxy* WireframeMaterialInstance = NULL;
		if (bWireframe)
		{
			WireframeMaterialInstance = new FColoredMaterialRenderProxy(
				GEngine->WireframeMaterial ? GEngine->WireframeMaterial->GetRenderProxy() : NULL,
				FLinearColor(0, 0.5f, 1.f)
			);

			Collector.RegisterOneFrameMaterialProxy(WireframeMaterialInstance);
		}

		// Iterate over sections
		for (const FDeformMeshSectionProxy* Section : Sections)
		{
			if (Section != nullptr && Section->bSectionVisible)
			{
				//Get the section's materil, or the wireframe material if we're rendering in wireframe mode
				FMaterialRenderProxy* MaterialProxy = bWireframe ? WireframeMaterialInstance : Section->Material->GetRenderProxy();

				// For each view..
				for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
				{
					//Check if our mesh is visible from this view
					if (VisibilityMap & (1 << ViewIndex))
					{
						const FSceneView* View = Views[ViewIndex];
						// Allocate a mesh batch and get a ref to the first element
						FMeshBatch& Mesh = Collector.AllocateMesh();
						FMeshBatchElement& BatchElement = Mesh.Elements[0];
						//Fill this batch element with the mesh section's render data
						BatchElement.IndexBuffer = &Section->IndexBuffer;
						Mesh.bWireframe = bWireframe;
						Mesh.VertexFactory = &Section->VertexFactory;
						Mesh.MaterialRenderProxy = MaterialProxy;

						//The LocalVertexFactory uses a uniform buffer to pass primitve data like the local to world transform for this frame and for the previous one
						//Most of this data can be fetched using the helper function below
						bool bHasPrecomputedVolumetricLightmap;
						FMatrix PreviousLocalToWorld;
						int32 SingleCaptureIndex;
						bool bOutputVelocity;
						GetScene().GetPrimitiveUniformShaderParameters_RenderThread(GetPrimitiveSceneInfo(), bHasPrecomputedVolumetricLightmap, PreviousLocalToWorld, SingleCaptureIndex, bOutputVelocity);
						//Alloate a temporary primitive uniform buffer, fill it with the data and set it in the batch element
						FDynamicPrimitiveUniformBuffer& DynamicPrimitiveUniformBuffer = Collector.AllocateOneFrameResource<FDynamicPrimitiveUniformBuffer>();
						DynamicPrimitiveUniformBuffer.Set(GetLocalToWorld(), PreviousLocalToWorld, GetBounds(), GetLocalBounds(), true, bHasPrecomputedVolumetricLightmap, DrawsVelocity(), bOutputVelocity);
						BatchElement.PrimitiveUniformBufferResource = &DynamicPrimitiveUniformBuffer.UniformBuffer;
						BatchElement.PrimitiveIdMode = PrimID_DynamicPrimitiveShaderData;

						//Additional data 
						BatchElement.FirstIndex = 0;
						BatchElement.NumPrimitives = Section->IndexBuffer.GetNumIndices() / 3;
						BatchElement.MinVertexIndex = 0;
						BatchElement.MaxVertexIndex = Section->MaxVertexIndex;
						Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
						Mesh.Type = PT_TriangleList;
						Mesh.DepthPriorityGroup = SDPG_World;
						Mesh.bCanApplyViewModeOverrides = false;

						//Add the batch to the collector
						Collector.AddMesh(ViewIndex, Mesh);
					}
				}
			}
		}
	}

	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const
	{
		FPrimitiveViewRelevance Result;
		Result.bDrawRelevance = IsShown(View);
		Result.bShadowRelevance = IsShadowCast(View);
		Result.bDynamicRelevance = true;
		Result.bRenderInMainPass = ShouldRenderInMainPass();
		Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
		Result.bRenderCustomDepth = ShouldRenderCustomDepth();
		Result.bTranslucentSelfShadow = bCastVolumetricTranslucentShadow;
		MaterialRelevance.SetPrimitiveViewRelevance(Result);
		Result.bVelocityRelevance = IsMovable() && Result.bOpaque && Result.bRenderInMainPass;
		return Result;
	}

	virtual bool CanBeOccluded() const override
	{
		return !MaterialRelevance.bDisableDepthTest;
	}

	virtual uint32 GetMemoryFootprint(void) const
	{
		return(sizeof(*this) + GetAllocatedSize());
	}

	uint32 GetAllocatedSize(void) const
	{
		return(FPrimitiveSceneProxy::GetAllocatedSize());
	}

	//Getter to the SRV of the transforms structured buffer
	inline FShaderResourceViewRHIRef& GetDeformTransformsSRV() { return DeformTransformsSRV; }

private:
	/** Array of sections */
	TArray<FDeformMeshSectionProxy*> Sections;

	FMaterialRelevance MaterialRelevance;

	//The render thread array of transforms of all the sections
	//Individual updates of each section's deform transform will just update the entry in this array
	//Before binding the SRV, we update the content of the structured buffer with this updated array
	TArray<FMatrix> DeformTransforms;

	//The structured buffer that will contain all the deform transoform and going to be used as a shader resource
	FBufferRHIRef DeformTransformsSB;

	//The shader resource view of the structured buffer, this is what we bind to the vertex factory shader
	FShaderResourceViewRHIRef DeformTransformsSRV;

	//Whether the structured buffer needs to be updated or not
	bool bDeformTransformsDirty;
};
