#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "ScreenPass.h"
#include "Containers/Queue.h" // Needed for TQueue
#include "UnrealClient.h"     // Needed for FRenderTarget
#include "Async/Async.h"      // Needed for AsyncTask (Game Thread dispatch)
#include "EngineUtils.h"      // Needed for TActorIterator
#include "Engine/World.h"     // Needed for UWorld
#include "GameFramework/Actor.h"
#include "Components/StaticMeshComponent.h"

/**
 * FSceneImageInterceptor for project: rdm_game
 * A Scene View Extension that captures the rendered scene color
 * and passes it to a custom C++ function.
 */
class FSceneImageInterceptor : public FSceneViewExtensionBase
{
private:
    // Structure to keep track of pending GPU readbacks
    struct FPendingReadback
    {
        FRHIGPUTextureReadback* Readback;
        FIntPoint Extent;
    };

    // Lock-free queue to poll async readbacks safely across multiple frames
    TQueue<FPendingReadback> PendingReadbacks;

public:
    FSceneImageInterceptor(const FAutoRegister& AutoRegister) : FSceneViewExtensionBase(AutoRegister) {}

    // Destructor to clean up memory
    ~FSceneImageInterceptor()
    {
        FPendingReadback Pending;
        while (PendingReadbacks.Dequeue(Pending))
        {
            if (Pending.Readback)
            {
                delete Pending.Readback;
            }
        }
    }

    // We hook into the PostRenderViewFamily_RenderThread.
    // This is called after the scene is rendered but before it's sent to the backbuffer.
    virtual void PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override
    {
        // Safely extract the UWorld pointer from the ViewFamily to pass to our processor
        UWorld* World = InViewFamily.Scene ? InViewFamily.Scene->GetWorld() : nullptr;

        // 1. Process any pending readbacks from PREVIOUS frames
        FPendingReadback Pending;
        while (PendingReadbacks.Peek(Pending))
        {
            // Check if the GPU has finished the async copy operation
            if (Pending.Readback->IsReady())
            {
                // Remove it from the queue since it's ready
                PendingReadbacks.Dequeue(Pending);

                int32 OutRowPitch = 0;

                // Lock the memory to get a C++ pointer to the pixels
                void* Data = Pending.Readback->Lock(OutRowPitch);
                if (Data)
                {
                    // CALL YOUR CUSTOM C++ CODE HERE
                    ProcessImageInCpp(Data, Pending.Extent.X, Pending.Extent.Y, OutRowPitch, World);
                }

                Pending.Readback->Unlock();

                // Cleanup to prevent memory leaks
                delete Pending.Readback;
            }
            else
            {
                // Because frames finish in order, if the oldest readback isn't ready,
                // the newer ones won't be either. Stop checking.
                break;
            }
        }

        // 2. Ensure there's a valid view
        if (InViewFamily.Views.Num() == 0) return;

        const FSceneView& View = *InViewFamily.Views[0];

        // FIX: The compiler errors occurred because FSceneTextureParameters is part of the private
        // Renderer module. To capture the screen in a Game Module, we should instead grab the
        // ViewFamily's final RenderTarget (the backbuffer), which is publicly accessible.
        if (!InViewFamily.RenderTarget) return;

        FRHITexture* RenderTargetTexture = InViewFamily.RenderTarget->GetRenderTargetTexture();
        if (!RenderTargetTexture) return;

        // Register the external RHI texture with the Render Graph Builder
        FRDGTextureRef SceneColor = RegisterExternalTexture(GraphBuilder, RenderTargetTexture, TEXT("CapturedSceneColor"));

        // Fallback: If SceneColor is null, the renderer might be in a state where it's not bound.
        if (!SceneColor) return;

        // 3. Setup an Asynchronous Readback for the CURRENT frame
        FRHIGPUTextureReadback* Readback = new FRHIGPUTextureReadback(TEXT("rdm_game_CaptureReadback"));

        // Use FResolveRect() to copy the entire texture area
        AddEnqueueCopyPass(GraphBuilder, Readback, SceneColor, FResolveRect());

        // 4. Store the readback to poll it in the upcoming frames
        FPendingReadback NewReadback;
        NewReadback.Readback = Readback;
        NewReadback.Extent = SceneColor->Desc.Extent;
        PendingReadbacks.Enqueue(NewReadback);
    }

