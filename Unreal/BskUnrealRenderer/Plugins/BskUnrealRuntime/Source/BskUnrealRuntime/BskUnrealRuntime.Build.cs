using UnrealBuildTool;

public class BskUnrealRuntime : ModuleRules
{
    public BskUnrealRuntime(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core", "CoreUObject", "Engine", "InputCore", "Json", "Networking", "Sockets", "ImageWrapper", "RenderCore", "RHI"
        });
    }
}
