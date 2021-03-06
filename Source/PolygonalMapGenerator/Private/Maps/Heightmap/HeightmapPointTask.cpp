// Fill out your copyright notice in the Description page of Project Settings.

#include "PolygonalMapGeneratorPrivatePCH.h"
#include "PolygonalMapHeightmap.h"
#include "Async/Async.h"
#include "Maps/Heightmap/HeightmapPointTask.h"

// The heightmap generator
UPolygonalMapHeightmap* FHeightmapPointGenerator::MapHeightmap = NULL;
UPolygonMap* FHeightmapPointGenerator::MapGraph = NULL;
UBiomeManager* FHeightmapPointGenerator::BiomeManager = NULL;
float FHeightmapPointGenerator::MapScale = 1.0f;
bool FHeightmapPointGenerator::bInterpolateUsingTriangleCenters = true;

// Results of the threads
TArray<FMapData> FHeightmapPointGenerator::HeightmapData = TArray<FMapData>();
TArray<FMapData> FHeightmapPointGenerator::StartingMapDataArray = TArray<FMapData>();

// This is the array of thread completions, used to determine if all threads are done
FGraphEventArray FHeightmapPointGenerator::CompletionEvents = FGraphEventArray();
int32 FHeightmapPointGenerator::CompletedThreads = 0;
int32 FHeightmapPointGenerator::TotalNumberOfThreads = 0;

bool FHeightmapPointGenerator::bShouldLogOnCompletion = true;
float FHeightmapPointGenerator::CompletionPercent = 0.0f;

FIslandGeneratorDelegate FHeightmapPointGenerator::OnAllPointsComplete;

bool FHeightmapPointGenerator::TasksAreComplete()
{
	//Check all thread completion events
	return CompletedThreads == TotalNumberOfThreads;
}

void FHeightmapPointGenerator::GenerateHeightmapPoints(UPolygonalMapHeightmap* HeightmapGenerator, UPolygonMap* Graph, UBiomeManager* BiomeMgr, const FHeightmapCreationData HeightmapProperties, const FIslandGeneratorDelegate OnComplete)
{
	int32 HeightmapSize = HeightmapProperties.Size;

	check(HeightmapSize > 0);
	MapHeightmap = HeightmapGenerator;
	MapGraph = Graph;
	BiomeManager = BiomeMgr;
	OnAllPointsComplete = OnComplete;
	MapScale = (float)MapGraph->GetGraphSize() / (float)HeightmapSize;
	bInterpolateUsingTriangleCenters = HeightmapProperties.bUseTriangleCentersForInterpolation;

	TotalNumberOfThreads = 0;
	CompletedThreads = 0;
	HeightmapData.Empty();

	//StartingMapDataArray = FHeightmapPointGenerator::MapGraph->GetAllMapData();

	bShouldLogOnCompletion = HeightmapSize <= 150;
	if (!bShouldLogOnCompletion)
	{
		UE_LOG(LogWorldGen, Warning, TEXT("You have a large heightmap size (%d)! Your heightmap may take a while (> 15 seconds) to complete. Logging individual completion events will be disabled. You can check the current completion percentage in FHeightmapPointGenerator::CompletionPercent or by calling GetCompletionPercent() on your PolygonalMapHeightmap object (accessible by calling GetHeightmap() on the IslandMapGenerator)"), HeightmapSize);
	}

	EPointSelectionMode pointSelectionMode = EPointSelectionMode::InterpolatedWithPolygonBiome;

	// Add a task for each heightmap pixel
	for (int32 x = 0; x < HeightmapSize; x++)
	{
		for(int32 y = 0; y < HeightmapSize; y++)
		{
			CompletionEvents.Add(TGraphTask<FHeightmapPointTask>::CreateTask(NULL, ENamedThreads::GameThread).ConstructAndDispatchWhenReady(x, y, pointSelectionMode));
			TotalNumberOfThreads++;
		}
	}
}

