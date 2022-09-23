#include "DeformMeshComponent.h"
#include "DeformMeshSceneProxy.h"

///////////////////////////////////////////////////////////////////////
// The Deform Mesh Component Methods' Definitions
///////////////////////////////////////////////////////////////////////
/*
 * Most of ths method below are self explanatory, they make changes to the game thread state and propagate changes to the render thread using the scene proxy
*/
void UDeformMeshComponent::CreateMeshSection(int32 SectionIndex, UStaticMesh* Mesh, const FTransform& Transform)
{
	// Ensure sections array is long enough
	if (SectionIndex >= DeformMeshSections.Num())
	{
		DeformMeshSections.SetNum(SectionIndex + 1, false);
	}

	// Reset this section (in case it already existed)
	FDeformMeshSection& NewSection = DeformMeshSections[SectionIndex];
	NewSection.Reset();

	// Fill in the mesh section with the needed data
	// I'm assuming that the StaticMesh has only one section and I'm only using that
	NewSection.StaticMesh = Mesh;
	NewSection.DeformTransform = Transform.ToMatrixWithScale().GetTransposed();

	//Update the local bound using the bounds of the static mesh that we're adding
	//I'm not taking in consideration the deformation here, if the deformation cause the mesh to go outside its bounds
	NewSection.StaticMesh->CalculateExtendedBounds();
	NewSection.SectionLocalBox += NewSection.StaticMesh->GetBoundingBox();

	//Add this sections' material to the list of the component's materials, with the same index as the section
	SetMaterial(SectionIndex, NewSection.StaticMesh->GetMaterial(0));
	

	UpdateLocalBounds(); // Update overall bounds
	MarkRenderStateDirty(); // New section requires recreating scene proxy
}

/// <summary>
/// Update the Transform Matrix that we use to deform the mesh
/// The update of the state in the game thread is simple, but for the scene proxy update, we need to enqueue a render command
/// </summary>
/// <param name="SectionIndex"> The index for the section that we want to update its DeformTransform </param>
/// <param name="Transform"> The new Transform Matrix </param>
void UDeformMeshComponent::UpdateMeshSectionTransform(int32 SectionIndex, const FTransform& Transform)
{
	if (SectionIndex < DeformMeshSections.Num())
	{
		//Set game thread state
		const FMatrix TransformMatrix = Transform.ToMatrixWithScale().GetTransposed();
		DeformMeshSections[SectionIndex].DeformTransform = TransformMatrix;

		DeformMeshSections[SectionIndex].SectionLocalBox += DeformMeshSections[SectionIndex].StaticMesh->GetBoundingBox().TransformBy(Transform);


		if (SceneProxy)
		{
			// Enqueue command to modify render thread info
			FDeformMeshSceneProxy* DeformMeshSceneProxy = (FDeformMeshSceneProxy*)SceneProxy;
			ENQUEUE_RENDER_COMMAND(FDeformMeshTransformsUpdate)(
				[DeformMeshSceneProxy, SectionIndex, TransformMatrix](FRHICommandListImmediate& RHICmdList)
				{
					DeformMeshSceneProxy->UpdateDeformTransform_RenderThread(SectionIndex, TransformMatrix);
				});
		}
		UpdateLocalBounds();		 // Update overall bounds
		MarkRenderTransformDirty();  // Need to send new bounds to render thread
	}
}

void UDeformMeshComponent::ClearMeshSection(int32 SectionIndex)
{
	if (SectionIndex < DeformMeshSections.Num())
	{
		DeformMeshSections[SectionIndex].Reset();
		UpdateLocalBounds();
		MarkRenderStateDirty();
	}
}

