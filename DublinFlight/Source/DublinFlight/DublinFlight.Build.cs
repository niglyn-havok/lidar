// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class DublinFlight : ModuleRules
{
	public DublinFlight(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivateIncludePaths.Add(ModuleDirectory);
	
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "ProceduralMeshComponent", "Niagara", "GeometryCollectionEngine", "Chaos", "FieldSystemEngine" });

		PrivateDependencyModuleNames.AddRange(new string[] { "Json", "RHI", "RenderCore", "SlateCore", "ApplicationCore" });
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(new string[] { "UnrealEd", "PlanarCut", "Voronoi", "MeshDescription", "StaticMeshDescription", "GeometryCore", "AssetRegistry" });
		}

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });
		
		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