void FHeightmapPointGenerator::CheckComplete()
{
	if(TasksAreComplete())
	{
		FHeightmapPointGenerator::TotalNumberOfThreads = 0;
		FHeightmapPointGenerator::CompletedThreads = 0;

		UE_LOG(LogWorldGen, Log, TEXT("Heightmap is complete!"));

		// Call the delegate on the game thread
		AsyncTask(ENamedThreads::GameThread, []() {
			OnAllPointsComplete.Execute();
			OnAllPointsComplete.Unbind();
		});
	}
}

FMapData FHeightmapPointTask::MakeMapPoint(FVector2D PixelPosition, UPolygonMap* MapGraph, UBiomeManager* BiomeManager, EPointSelectionMode PointSelectionMode)
{
	FMapData pixelData = FMapData();
	pixelData.Point = PixelPosition * FHeightmapPointGenerator::MapScale;

	FPointInterpolationData pointData = MapGraph->FindInterpolatedDataForPoint(pixelData.Point, FHeightmapPointGenerator::bInterpolateUsingTriangleCenters);
	if (pointData.bTriangleIsValid)
	{
		// The point is valid, populate from the triangle
		pixelData.Elevation = pointData.InterpolatedElevation;
		if (PointSelectionMode == EPointSelectionMode::UsePolygon)
		{
			pixelData.Moisture = pointData.SourceTriangle.CornerData.Moisture;
			pixelData.Tags = pointData.SourceTriangle.CornerData.Tags;
			//pixelData.Biome = pointData.SourceTriangle.CornerData.Biome;
		}
		else
		{
			pixelData.Moisture = pointData.InterpolatedMoisture;
			// TODO: Interpolate tags
			pixelData.Tags = pointData.SourceTriangle.CornerData.Tags;
			/*if (PointSelectionMode == EPointSelectionMode::InterpolatedWithPolygonBiome && pointData.SourceTriangle.CornerData.Biome.IsValid())
			{
				// Grab the biome directly from the CornerData
				pixelData.Biome = pointData.SourceTriangle.CornerData.Biome;
			}
			else
			{
				// Right now, this sometimes causes a crash
				// TODO: Find out why it crashes (maybe due to multithreading?)
				// In the meantime, use EPointSelectionMode::InterpolatedWithPolygonBiome instead
				pixelData.Biome = BiomeManager->DetermineBiome(pixelData);
			}*/
		}
	}
	else
	{
		// If the point is invalid, the default constructor for the MapData struct is
		// sufficient for making an ocean pixel. We just need to set the biome.
		//pixelData.Biome = FGameplayTag::RequestGameplayTag(TEXT("MapData.Biome.Water.Ocean"));
	}
	return pixelData;
}

void FHeightmapPointTask::DoTask(ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent)
{
	FVector2D point = FVector2D(X, Y);

	// Now make the actual map point
	FMapData mapData = MakeMapPoint(point, FHeightmapPointGenerator::MapGraph, FHeightmapPointGenerator::BiomeManager, PointSelectionMode);
	FHeightmapPointGenerator::HeightmapData.Add(mapData);
	FHeightmapPointGenerator::CompletedThreads++;

	float percentComplete = (float)FHeightmapPointGenerator::CompletedThreads / (float)FHeightmapPointGenerator::TotalNumberOfThreads; 
	if (FHeightmapPointGenerator::bShouldLogOnCompletion)
	{
		UE_LOG(LogWorldGen, Log, TEXT("Created pixel at (%d, %d), completing thread %d of %d. Heightmap completion percent: %f percent."), X, Y, FHeightmapPointGenerator::CompletedThreads, FHeightmapPointGenerator::TotalNumberOfThreads, percentComplete);
	}
	if (FHeightmapPointGenerator::CompletedThreads == FHeightmapPointGenerator::TotalNumberOfThreads)
	{
		// If we're all done, check in with the on completion delegate
		FHeightmapPointGenerator::CheckComplete();
	}
}
