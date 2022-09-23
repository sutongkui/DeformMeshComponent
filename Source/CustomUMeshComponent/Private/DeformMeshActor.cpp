// Fill out your copyright notice in the Description page of Project Settings.


#include "DeformMeshActor.h"
#include "EngineUtils.h"


// Sets default values
ADeformMeshActor::ADeformMeshActor()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;
	DeformMeshComp = CreateDefaultSubobject<UDeformMeshComponent>(TEXT("Deform Mesh Component"));
	RootComponent = DeformMeshComp;
	Controller = CreateDefaultSubobject<AActor>(TEXT("Controller"));

}

void ADeformMeshActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	if (TestMesh == nullptr)
	{
		DeformMeshComp->ClearAllMeshSections();
	}
	else
	{
		const auto Transform = Controller->GetTransform();
		//We create a new deform mesh section using the static mesh and the transform of the actor
		DeformMeshComp->CreateMeshSection(0, TestMesh, Transform);
	}
}