    /**
     * Custom function that receives the image data.
     */
    static void ProcessImageInCpp(void* RawData, int32 Width, int32 Height, int32 Pitch, UWorld* World)
    {
        // Calculate the time passed from the previous frame
        static double LastTime = FPlatformTime::Seconds();
        double CurrentTime = FPlatformTime::Seconds();
        double DeltaTimeMs = (CurrentTime - LastTime) * 1000.0;
        LastTime = CurrentTime;

        // LOG MESSAGE FOR DEBUGGING
        UE_LOG(LogTemp, Warning, TEXT("rdm_game: Image captured successfully! Resolution: %dx%d. Time since last frame: %.2f [ms]"), Width, Height, DeltaTimeMs);

        // Access the pixel data
        uint8* Pixels = static_cast<uint8*>(RawData);
        if (Pixels)
        {
            // Note: Because we are capturing the final ViewFamily RenderTarget (backbuffer),
            // the format is typically 4 bytes per pixel (BGRA8).
            int32 BytesPerPixel = 4;

            // Create an array to hold the float representation of the image
            // Size is Width * Height * 4 (for R, G, B, A channels)
            TArray<float> FloatImage;
            FloatImage.SetNumUninitialized(Width * Height * 4);

            double RedChannelSum = 0.0; // Variable to store the sum of the Red channel

            for (int32 Y = 0; Y < Height; ++Y)
            {
                uint8* RowPtr = Pixels + (Y * Pitch);
                for (int32 X = 0; X < Width; ++X)
                {
                    int32 PixelOffset = X * BytesPerPixel;
                    int32 FloatOffset = (Y * Width + X) * 4;

                    // BGRA8 to RGBA float conversion (normalized 0.0f to 1.0f)
                    float RedValue = static_cast<float>(RowPtr[PixelOffset + 2]) / 255.0f;

                    FloatImage[FloatOffset + 0] = RedValue; // Red
                    FloatImage[FloatOffset + 1] = static_cast<float>(RowPtr[PixelOffset + 1]) / 255.0f; // Green
                    FloatImage[FloatOffset + 2] = static_cast<float>(RowPtr[PixelOffset + 0]) / 255.0f; // Blue
                    FloatImage[FloatOffset + 3] = static_cast<float>(RowPtr[PixelOffset + 3]) / 255.0f; // Alpha

                    RedChannelSum += RedValue; // Add to the sum
                }
            }

            // Print the calculated sum to the console
            UE_LOG(LogTemp, Warning, TEXT("rdm_game: Total sum of Red channel pixels: %f"), RedChannelSum);

            // FloatImage now contains the full image as floats!
            // Example: Accessing center pixel float values
            int32 CenterX = Width / 2;
            int32 CenterY = Height / 2;
            int32 CenterFloatOffset = (CenterY * Width + CenterX) * 4;

            // UE_LOG(LogTemp, Warning, TEXT("Center Pixel Float RGBA: %f, %f, %f, %f"),
            //     FloatImage[CenterFloatOffset], FloatImage[CenterFloatOffset+1],
            //     FloatImage[CenterFloatOffset+2], FloatImage[CenterFloatOffset+3]);
        }

        // --- COMPONENT MODIFICATION LOGIC ---
        // Since we are currently executing on the Render Thread, we MUST dispatch
        // actor/component modifications back to the Game Thread to prevent crashes.
        if (World)
        {
            // Use a Weak Pointer to ensure the World hasn't been destroyed before the task runs
            TWeakObjectPtr<UWorld> WeakWorld(World);

            AsyncTask(ENamedThreads::GameThread, [WeakWorld]()
            {
                UWorld* GameWorld = WeakWorld.Get();
                if (!GameWorld || !GameWorld->IsGameWorld()) return;

                // Search through all actors in the level
                for (TActorIterator<AActor> It(GameWorld); It; ++It)
                {
                    AActor* Actor = *It;
                    if (!Actor) continue;

                    // Grab the Actor Name, and (if in Editor) the Label
                    FString ActorName = Actor->GetName();
                    FString ActorLabel = ActorName;
#if WITH_EDITOR
                    ActorLabel = Actor->GetActorLabel();
#endif

                    // Check if this actor is the "arbol"
                    if (ActorName.Contains(TEXT("arbol")) || ActorLabel.Contains(TEXT("arbol")) || Actor->Tags.Contains(FName("arbol")))
                    {
                        // Look for "StaticMeshComponent0" in this actor
                        TArray<UStaticMeshComponent*> MeshComps;
                        Actor->GetComponents<UStaticMeshComponent>(MeshComps);

                        for (UStaticMeshComponent* MeshComp : MeshComps)
                        {
                            if (MeshComp && MeshComp->GetName().Contains(TEXT("StaticMeshComponent0")))
                            {
                                // Randomly change its X and Y coordinates (jumping between -10 and 10 units)
                                FVector CurrentLoc = MeshComp->GetComponentLocation();
                                CurrentLoc.X += FMath::RandRange(-10.0f, 10.0f);
                                CurrentLoc.Y += FMath::RandRange(-10.0f, 10.0f);

                                MeshComp->SetWorldLocation(CurrentLoc);

                                // We found and modified the component, no need to keep searching
                                break;
                            }
                        }
                    }

                    if (ActorName.Contains(TEXT("rabbit_in_a_cup")) || ActorLabel.Contains(TEXT("rabbit_in_a_cup"))
                        || Actor->Tags.Contains(FName("rabbit_in_a_cup")))
                    {
                        // Look for "StaticMeshComponent0" in this actor
                        TArray<UStaticMeshComponent*> MeshComps;
                        Actor->GetComponents<UStaticMeshComponent>(MeshComps);

                        for (UStaticMeshComponent* MeshComp : MeshComps)
                        {
                            if (MeshComp && MeshComp->GetName().Contains(TEXT("StaticMeshComponent0")))
                            {
                                // Randomly change its X and Y coordinates (jumping between -10 and 10 units)
                                FVector CurrentLoc = MeshComp->GetComponentLocation();
                                CurrentLoc.X += FMath::RandRange(-5.0f, 5.0f);
                                CurrentLoc.Y += FMath::RandRange(-5.0f, 5.0f);

                                MeshComp->SetWorldLocation(CurrentLoc);

                                // We found and modified the component, no need to keep searching
                                break;
                            }
                        }
                    }

                        // Check if this actor is the "Arna_ExportedFace" Skeletal Mesh Actor
                    else if (ActorName.Contains(TEXT("Arna_ExportedFace")) || ActorLabel.Contains(TEXT("Arna_ExportedFace")) || Actor->Tags.Contains(FName("Arna_ExportedFace")))
                    {
                        // Randomly change the actor's X and Y coordinates (jumping between -10 and 10 units)
                        FVector CurrentLoc = Actor->GetActorLocation();
                        CurrentLoc.X += FMath::RandRange(-10.0f, 10.0f);
                        CurrentLoc.Y += FMath::RandRange(-10.0f, 10.0f);

                        Actor->SetActorLocation(CurrentLoc);
                    }
                }
            });
        }
    }

    // Boilerplate overrides required by the Engine
    virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
    virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
    virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}
};