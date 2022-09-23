// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DeformMeshComponent.h"
#include "Components/SceneComponent.h"
#include "DeformMeshActor.generated.h"


UCLASS()
class DEFORMMESH_API ADeformMeshActor : public AActor
{
	GENERATED_BODY()

/*
 * This is a simple actor that has a DeformMeshComponent
 * It uses the DeformMeshComponent API to create mesh sections and update their deform transforms
*/
public:
	
	// Sets default values for this actor's properties
	ADeformMeshActor();
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;

public:	
	UPROPERTY(EditAnywhere)
		UDeformMeshComponent* DeformMeshComp;

	//We're creating a mesh section from this static mesh
	UPROPERTY(EditAnywhere)
		UStaticMesh* TestMesh;

	// We're using the transform of this actor as a deform transform
	UPROPERTY(EditAnywhere)
		AActor* Controller;

};
