// Fill out your copyright notice in the Description page of Project Settings.

#include "mRenderManager.h"

// Sets default values
AmRenderManager::AmRenderManager()
{
    // Set this actor to call Tick() every frame.
    PrimaryActorTick.bCanEverTick = true;
}

// Called when the game starts or when spawned
void AmRenderManager::BeginPlay()
{
    Super::BeginPlay();

    // This activates the Render Graph hook.
    // We assign it to our member variable 'MyInterceptor' declared in the header.
    // NOTE: This actor MUST be placed in the Level for this code to run.
    MyInterceptor = FSceneViewExtensions::NewExtension<FSceneImageInterceptor>();
    
    if (MyInterceptor.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("RenderManager: Image Interceptor has been successfully registered."));
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("RenderManager: Failed to register Image Interceptor!"));
    }
}

// Called every frame
void AmRenderManager::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);
}


