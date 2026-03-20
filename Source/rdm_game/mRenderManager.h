// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SceneImageInterceptor.h" // Include your interceptor
#include "mRenderManager.generated.h"

UCLASS()
class RDM_GAME_API AmRenderManager : public AActor
{
    GENERATED_BODY()
    
public:
    // Sets default values for this actor's properties
    AmRenderManager();

protected:
    // Called when the game starts or when spawned
    virtual void BeginPlay() override;

    /** * The shared pointer to our custom render interceptor.
     * Moving this inside the class prevents global scope issues and
     * ensures it is managed by the Actor's lifecycle.
     */
    TSharedPtr<FSceneImageInterceptor> MyInterceptor;

public:
    // Called every frame
    virtual void Tick(float DeltaTime) override;

};