/// <summary>
/// This method is called after we finished updating all the section transforms that we want to update
/// This will update the structured buffer with the new transforms
/// </summary>
void UDeformMeshComponent::FinishTransformsUpdate()
{
	if (SceneProxy)
	{
		// Enqueue command to modify render thread info
		FDeformMeshSceneProxy* DeformMeshSceneProxy = (FDeformMeshSceneProxy*)SceneProxy;
		ENQUEUE_RENDER_COMMAND(FDeformMeshAllTransformsSBUpdate)(
			[DeformMeshSceneProxy](FRHICommandListImmediate& RHICmdList)
			{
				DeformMeshSceneProxy->UpdateDeformTransformsSB_RenderThread();
			});
	}
}

void UDeformMeshComponent::ClearAllMeshSections()
{
	DeformMeshSections.Empty();
	UpdateLocalBounds();
	MarkRenderStateDirty();
}

void UDeformMeshComponent::SetMeshSectionVisible(int32 SectionIndex, bool bNewVisibility)
{
	if (SectionIndex < DeformMeshSections.Num())
	{
		// Set game thread state
		DeformMeshSections[SectionIndex].bSectionVisible = bNewVisibility;

		if (SceneProxy)
		{
			// Enqueue command to modify render thread info
			FDeformMeshSceneProxy* DeformMeshSceneProxy = (FDeformMeshSceneProxy*)SceneProxy;
			ENQUEUE_RENDER_COMMAND(FDeformMeshSectionVisibilityUpdate)(
				[DeformMeshSceneProxy, SectionIndex, bNewVisibility](FRHICommandListImmediate& RHICmdList)
				{
					DeformMeshSceneProxy->SetSectionVisibility_RenderThread(SectionIndex, bNewVisibility);
				});
		}
	}
}

bool UDeformMeshComponent::IsMeshSectionVisible(int32 SectionIndex) const
{
	return (SectionIndex < DeformMeshSections.Num()) ? DeformMeshSections[SectionIndex].bSectionVisible : false;
}

int32 UDeformMeshComponent::GetNumSections() const
{
	return DeformMeshSections.Num();
}


FDeformMeshSection* UDeformMeshComponent::GetDeformMeshSection(int32 SectionIndex)
{
	if (SectionIndex < DeformMeshSections.Num())
	{
		return &DeformMeshSections[SectionIndex];
	}
	else
	{
		return nullptr;
	}
}

void UDeformMeshComponent::SetDeformMeshSection(int32 SectionIndex, const FDeformMeshSection& Section)
{
	// Ensure sections array is long enough
	if (SectionIndex >= DeformMeshSections.Num())
	{
		DeformMeshSections.SetNum(SectionIndex + 1, false);
	}

	DeformMeshSections[SectionIndex] = Section;

	UpdateLocalBounds(); // Update overall bounds
	MarkRenderStateDirty(); // New section requires recreating scene proxy
}

FPrimitiveSceneProxy* UDeformMeshComponent::CreateSceneProxy()
{
	if (!SceneProxy)
		return new FDeformMeshSceneProxy(this);
	else
		return SceneProxy;
}

int32 UDeformMeshComponent::GetNumMaterials() const
{
	return DeformMeshSections.Num();
}


//Use this to update the Bounds by taking in consideration the deform transform
FBoxSphereBounds UDeformMeshComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	FBoxSphereBounds Ret(LocalBounds.TransformBy(LocalToWorld));

	Ret.BoxExtent *= BoundsScale;
	Ret.SphereRadius *= BoundsScale;

	return Ret;
}

void UDeformMeshComponent::UpdateLocalBounds()
{
	FBox LocalBox(ForceInit);

	for (const FDeformMeshSection& Section : DeformMeshSections)
	{
		LocalBox += Section.SectionLocalBox;
	}

	LocalBounds = LocalBox.IsValid ? FBoxSphereBounds(LocalBox) : FBoxSphereBounds(FVector(0, 0, 0), FVector(0, 0, 0), 0); // fallback to reset box sphere bounds

	// Update global bounds
	UpdateBounds();
	// Need to send to render thread
	MarkRenderTransformDirty();
}

